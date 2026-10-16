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

    // Barrier
    GroupMemoryBarrierWithGroupSync();

    // ------------------------------------------------------------------
    // Windowed Hillis-Steele scan (arf:22-36)
    // IMPORTANT: loop bound is gl_WorkGroupSize.x / 2 = 256, NOT 512
    // This is a bug in the reference that must be reproduced (OQ-5)
    // ------------------------------------------------------------------
    for (uint step = 1u; step < 256u; step *= 2u) {
        GroupMemoryBarrierWithGroupSync();

        uint up = 0u;
        if (gi >= step) {
            up = gs_scan[gi - step];
        }

        GroupMemoryBarrierWithGroupSync();

        v.w += up;
        gs_scan[gi] = v.w;
    }

    // ------------------------------------------------------------------
    // Store result (arf:38)
    // ------------------------------------------------------------------
    u0.Store4(int(offset), v);
}
