// D3D11 (cs_5_0) port of PyroWave shaders/dwt.comp @ a0d2e71 (PRECISION=1, FP16=0).
// The tile exchange is a global RWBuffer, not groupshared. On the HD 6670 the
// groupshared tile matched this math for an 8-wide LL and split a 16-wide LL.
// Four dispatches (phase 0..3) so a wave never reads a value another wave just wrote.
cbuffer Push : register(b0)
{
    int2 resolution;
    float2 inv_resolution;
    int2 aligned_resolution;
    uint dc_shift;
    float range_scale;
    uint phase;
    uint groups_x;
    uint slot_stride;
    uint coeff_pitch;
};

Texture2DArray<float> uTexture : register(t0);
SamplerState uSampler : register(s0); // MIRROR addressing == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
RWTexture2DArray<float> uOutput : register(u0);
RWBuffer<float> uScratch : register(u1);
// Same values as uOutput, one float per texel. Quant reads this. Texture2D.Load
// on the HD 6670 returns a bad texel once x or y passes 7 on a narrow band.
RWBuffer<float> uCoeff : register(u2);

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

#define SHARED_ROWS ((BLOCK_SIZE + 2 * APRON) / 2)
#define SHARED_STRIDE 64
static uint local_index;
static uint read_base;
static uint write_base;

float2 load_shared(int y, int x)
{
    uint i = (read_base + uint(y * SHARED_STRIDE + x)) * 2u;
    return float2(uScratch[i], uScratch[i + 1u]);
}

void store_shared(int y, int x, float2 v)
{
    uint i = (write_base + uint(y * SHARED_STRIDE + x)) * 2u;
    uScratch[i] = v.x;
    uScratch[i + 1u] = v.y;
}

int2 unswizzle8x8(uint index)
{
    uint y = index & 1u;
    uint x = (index >> 1) & 3u;
    y |= ((index >> 3) & 3u) << 1;
    x |= ((index >> 5) & 1u) << 2;
    return int2(x, y);
}

// Integer mirror of generate_mirror_uv + Gather, matching map_coord in test_t6c.
int mirror_coord(int i, int R)
{
    int p = R * 2;
    int t = i % p;
    if (t < 0) t += p;
    return (t >= R) ? (p - 1 - t) : t;
}

int map_coord(int c, int R, int A)
{
    int g = c & ~1;
    if (g < 0) g -= 1;
    g += 1;
    int endc = 2 * A - R;
    int past = g + 2 * (R - A) + 1;
    g = (g >= endc) ? past : min(g, R);
    return ((c & 1) != 0) ? mirror_coord(g, R) : mirror_coord(g - 1, R);
}

float load_texel(int x, int y)
{
    x = map_coord(x, resolution.x, aligned_resolution.x);
    y = map_coord(y, resolution.y, aligned_resolution.y);
    float v = uTexture.Load(int4(x, y, 0, 0)).x;
    v *= range_scale;
    if (dc_shift != 0u) v -= 0.5;
    return v;
}

float4 gather_texels(int2 coord)
{
    // Gather .wzxy footprint, stored later as vertical pairs (xz, yw):
    // (x,y), (x+1,y), (x,y+1), (x+1,y+1).
    return float4(
        load_texel(coord.x, coord.y),
        load_texel(coord.x + 1, coord.y),
        load_texel(coord.x, coord.y + 1),
        load_texel(coord.x + 1, coord.y + 1));
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
    uint tile = (gid.y * groups_x + gid.x) * (SHARED_ROWS * SHARED_STRIDE);
    // phase 0 writes slot 0, phase 1 reads slot 0 and writes slot 1,
    // phase 2 reads slot 1 and writes slot 0, phase 3 reads slot 0.
    read_base = tile + ((phase == 2u) ? slot_stride : 0u);
    write_base = tile + ((phase == 1u) ? slot_stride : 0u);

    if (phase == 0u) {
        load_image_with_apron(gid.xy);
        return;
    }
    if (phase == 1u) {
        forward_transform8x2();
        forward_transform4x2(local_index < 32u, BLOCK_SIZE_HALF);
        return;
    }
    if (phase == 2u) {
        forward_transform8x2();
        return;
    }

    int out_w = aligned_resolution.x >> 1;
    int out_h = aligned_resolution.y >> 1;
    int2 lc = unswizzle8x8(local_index);
    for (int y = lc.y; y < BLOCK_SIZE_HALF; y += 8)
    {
        for (int x = lc.x * 2; x < BLOCK_SIZE; x += 16)
        {
            float2 v0 = load_shared(y, x + 0);
            float2 v1 = load_shared(y, x + 1);
            int2 c = int2(gid.xy) * (BLOCK_SIZE / 2) + int2(x >> 1, y);
            if ((uint)c.x < (uint)out_w && (uint)c.y < (uint)out_h) {
                uOutput[int3(c, 0)] = v0.x; // LL
                uOutput[int3(c, 2)] = v0.y; // horiz low / vert high
                uOutput[int3(c, 1)] = v1.x; // horiz high / vert low
                uOutput[int3(c, 3)] = v1.y; // HH
                uint pitch_h = (uint(out_h) + 31u) & ~31u;
                uint base = uint(c.y) * coeff_pitch + uint(c.x);
                uint layer_stride = coeff_pitch * pitch_h;
                uCoeff[0u * layer_stride + base] = v0.x;
                uCoeff[2u * layer_stride + base] = v0.y;
                uCoeff[1u * layer_stride + base] = v1.x;
                uCoeff[3u * layer_stride + base] = v1.y;
            }
        }
    }
}
