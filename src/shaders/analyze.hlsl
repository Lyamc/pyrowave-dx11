// analyze.hlsl -- Rate-control analysis: sum per-block rate/distortion, emit RDO operations
// Port of R/shaders/analyze_rate_control.comp to cs_5_0
//
// Spec: docs/stages/03_analyze_rate_control.md
// Source: R/shaders/analyze_rate_control.comp (ar:N)
//
// Dispatch: ceil(w/32) x ceil(h/32) groups of 64 threads
// Each workgroup processes one 32x32 block (4x4 8x8 blocks = 16 sub-blocks)

#include "common.hlsli"

// ------------------------------------------------------------------
// Push constants (matches AnalyzeRateControlPushData, 48 bytes)
// ------------------------------------------------------------------
cbuffer PushConstants : register(b0) {
    int2  resolution;              // @0
    int2  resolution_8x8_blocks;   // @8
    int   block_offset_8x8;        // @16
    int   block_stride_8x8;        // @20
    int   block_offset_32x32;      // @24
    int   block_stride_32x32;      // @28
    uint  total_wg_count;          // @32
    uint  num_blocks_aligned;      // @36
    uint  block_index_shamt;       // @40
};

// ------------------------------------------------------------------
// Resources
// ------------------------------------------------------------------
// u0: RWByteAddressBuffer = bucket buffer
RWByteAddressBuffer u0 : register(u0);

// t0: ByteAddressBuffer = block stats from stage 2 (64 bytes per block)
ByteAddressBuffer t0 : register(t0);

// ------------------------------------------------------------------
// Groupshared memory
// ------------------------------------------------------------------
groupshared uint gs_cost64[64];       // per-thread cost during the 16-lane xor butterfly
groupshared float gs_dist64[64];      // per-thread distortion during the butterfly
groupshared uint gs_rate_cost[16];    // rate for each i=0..15
groupshared float gs_distortion[16];  // distortion for each i=0..15
groupshared uint gs_scan_val[64];     // Hillis-Steele scan (all 64 threads)

