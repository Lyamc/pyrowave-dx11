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

// u1: quant_data, one int per 32x32 block. Byte-addressed so FL 11_0
// InterlockedMax is the same path as the payload counter.
RWByteAddressBuffer u1 : register(u1);

// ------------------------------------------------------------------
// Groupshared memory (must be at global scope in HLSL)
// ------------------------------------------------------------------
groupshared uint gs_scan[64];

// ------------------------------------------------------------------
// Main
// ------------------------------------------------------------------
[numthreads(64, 1, 1)]
void main(uint3 grp : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    // One workgroup per (bucket, subdivision). Vulkan dispatches 128*16 = 2048.
    uint x = grp.x;

    int consumed_payload = int(t0.Load(4));
    int required_savings = consumed_payload - int(target_payload_size);

    int prev_bucket_total = 0;
    bool bucket_empty = false;
    if (x > 0u) {
        prev_bucket_total = int(t0.Load(64u + (x - 1u) * 4u));
        bucket_empty = (prev_bucket_total == int(t0.Load(64u + x * 4u)));
        required_savings -= prev_bucket_total;
    } else {
        bucket_empty = (int(t0.Load(64u)) == 0);
    }

    bool active = !bucket_empty && (required_savings > 0);
    uint num_blocks = num_blocks_per_subdivision;
    uint total_saved = 0u;

    for (uint i = 0u; i < num_blocks; i += 64u) {
        int quant = 0;
        uint saving = 0u;
        uint block_offset = 0u;
        if (active && (i + gi) < num_blocks) {
            uint rdo_idx = x * num_blocks + i + gi;
            uint rdo_base = 8256u + rdo_idx * 8u;
            quant = int(t0.Load(int(rdo_base)));
            uint bos = t0.Load(int(rdo_base + 4));
            saving = (bos >> 16u) & 0xffffu;
            block_offset = bos & 0xffffu;
        }

        gs_scan[gi] = saving;
        GroupMemoryBarrierWithGroupSync();
        for (uint step = 1u; step < 64u; step *= 2u) {
            uint addend = (gi >= step) ? gs_scan[gi - step] : 0u;
            GroupMemoryBarrierWithGroupSync();
            gs_scan[gi] += addend;
            GroupMemoryBarrierWithGroupSync();
        }
        uint incl = gs_scan[gi];

        bool should_apply = active && (total_saved + incl - saving) < uint(required_savings) && saving != 0u;
        if (should_apply) {
            u1.InterlockedMax(block_offset * 4u, uint(quant));
        }

        GroupMemoryBarrierWithGroupSync();
        total_saved += gs_scan[63u];
    }
}
