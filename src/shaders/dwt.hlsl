// D3D11 (cs_5_0) port of PyroWave shaders/dwt.comp @ a0d2e71 (PRECISION=1, FP16=0).
// The tile stays in groupshared and one dispatch runs all four passes. The
// earlier 16-wide split came from Texture2D.Load, which this shader does not
// use: samples come from the raw buffer below. Two slots so a pass can read
// the previous slot while it writes the next, the same split the global
// scratch used across four dispatches.
// packoffset keeps this layout in every compiled variant. An unread field
// would otherwise be stripped and the host's 64-byte record would land
// on the wrong constants.
cbuffer Push : register(b0)
{
    int2 resolution           : packoffset(c0);
    float2 inv_resolution     : packoffset(c0.z);
    int2 aligned_resolution   : packoffset(c1);
    uint dc_shift             : packoffset(c1.z);
    float range_scale         : packoffset(c1.w);
    uint phase                : packoffset(c2);
    uint groups_x             : packoffset(c2.y);
    uint slot_stride          : packoffset(c2.z);
    uint coeff_pitch          : packoffset(c2.w);
    // src_float == 0: tightly packed R8 plane, src_pitch is the byte stride.
    // src_float != 0: layer 0 of the previous band, src_pitch is the float stride.
    uint src_pitch            : packoffset(c3);
    uint src_float            : packoffset(c3.y);
    uint group_origin_x       : packoffset(c3.z);
    uint group_origin_y       : packoffset(c3.w);
};

// Compiled four ways. Interior groups only sample inside the logical plane,
// where map_coord is the identity, so that variant never mirrors. Byte and
// float sources are different dispatches, so each variant keeps one load.
#ifndef DWT_INTERIOR
#define DWT_INTERIOR 0
#endif
#ifndef DWT_FLOAT
#define DWT_FLOAT 0
#endif

// Texture2D.Load on the HD 6670 returns a bad texel once x or y passes 7 on a
// narrow band. A 512x288 level-3 band is 32x18, so the rows past y = 7 (the
// bottom of the picture) were wrong. A 64-wide frame is aligned to 128 and
// its bands are narrow, so the same load copied the left edge across the canvas.
ByteAddressBuffer uSrc : register(t0);
RWTexture2DArray<float> uOutput : register(u0);
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
#define SHARED_STRIDE 40
groupshared float2 gs0[SHARED_ROWS][SHARED_STRIDE];
groupshared float2 gs1[SHARED_ROWS][SHARED_STRIDE];
static uint local_index;

// slot is a literal at every call. A runtime select made each lifting step
// touch both arrays.
float2 load_gs(int slot, int y, int x)
{
    if (slot == 0) return gs0[y][x];
    return gs1[y][x];
}

