// D3D11 (cs_5_0) port of PyroWave shaders/dwt.comp @ a0d2e71 (PRECISION=1, FP16=0 variant):
// FP32 math, FP16-packed (f32tof16) groupshared storage, CDF 9/7 lifting, 32x32 blocks + 4 texel apron.
// The GLSL only uses subgroups to derive local_index (gl_SubgroupID*size+invocation), which is
// exactly SV_GroupIndex here; all cross-lane exchange already goes through shared memory + barrier().
cbuffer Push : register(b0)
{
    int2 resolution;
    float2 inv_resolution;
    int2 aligned_resolution;
    uint dc_shift;
    float range_scale;
};

Texture2DArray<float> uTexture : register(t0);
SamplerState uSampler : register(s0); // MIRROR addressing == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
RWTexture2DArray<float> uOutput : register(u0);

#define APRON 4
#define APRON_HALF 2
#define BLOCK_SIZE 32
#define BLOCK_SIZE_HALF 16

static const float ALPHA = -1.586134342059924;
static const float BETA = -0.052980118572961;
static const float GAMMA = 0.882911075530934;
static const float DELTA = 0.443506852043971;
static const float K = 1.230174104914001;
static const float inv_K = 1.0 / 1.230174104914001;

groupshared uint shared_block[(BLOCK_SIZE + 2 * APRON) / 2][(BLOCK_SIZE + 2 * APRON) + 1];
static uint local_index;

float2 load_shared(int y, int x)
{
    uint v = shared_block[y][x];
    return float2(f16tof32(v & 0xffffu), f16tof32(v >> 16));
}

void store_shared(int y, int x, float2 v)
{
    shared_block[y][x] = f32tof16(v.x) | (f32tof16(v.y) << 16);
}

int2 unswizzle8x8(uint index)
{
    uint y = index & 1u;
    uint x = (index >> 1) & 3u;
    y |= ((index >> 3) & 3u) << 1;
    x |= ((index >> 5) & 1u) << 2;
    return int2(x, y);
}

float2 generate_mirror_uv(int2 coord)
{
    coord -= int2(coord < int2(0, 0));
    coord += 1;
    int2 end_mirrored_clamp = 2 * aligned_resolution - resolution;
    int2 past_wrapped_coord = coord + 2 * (resolution - aligned_resolution) + 1;
    coord = (coord >= end_mirrored_clamp) ? past_wrapped_coord : min(coord, resolution);
    return float2(coord) * inv_resolution;
}

float4 gather_texels(int2 coord)
{
    float4 t = uTexture.Gather(uSampler, float3(generate_mirror_uv(coord), 0.0)).wzxy;
    t *= range_scale;
    if (dc_shift != 0u)
        t -= 0.5;
    return t;
}

void load_image_with_apron(uint2 group_id)
{
    int2 base_coord = int2(group_id) * int2(BLOCK_SIZE, BLOCK_SIZE) - APRON;
    int2 local_coord0 = 2 * unswizzle8x8(local_index);
    int2 coord0 = base_coord + local_coord0;

    float4 texels0 = gather_texels(coord0);
    float4 texels1 = gather_texels(coord0 + int2(16, 0));
    float4 texels2 = gather_texels(coord0 + int2(0, 16));
    float4 texels3 = gather_texels(coord0 + int2(16, 16));

    int ly = local_coord0.y >> 1;
    store_shared(ly + 0, local_coord0.x + 0, texels0.xz);
    store_shared(ly + 0, local_coord0.x + 1, texels0.yw);
    store_shared(ly + 0, local_coord0.x + 16, texels1.xz);
    store_shared(ly + 0, local_coord0.x + 17, texels1.yw);
    store_shared(ly + 8, local_coord0.x + 0, texels2.xz);
    store_shared(ly + 8, local_coord0.x + 1, texels2.yw);
    store_shared(ly + 8, local_coord0.x + 16, texels3.xz);
    store_shared(ly + 8, local_coord0.x + 17, texels3.yw);

    {
        int2 lc = int2(BLOCK_SIZE + 2 * (local_index % 4u), 2 * (local_index / 4u));
        float4 t = gather_texels(base_coord + lc);
        store_shared(lc.y >> 1, lc.x + 0, t.xz);
        store_shared(lc.y >> 1, lc.x + 1, t.yw);
    }
    {
        int2 lc = int2(2 * (local_index % 16u), BLOCK_SIZE + 2 * (local_index / 16u));
        float4 t = gather_texels(base_coord + lc);
        store_shared(lc.y >> 1, lc.x + 0, t.xz);
        store_shared(lc.y >> 1, lc.x + 1, t.yw);
    }
    if (local_index < 16u)
    {
        int2 lc = int2(BLOCK_SIZE + 2 * (local_index % 4u), BLOCK_SIZE + 2 * (local_index / 4u));
        float4 t = gather_texels(base_coord + lc);
        store_shared(lc.y >> 1, lc.x + 0, t.xz);
        store_shared(lc.y >> 1, lc.x + 1, t.yw);
    }
}

