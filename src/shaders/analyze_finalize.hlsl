// analyze_finalize.hlsl -- Windowed prefix sum of total_savings_per_bucket
// Port of R/shaders/analyze_rate_control_finalize.comp to cs_5_0
//
// Spec: docs/stages/03_analyze_rate_control.md §4-5
// Source: R/shaders/analyze_rate_control_finalize.comp (arf:N)
//
// Dispatch: 1 group of 512 threads
// Windowed scan: steps 1..128 (NOT 1..512) — reproduces upstream bug (OQ-5)

#include "common.hlsli"

// ------------------------------------------------------------------
// Resources
// ------------------------------------------------------------------
// u0: RWByteAddressBuffer = bucket buffer, total_savings at offset 64
// Array of uvec4 (16 bytes each), 128*16/4 = 512 entries
RWByteAddressBuffer u0 : register(u0);

// ------------------------------------------------------------------
// Groupshared memory
// ------------------------------------------------------------------
groupshared uint gs_scan[512];

[numthreads(512, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    // ------------------------------------------------------------------
    // Load uvec4 and do intra-vector prefix sum (arf:14-19)
    // ------------------------------------------------------------------
    uint offset = 64u + uint(gi) * 16u; // 16 bytes per uvec4
    uint4 v = u0.Load4(int(offset));    // v = (s0, s1, s2, s3)

    // Intra-vector prefix sum: v.y+=v.x, v.z+=v.y, v.w+=v.z
    v.y += v.x;
    v.z += v.y;
    v.w += v.z;

    gs_scan[gi] = v.w;
    GroupMemoryBarrierWithGroupSync();

    // Inclusive scan of each uvec4's last component, then add that exclusive
    // prefix to all four lanes. Adding `up` into v on every Hillis-Steele
    // step inflated the prefix and left later 32x32 blocks at quant 0.
    for (uint step = 1u; step < 512u; step *= 2u) {
        uint addend = (gi >= step) ? gs_scan[gi - step] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_scan[gi] += addend;
        GroupMemoryBarrierWithGroupSync();
    }

    uint excl = (gi == 0u) ? 0u : gs_scan[gi - 1u];
    v += excl;

    u0.Store4(int(offset), v);
}