// ------------------------------------------------------------------
// Helper: distortion to bucket index (ar:73-84)
// ------------------------------------------------------------------
uint distortion_to_bucket(float d, float cost, float d_base, float cost_base)
{
    if (cost == cost_base)
        return 0u;

    // FS-A1: log2 and division are float-sensitive
    float index = 60.0f + 2.0f * log2(max(d - d_base, 0.0f) / (cost_base - cost));
    return uint(max(index + 0.5f, 0.0f));
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    // ------------------------------------------------------------------
    // Thread mapping (ar:149-157)
    // ------------------------------------------------------------------
    uint lb = gi & 15u; // 0..15, local 8x8 block within 32x32
    uint lx = lb & 3u;
    uint ly = lb >> 2u;
    uint g = gi >> 4u; // wave 0..3; each owns i = g, g+4, g+8, g+12

    // 32x32 block index
    int2 block32x32_idx = int2(int(gid.x), int(gid.y));
    int2 block8x8_idx = 4 * block32x32_idx + int2(int(lx), int(ly));

    // ------------------------------------------------------------------
    // Read block stats (ar:159-165)
    // ------------------------------------------------------------------
    bool in8 = (block8x8_idx.x < resolution_8x8_blocks.x) && (block8x8_idx.y < resolution_8x8_blocks.y);
    int b8 = block_offset_8x8 + block_stride_8x8 * block8x8_idx.y + block8x8_idx.x;

    uint num_planes = 0u;
    uint stats_base = 0u;
    if (in8) {
        stats_base = uint(b8) * 64u;
        num_planes = t0.Load(stats_base + 0);
    }

    if (gi < 16u) {
        gs_rate_cost[gi] = 0u;
        gs_distortion[gi] = 0.0f;
    }
    GroupMemoryBarrierWithGroupSync();

    // Spec §4: four waves, i ∈ {g, g+4, g+8, g+12}. Reduce over the 16
    // lanes of this wave (xor 1,2,4,8). One 16-pass loop on threads 0..15
    // was 128 barriers and 720p TDRed into empty packets on the HD 6670.
    [loop]
    for (uint k = 0u; k < 4u; k++) {
        uint i = g + k * 4u;
        float dist = 0.0f;
        uint cost = 0u;
        if (in8) {
            uint ei = min(i, num_planes);
            uint e = t0.Load(int(stats_base + 4u + ei * 4u));
            dist = f16tof32(e & 0xffffu);
            cost = e >> 16u;
        }
        if (cost != 0u)
            cost += 24u;

        uint cost_r = cost;
        float dist_r = dist;
        [unroll]
        for (uint s = 1u; s < 16u; s *= 2u) {
            gs_cost64[gi] = cost_r;
            gs_dist64[gi] = dist_r;
            GroupMemoryBarrierWithGroupSync();
            uint peer = (gi & ~15u) | ((lb ^ s) & 15u);
            cost_r += gs_cost64[peer];
            dist_r += gs_dist64[peer];
            GroupMemoryBarrierWithGroupSync();
        }

        if (lb == 0u) {
            if (cost_r != 0u)
                cost_r += 64u;
            gs_rate_cost[i] = (cost_r + 31u) >> 5u;
            gs_distortion[i] = dist_r;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // ------------------------------------------------------------------
    // Hillis-Steele inclusive max scan (all 64 threads participate)
    // Only gi < 16 have meaningful data; others have 0
    // The scan must run on all threads to avoid X3663
    // ------------------------------------------------------------------
    // Initialize scan array: threads 0..15 have bucket_idx, others have 0
    uint scan_val = 0u;
    if (gi < 16u) {
        float cost_f = float(gs_rate_cost[gi]);
        float dist_f = gs_distortion[gi];

        // Bucket index (ar:115-118)
        uint bucket_idx = distortion_to_bucket(dist_f, cost_f, gs_distortion[0], float(gs_rate_cost[0]));
        if (gi == 0u)
            bucket_idx = 0u;

        // Inclusive max: v = min(v, 112 + lane)
        scan_val = min(bucket_idx, 112u + gi);
    }
    gs_scan_val[gi] = scan_val;
    GroupMemoryBarrierWithGroupSync();

    // Hillis-Steele steps: 1, 2, 4, 8 (all 64 threads)
    for (uint s = 1u; s < 16u; s *= 2u) {
        uint up = (gi >= s) ? gs_scan_val[gi - s] + s : 0u;
        GroupMemoryBarrierWithGroupSync();
        scan_val = max(scan_val, up);
        gs_scan_val[gi] = scan_val;
        GroupMemoryBarrierWithGroupSync();
    }

    // ------------------------------------------------------------------
    // Emit RDO operations (all 64 threads, but only gi < 16 produce output)
    // ------------------------------------------------------------------
    if (gi < 16u) {
        uint inclusive_bucket = min(scan_val, 127u);

        // Emit operations (ar:129-147)
        if (gi == 0u) {
            // consumed_payload += rate[0] (ar:129-130)
            u0.InterlockedAdd(4u, gs_rate_cost[0]);
        }
        else {
            // saving = rate[gi-1] - rate[gi] (ar:133). Unsigned wrap here
            // poisons the prefix sum and leaves later 32x32 blocks at quant 0.
            uint r_prev = gs_rate_cost[gi - 1u];
            uint r_cur = gs_rate_cost[gi];
            uint saving = (r_prev > r_cur) ? (r_prev - r_cur) : 0u;

            if (saving != 0u) {
                // block_index (ar:136-139)
                int block_index = block_offset_32x32 + int(gid.y) * block_stride_32x32 + int(gid.x);
                uint subdivision = uint(block_index) >> block_index_shamt;

                // total_savings[inclusive_bucket * 16 + subdivision] += saving (ar:141-142)
                uint savings_offset = 64u + (inclusive_bucket * 16u + subdivision) * 4u;
                u0.InterlockedAdd(savings_offset, saving);

                // rdo_operations[block_index + inclusive_bucket * num_blocks_aligned] (ar:143-144)
                uint rdo_idx = uint(block_index) + inclusive_bucket * num_blocks_aligned;
                uint rdo_offset = 8256u + rdo_idx * 8u;
                uint block_offset_saving = uint(block_index) | (saving << 16u);
                u0.Store(int(rdo_offset), int(gi));
                u0.Store(int(rdo_offset + 4), int(block_offset_saving));
            }
        }
    }
}