void forward_transform8x2()
{
    float2 values[16];
    int2 lc = int2(8 * (local_index % 4u), local_index / 4u);

    [unroll] for (int i0 = 0; i0 < 16; i0++) values[i0] = load_shared(lc.y, lc.x + i0);
    [unroll] for (int i1 = 1; i1 < 15; i1 += 2) values[i1] += ALPHA * (values[i1 - 1] + values[i1 + 1]);
    [unroll] for (int i2 = 2; i2 < 14; i2 += 2) values[i2] += BETA * (values[i2 - 1] + values[i2 + 1]);
    [unroll] for (int i3 = 3; i3 < 13; i3 += 2) values[i3] += GAMMA * (values[i3 - 1] + values[i3 + 1]);
    [unroll] for (int i4 = 4; i4 < 12; i4 += 2) values[i4] += DELTA * (values[i4 - 1] + values[i4 + 1]);

    GroupMemoryBarrierWithGroupSync(); // WAR hazard

    [unroll] for (int i5 = 2; i5 < 6; i5++)
    {
        float2 a = values[2 * i5 + 0] * inv_K;
        float2 b = values[2 * i5 + 1] * K;
        int yc = (lc.x >> 1) + (i5 - 2);
        store_shared(yc, 2 * lc.y + 0, float2(a.x, b.x));
        store_shared(yc, 2 * lc.y + 1, float2(a.y, b.y));
    }
}

void forward_transform4x2(bool active_lane, int y_offset)
{
    float2 values[12];
    int2 lc = int2(4 * (local_index % 8u), local_index / 8u + y_offset);

    if (active_lane)
    {
        [unroll] for (int i0 = 0; i0 < 12; i0++) values[i0] = load_shared(lc.y, lc.x + i0);
        [unroll] for (int i1 = 1; i1 < 11; i1 += 2) values[i1] += ALPHA * (values[i1 - 1] + values[i1 + 1]);
        [unroll] for (int i2 = 2; i2 < 10; i2 += 2) values[i2] += BETA * (values[i2 - 1] + values[i2 + 1]);
        [unroll] for (int i3 = 3; i3 < 9; i3 += 2) values[i3] += GAMMA * (values[i3 - 1] + values[i3 + 1]);
        [unroll] for (int i4 = 4; i4 < 8; i4 += 2) values[i4] += DELTA * (values[i4 - 1] + values[i4 + 1]);
    }
    else
    {
        [unroll] for (int z = 0; z < 12; z++) values[z] = float2(0.0, 0.0);
    }

    GroupMemoryBarrierWithGroupSync();

    if (active_lane)
    {
        [unroll] for (int i5 = 2; i5 < 4; i5++)
        {
            float2 a = values[2 * i5 + 0] * inv_K;
            float2 b = values[2 * i5 + 1] * K;
            int yc = (lc.x >> 1) + (i5 - 2);
            store_shared(yc, 2 * lc.y + 0, float2(a.x, b.x));
            store_shared(yc, 2 * lc.y + 1, float2(a.y, b.y));
        }
    }
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    local_index = gi;
    load_image_with_apron(gid.xy);
    GroupMemoryBarrierWithGroupSync();
    forward_transform8x2();                              // horizontal
    forward_transform4x2(local_index < 32u, BLOCK_SIZE_HALF); // horizontal, vertical-apron rows
    GroupMemoryBarrierWithGroupSync();
    forward_transform8x2();                              // vertical
    GroupMemoryBarrierWithGroupSync();

    int2 lc = unswizzle8x8(local_index);
    for (int y = lc.y; y < BLOCK_SIZE_HALF; y += 8)
    {
        for (int x = lc.x * 2; x < BLOCK_SIZE; x += 16)
        {
            float2 v0 = load_shared(y, x + 0);
            float2 v1 = load_shared(y, x + 1);
            int2 c = int2(gid.xy) * (BLOCK_SIZE / 2) + int2(x >> 1, y);
            uOutput[int3(c, 0)] = v0.x; // LL
            uOutput[int3(c, 2)] = v0.y; // horiz low / vert high
            uOutput[int3(c, 1)] = v1.x; // horiz high / vert low
            uOutput[int3(c, 3)] = v1.y; // HH
        }
    }
}
