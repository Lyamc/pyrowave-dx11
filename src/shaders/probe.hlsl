// probe.hlsl -- minimal compute shader to prove the fxc build pipeline
// cs_5_0, /O3 /WX /Ges /Gis
// This shader does nothing but write a constant to a UAV.

#include "common.hlsli"

cbuffer PushConstants : register(b0) {
    uint dispatch_id;
    uint padding[3];
};

RWTexture2D<float4> outputTexture : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    // Write a constant value -- proves the UAV path works.
    outputTexture[gid.xy] = float4(1.0f, 0.0f, 0.0f, 1.0f);
}
