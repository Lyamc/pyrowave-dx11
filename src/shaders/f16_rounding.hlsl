// OQ-3 f16 rounding test: D3D11 compute shader
// Tests f32tof16 rounding mode, denormal handling, overflow
// cs_5_0 compatible

struct InputData
{
    float value;
};

struct OutputData
{
    uint f32_bits;
    uint16_t f16_result;
    uint16_t reserved;
};

// cbuffer for test count
cbuffer TestConfig : register(b0)
{
    uint test_count;
};

// UAVs
RWStructuredBuffer<InputData> g_input : register(u0);
RWStructuredBuffer<OutputData> g_output : register(u1);

[numthreads(64, 1, 1)]
void main(uint3 threadID : SV_DispatchThreadID)
{
    uint idx = threadID.x;
    if (idx >= test_count) return;

    InputData input = g_input[idx];
    OutputData output;

    output.f32_bits = asuint(input.value);
    output.f16_result = f32tof16(input.value);
    output.reserved = 0;

    g_output[idx] = output;
}
