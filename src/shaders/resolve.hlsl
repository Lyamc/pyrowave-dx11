// resolve.hlsl -- Resolve rate control: select final quant per block
// Port of R/shaders/resolve_rate_control.comp to cs_5_0
//
// Spec: docs/stages/04_resolve_rate_control.md
// Source: R/shaders/resolve_rate_control.comp

#include "common.hlsli"

// ------------------------------------------------------------------
// Push constants (matches Registers, 8 bytes)
// ------------------------------------------------------------------
cbuffer PushConstants : register(b0) {
    uint target_payload_size;         // @0
    uint num_blocks_per_subdivision;  // @4 = per_sub
    uint _pad[2];                     // @8
};

// ------------------------------------------------------------------
// Resources
// ------------------------------------------------------------------
// t0: ByteAddressBuffer = bucket buffer (read-only)
ByteAddressBuffer t0 : register(t0);

// u1: RWStructuredBuffer<int> = quant_data (output: max quant per block)
RWStructuredBuffer<int> u1 : register(u1);

// ------------------------------------------------------------------
// Groupshared memory (must be at global scope in HLSL)
// ------------------------------------------------------------------
// Fixed-size: max per_sub is 4096 (8K video). 4096*4 = 16 KB, within 32 KB limit.
groupshared int gs_quant_max[4096];
groupshared uint gs_saving[64];
groupshared uint gs_scan[64];

// ------------------------------------------------------------------
// Main
// ------------------------------------------------------------------
[numthreads(64, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    // ------------------------------------------------------------------
    // Bucket emptiness check (ar:44-56)
    // All threads reach the barrier below, so no early returns before it.
    // Use a flag to gate whether work happens.
    // ------------------------------------------------------------------
    uint sub_idx = gid.x;

    int consumed_payload = int(t0.Load(4));
    int required_savings = consumed_payload - int(target_payload_size);

    int prev_bucket_total = 0;
    bool bucket_empty = false;

    if (sub_idx > 0u) {
        prev_bucket_total = int(t0.Load(64u + (sub_idx - 1u) * 4u));
        bucket_empty = (prev_bucket_total == int(t0.Load(64u + sub_idx * 4u)));
    } else {
        bucket_empty = (int(t0.Load(64u)) == 0);
    }

    // All threads reach the barrier — no early returns before it
    bool active = !bucket_empty && (required_savings > 0);

    if (active) {
        required_savings -= prev_bucket_total;
    }

    // ------------------------------------------------------------------
    // Scan RDO operations for this subdivision (ar:60-78)
    // Each thread processes one block per loop iteration.
    //
    // cs_5_0 does not support InterlockedMax on RWStructuredBuffer.
    // We accumulate max per block in groupshared and write back with Store.
    // gs_quant_max is 4096 elements (handles up to 8K video, 16 KB).
    // ------------------------------------------------------------------
    uint num_blocks = num_blocks_per_subdivision;

    uint total_saved = 0u;

    // Loop over blocks in steps of 64 (one per thread per iteration)
    // All threads participate in every iteration (no early exit from loop)
    for (uint i = 0u; i < num_blocks; i += 64u) {
        // Each thread loads its block's RDOperation
        uint rdo_idx = sub_idx * num_blocks + i + gi;
        uint rdo_base = 8256u + rdo_idx * 8u;
        int quant = 0;
        uint block_offset_saving = 0u;
        uint saving = 0u;
        uint block_offset = 0u;

        if (active && rdo_idx < sub_idx * num_blocks + num_blocks) {
            quant = int(t0.Load(int(rdo_base)));
            block_offset_saving = t0.Load(int(rdo_base + 4));
            saving = (block_offset_saving >> 16u) & 0xffffu;
            block_offset = block_offset_saving & 0xffffu;
        }

        // Store saving in groupshared for scan
        gs_saving[gi] = saving;
        GroupMemoryBarrierWithGroupSync();

        // Inclusive sum scan (Hillis-Steele, 6 steps for 64 threads)
        gs_scan[gi] = saving;
        for (uint offset = 1u; offset < 64u; offset *= 2u) {
            uint up = (gi >= offset) ? gs_scan[gi - offset] + offset : 0u;
            GroupMemoryBarrierWithGroupSync();
            uint val = gs_scan[gi];
            val = max(val, up);
            gs_scan[gi] = val;
            GroupMemoryBarrierWithGroupSync();
        }

        // inclusive scan value for this thread
        uint incl = gs_scan[gi];

        // should_apply: total_saved + incl - saving < required_savings
        bool should_apply = active && (total_saved + incl - saving) < uint(required_savings);

        if (should_apply && saving != 0u) {
            // Accumulate max in groupshared (block_offset < 4096 for 8K)
            int old = gs_quant_max[int(block_offset)];
            if (quant > old) {
                gs_quant_max[int(block_offset)] = quant;
            }
        }

        // total_saved += scan of thread 63 (the total for this iteration)
        GroupMemoryBarrierWithGroupSync();
        total_saved += gs_scan[63u];
    }

    // Barrier then write back max values to quant buffer
    GroupMemoryBarrierWithGroupSync();

    // Each thread writes its assigned block's max
    if (active && int(gi) < int(num_blocks)) {
        int q = gs_quant_max[int(gi)];
        if (q >= 0) {
            u1[int(gi)] = q;
        }
    }
}
