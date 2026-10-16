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
groupshared uint gs_rate_cost[16];    // rate for each i=0..15
groupshared float gs_distortion[16];  // distortion for each i=0..15
groupshared uint gs_scan_val[64];     // Hillis-Steele scan (all 64 threads)

// ------------------------------------------------------------------
// Helper: f16 bits to f32 (ar: errors buffer layout)
// ------------------------------------------------------------------
float f16_to_f32(uint f16_bits)
{
    if (f16_bits == 0u) return 0.0f;
    uint sign = (f16_bits >> 15) & 1u;
    uint exp = (f16_bits >> 10) & 0x1fu;
    uint frac = (f16_bits >> 0) & 0x1ffu;
    if (exp == 0u) {
        // Subnormal -> zero
        return asfloat(sign << 31u);
    }
    // Normal
    return asfloat((sign << 31u) | ((exp + 112u) << 23u) | (frac << 13u));
}

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
void main(uint3 gid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    // ------------------------------------------------------------------
    // Thread mapping (ar:149-157)
    // ------------------------------------------------------------------
    uint lb = gi & 15u; // 0..15, local 8x8 block within 32x32
    uint lx = lb & 3u;
    uint ly = lb >> 2u;

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

    // ------------------------------------------------------------------
    // Compute rate and distortion for each i=0..15
    // Each thread handles one i value: thread t handles i=t
    // We unroll the 4-iteration loop from the GLSL (ar:167-214)
    // ------------------------------------------------------------------
    // Threads 0,4,8,12 compute i=0; threads 1,5,9,13 compute i=1; etc.
    // Then we reduce over the 4 threads for each i.

    // Per-thread: my_i = gi % 16, my_group = gi / 16
    // Actually: each thread t computes for i=t, then threads {t, t+16, t+32, t+48} reduce
    // But we have 64 threads and 16 i-values, so 4 threads per i-value.

    // Simpler: each thread computes its own (dist, cost) for i=gi%16,
    // then reduces with the 3 other threads handling the same i.

    uint my_i = gi % 16u;
    uint my_offset = gi / 16u; // 0..3, which of the 4 threads for this i

    float my_dist = 0.0f;
    uint my_cost = 0u;

    if (in8) {
        uint err_offset = stats_base + 4u + my_i * 8u;
        if (my_i < num_planes) {
            uint e = t0.Load(int(err_offset));
            my_dist = f16_to_f32(e & 0xffffu);
            my_cost = e >> 16u;
        }
    }

    if (my_cost != 0u)
        my_cost += 24u;

    // Store for reduction
    gs_rate_cost[lb] = my_cost;
    gs_distortion[lb] = my_dist;

    // Barrier: all 64 threads have written
    GroupMemoryBarrierWithGroupSync();

    // ------------------------------------------------------------------
    // Reduce over 4 threads per i-value (ar:184-214)
    // XOR butterfly: 1, 2 (only need 2 steps for 4 threads)
    // Each thread t reduces with t^1, t^2
    // ------------------------------------------------------------------
    uint reduce_idx = gi % 16u;
    uint reduce_base = gi - reduce_idx; // 0, 16, 32, or 48

    uint red_cost = gs_rate_cost[reduce_idx];
    float red_dist = gs_distortion[reduce_idx];

    // XOR butterfly for 4 threads: steps 1, 2
    if ((gi & 1u) != 0u) {
        uint peer = reduce_base; // gi ^ 1
        red_cost += gs_rate_cost[peer % 16u];
        red_dist += gs_distortion[peer % 16u];
    }
    GroupMemoryBarrierWithGroupSync();

    if ((gi & 2u) != 0u) {
        uint peer = reduce_base + (gi & 1u); // gi ^ 2
        red_cost += gs_rate_cost[peer % 16u];
        red_dist += gs_distortion[peer % 16u];
    }
    GroupMemoryBarrierWithGroupSync();

    // Only thread 0 of each i-group (gi % 16 == 0) writes the result
    if (reduce_idx == 0u) {
        // cost += 64 if non-zero (ar:208-209)
        if (red_cost != 0u)
            red_cost += 64u;

        // rate = (cost + 31) >> 5 (ar:212)
        gs_rate_cost[my_i] = (red_cost + 31u) >> 5u;
        gs_distortion[my_i] = red_dist;
    }

    // Barrier: all 16 rate/dist values ready
    GroupMemoryBarrierWithGroupSync();

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
        uint inclusive_bucket = scan_val;

        // Emit operations (ar:129-147)
        if (gi == 0u) {
            // consumed_payload += rate[0] (ar:129-130)
            u0.InterlockedAdd(4u, gs_rate_cost[0]);
        }
        else {
            // saving = rate[gi-1] - rate[gi] (ar:133)
            uint saving = gs_rate_cost[gi - 1u] - gs_rate_cost[gi];

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