void store_gs(int slot, int y, int x, float2 v)
{
    if (slot == 0) gs0[y][x] = v;
    else gs1[y][x] = v;
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
    // The apron is 4 and a tile is 32, so a sample lands within four periods
    // of a 4-wide band and within one period of a full-frame band. An integer
    // divide here dominated the level-0 dispatch on the HD 6670.
    int p = R * 2;
    int t = i;
    if (t >= p) t -= p;
    if (t >= p) t -= p;
    if (t >= p) t -= p;
    if (t >= p) t -= p;
    if (t < 0) t += p;
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

#if !DWT_INTERIOR
float load_texel(int x, int y)
{
    x = map_coord(x, resolution.x, aligned_resolution.x);
    y = map_coord(y, resolution.y, aligned_resolution.y);
#if DWT_FLOAT
    uint idx = uint(y) * src_pitch + uint(x);
    return asfloat(uSrc.Load(idx * 4u));
#else
    uint idx = uint(y) * src_pitch + uint(x);
    uint word = uSrc.Load(idx & ~3u);
    uint shift = (idx & 3u) * 8u;
    // Plane dispatches use range 1 and the DC shift. Both are uniforms.
    return float((word >> shift) & 255u) * (1.0 / 255.0) - 0.5;
#endif
}
#endif

float4 gather_texels(int2 coord)
{
    // Gather footprint, stored later as vertical pairs (xz, yw):
    // (x,y), (x+1,y), (x,y+1), (x+1,y+1). Origins are even.
#if DWT_INTERIOR && DWT_FLOAT
    uint row0 = uint(coord.y) * src_pitch + uint(coord.x);
    uint row1 = row0 + src_pitch;
    uint2 a = uSrc.Load2(row0 * 4u);
    uint2 b = uSrc.Load2(row1 * 4u);
    return float4(asfloat(a.x), asfloat(a.y), asfloat(b.x), asfloat(b.y));
#elif DWT_INTERIOR
    uint row0 = uint(coord.y) * src_pitch + uint(coord.x);
    uint row1 = row0 + src_pitch;
    uint w0 = uSrc.Load(row0 & ~3u);
    uint w1 = uSrc.Load(row1 & ~3u);
    uint s0 = (row0 & 3u) * 8u;
    uint s1 = (row1 & 3u) * 8u;
    float k = 1.0 / 255.0;
    return float4(
        float((w0 >> s0) & 255u) * k - 0.5,
        float((w0 >> (s0 + 8u)) & 255u) * k - 0.5,
        float((w1 >> s1) & 255u) * k - 0.5,
        float((w1 >> (s1 + 8u)) & 255u) * k - 0.5);
#else
    return float4(
        load_texel(coord.x, coord.y),
        load_texel(coord.x + 1, coord.y),
        load_texel(coord.x, coord.y + 1),
        load_texel(coord.x + 1, coord.y + 1));
#endif
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
    store_gs(0, ly + 0, local_coord0.x + 0, texels0.xz);
    store_gs(0, ly + 0, local_coord0.x + 1, texels0.yw);
    store_gs(0, ly + 0, local_coord0.x + 16, texels1.xz);
    store_gs(0, ly + 0, local_coord0.x + 17, texels1.yw);
    store_gs(0, ly + 8, local_coord0.x + 0, texels2.xz);
    store_gs(0, ly + 8, local_coord0.x + 1, texels2.yw);
    store_gs(0, ly + 8, local_coord0.x + 16, texels3.xz);
    store_gs(0, ly + 8, local_coord0.x + 17, texels3.yw);

    {
        int2 lc = int2(BLOCK_SIZE + 2 * (local_index % 4u), 2 * (local_index / 4u));
        float4 t = gather_texels(base_coord + lc);
        store_gs(0, lc.y >> 1, lc.x + 0, t.xz);
        store_gs(0, lc.y >> 1, lc.x + 1, t.yw);
    }
    {
        int2 lc = int2(2 * (local_index % 16u), BLOCK_SIZE + 2 * (local_index / 16u));
        float4 t = gather_texels(base_coord + lc);
        store_gs(0, lc.y >> 1, lc.x + 0, t.xz);
        store_gs(0, lc.y >> 1, lc.x + 1, t.yw);
    }
    if (local_index < 16u)
    {
        int2 lc = int2(BLOCK_SIZE + 2 * (local_index % 4u), BLOCK_SIZE + 2 * (local_index / 4u));
        float4 t = gather_texels(base_coord + lc);
        store_gs(0, lc.y >> 1, lc.x + 0, t.xz);
        store_gs(0, lc.y >> 1, lc.x + 1, t.yw);
    }
}

void forward_transform8x2(int rd, int wr)
{
    float2 values[16];
    int2 lc = int2(8 * (local_index % 4u), local_index / 4u);

    [unroll] for (int i0 = 0; i0 < 16; i0++) values[i0] = load_gs(rd, lc.y, lc.x + i0);
    [unroll] for (int i1 = 1; i1 < 15; i1 += 2) values[i1] += ALPHA * (values[i1 - 1] + values[i1 + 1]);
    [unroll] for (int i2 = 2; i2 < 14; i2 += 2) values[i2] += BETA * (values[i2 - 1] + values[i2 + 1]);
    [unroll] for (int i3 = 3; i3 < 13; i3 += 2) values[i3] += GAMMA * (values[i3 - 1] + values[i3 + 1]);
    [unroll] for (int i4 = 4; i4 < 12; i4 += 2) values[i4] += DELTA * (values[i4 - 1] + values[i4 + 1]);

    [unroll] for (int i5 = 2; i5 < 6; i5++)
    {
        float2 a = values[2 * i5 + 0] * inv_K;
        float2 b = values[2 * i5 + 1] * K;
        int yc = (lc.x >> 1) + (i5 - 2);
        store_gs(wr, yc, 2 * lc.y + 0, float2(a.x, b.x));
        store_gs(wr, yc, 2 * lc.y + 1, float2(a.y, b.y));
    }
}

void forward_transform4x2(bool active_lane, int y_offset, int rd, int wr)
{
    float2 values[12];
    int2 lc = int2(4 * (local_index % 8u), local_index / 8u + y_offset);

    if (active_lane)
    {
        [unroll] for (int i0 = 0; i0 < 12; i0++) values[i0] = load_gs(rd, lc.y, lc.x + i0);
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
            store_gs(wr, yc, 2 * lc.y + 0, float2(a.x, b.x));
            store_gs(wr, yc, 2 * lc.y + 1, float2(a.y, b.y));
        }
    }
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    local_index = gi;
    uint2 tile = gid.xy + uint2(group_origin_x, group_origin_y);
    load_image_with_apron(tile);
    GroupMemoryBarrierWithGroupSync();

    forward_transform8x2(0, 1);
    forward_transform4x2(local_index < 32u, BLOCK_SIZE_HALF, 0, 1);
    GroupMemoryBarrierWithGroupSync();

    forward_transform8x2(1, 0);
    GroupMemoryBarrierWithGroupSync();

    int out_w = aligned_resolution.x >> 1;
    int out_h = aligned_resolution.y >> 1;
    int2 lc = unswizzle8x8(local_index);
    for (int y = lc.y; y < BLOCK_SIZE_HALF; y += 8)
    {
        for (int x = lc.x * 2; x < BLOCK_SIZE; x += 16)
        {
            float2 v0 = load_gs(0, y, x + 0);
            float2 v1 = load_gs(0, y, x + 1);
            int2 c = int2(tile) * (BLOCK_SIZE / 2) + int2(x >> 1, y);
            if ((uint)c.x < (uint)out_w && (uint)c.y < (uint)out_h) {
                // Quant and the next level read uCoeff. The texture UAV is
                // kept bound so the dispatch signature stays put, but writing
                // it doubled the bandwidth on the HD 6670.
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
