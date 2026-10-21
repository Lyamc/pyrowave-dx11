// quant.hlsl -- Quantization stage: per-block quant scale, quantize, stats, payload
// Port of R/shaders/wavelet_quant.comp to cs_5_0
//
// Spec: docs/stages/02_quant.md
// Source: R/shaders/wavelet_quant.comp (wq:N), R/shaders/dwt_quant_scale.h,
//         R/shaders/constants.h, R/pyrowave_encoder.cpp:28-38,111-176,419-492
//
// Structure: five barriers, no barrier inside the q loop (§5.3)

#include "common.hlsli"
#include "../probe_fs_q2.inc"   // fs_q2_qs_bits[16]

// ------------------------------------------------------------------
// Push constants (matches QuantizerPushData in R/pyrowave_encoder.cpp:28-38)
// ------------------------------------------------------------------
cbuffer PushConstants : register(b0) {
    int2  resolution                   : packoffset(c0);
    int2  resolution_8x8_blocks        : packoffset(c0.z);
    float2 inv_resolution              : packoffset(c1);
    float input_layer                  : packoffset(c1.z);
    float quant_resolution             : packoffset(c1.w);
    int   block_offset                 : packoffset(c2);
    int   block_stride                 : packoffset(c2.y);
    float rdo_distortion_scale         : packoffset(c2.z);
    uint  coeff_pitch                  : packoffset(c2.w);
};

// Subbands of one level share the coefficient buffer. gid.z picks the row.
// Fusing them is one launch instead of one launch per subband.
cbuffer Subbands : register(b1) {
    float4 band_lq[4];
    int4 band_off;
};

// ------------------------------------------------------------------
// Resources
// ------------------------------------------------------------------
ByteAddressBuffer t0 : register(t0);
RWByteAddressBuffer u0 : register(u0);
RWByteAddressBuffer u1 : register(u1);
RWByteAddressBuffer u2 : register(u2);

// ------------------------------------------------------------------
// Constants from constants.h
// ------------------------------------------------------------------
#define Q_PLANES_OFFSET 16
#define Q_PLANES_BITS   4
#define QUANT_SCALE_OFFSET 20
#define QUANT_SCALE_BITS   4

// ------------------------------------------------------------------
// Groupshared memory (§5.3)
// ------------------------------------------------------------------
// 19 bytes per lane when msb <= 16 (docs/stages/02_quant.md). 64 words is
// 32 bytes per lane. The old 640-word slice was 20 KB and left room for
// one wave per SIMD on the HD 6670.
#define CLUSTER_WORDS 64
#define CLUSTER_BYTES (CLUSTER_WORDS * 4)
// Pack loads payload[meta+8]. Bytes 8..135 are 32 stripe counters, so the
// first 8x8 starts at byte 136. The slot index is not part of the packet.
#define PAYLOAD_SLOT0 128u

groupshared float  gs_max[128];
groupshared int    gs_early[128];
groupshared int    gs_ctotal[16];
groupshared int    gs_group_base[8];
groupshared uint   gs_staging[8 * CLUSTER_WORDS];
groupshared uint   gs_payload_counter[8]; // bytes allocated by this cluster
groupshared uint   gs_pcode[128];

// firstbithigh(0) is -1. On the HD 6670 it is not, so a 4x2 of zeros inside a
// live 8x8 (the partial block at the bottom of a 512x288 LL) was charged an
// extra magnitude bit. The decoder then scrambled that row. A full block never
// asks for the MSB of zero, which is why 720p and a 512x256 frame stayed a ramp.
int msb_of(uint v)
{
    if (v == 0u) return -1;
    return int(firstbithigh(v));
}

// Coefficient buffer written by the DWT store. Out-of-range texels are 0,
// matching Texture2D.Load. Padding past the logical size is not cleared,
// so a wide load is only used when all four texels are inside the band.
float band_at(int layer, int x, int y)
{
    if ((uint)x >= (uint)resolution.x || (uint)y >= (uint)resolution.y) return 0.0f;
    uint pitch_h = (uint(resolution.y) + 31u) & ~31u;
    uint idx = uint(layer) * coeff_pitch * pitch_h + uint(y) * coeff_pitch + uint(x);
    return asfloat(t0.Load(idx * 4u));
}

float4 band_row4(int layer, int x, int y)
{
    if ((uint)y >= (uint)resolution.y || (uint)x >= (uint)resolution.x)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (((uint)x & 3u) == 0u && (uint)x + 3u < (uint)resolution.x) {
        uint pitch_h = (uint(resolution.y) + 31u) & ~31u;
        uint idx = uint(layer) * coeff_pitch * pitch_h + uint(y) * coeff_pitch + uint(x);
        uint4 w = t0.Load4(idx * 4u);
        return float4(asfloat(w.x), asfloat(w.y), asfloat(w.z), asfloat(w.w));
    }
    return float4(
        band_at(layer, x, y), band_at(layer, x + 1, y),
        band_at(layer, x + 2, y), band_at(layer, x + 3, y));
}

// gs_staging starts at 0 and each payload byte is written once. A word that
// this lane fills completely does not share storage with another lane.
void emit_staged(inout uint acc, inout uint fill, inout uint pos, uint cluster, uint value)
{
    if (fill == 0u && (pos & 3u) != 0u) {
        uint word_idx = cluster * uint(CLUSTER_WORDS) + (pos >> 2u);
        uint bit_shift = (pos & 3u) << 3u;
        InterlockedOr(gs_staging[word_idx], (value & 0xffu) << bit_shift);
        pos++;
        return;
    }
    acc |= (value & 0xffu) << (fill * 8u);
    fill++;
    if (fill == 4u) {
        gs_staging[cluster * uint(CLUSTER_WORDS) + (pos >> 2u)] = acc;
        pos += 4u;
        acc = 0u;
        fill = 0u;
    }
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    // Eight 8x8 blocks per group, one cluster of 8 lanes each. The old single
    // cluster was a workaround for Texture2D.Load past x or y of 7. Coefficients
    // now come from the raw buffer, so a full wave is safe. Each cluster keeps
    // its own staging slice so the payloads do not share words.
    uint cluster = gi >> 3;
    uint lane = gi & 7u;
    if (gi == 0u) {
        [unroll] for (int c = 0; c < 8; c++) {
            gs_payload_counter[c] = 0u;
            gs_group_base[c] = 0;
        }
        [unroll] for (int c2 = 0; c2 < 16; c2++) gs_ctotal[c2] = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    // ------------------------------------------------------------------
    // Thread mapping. gid.x covers 8 blocks. gi is cluster * 8 + lane.
    // ------------------------------------------------------------------
    uint block_x = 0u;
    uint block_y = 0u;
    uint local_coord_x = 4 * (lane >> 2);
    uint local_coord_y = 2 * (lane & 3);
    int2 block8 = int2(int(gid.x) * 8 + int(cluster), int(gid.y));
    int2 coord = int2(block8.x * 8 + int(local_coord_x), block8.y * 8 + int(local_coord_y));
    uint ublock_x = block_x;
    uint ublock_y = block_y;
    bool in_bounds = (block8.x < resolution_8x8_blocks.x) && (block8.y < resolution_8x8_blocks.y);
    uint sub = gid.z;
    uint block_linear = uint(band_off[sub]) + uint(block_stride) * uint(block8.y) + uint(block8.x);
    int layer = (int)band_lq[sub].x;

    // ------------------------------------------------------------------
    // P0: Load texels from the DWT coefficient buffer. Texture2D.Load on the
    // HD 6670 disagrees with the UAV store once x or y passes 7.
    // 4x2 pixel order is 0 2 4 6 / 1 3 5 7: (x,y), (x,y+1), (x+1,y), (x+1,y+1).
    // ------------------------------------------------------------------
    int cx = int(coord.x);
    int cy = int(coord.y);

    // cx is a multiple of 4. One load per row, then the same 2x2 order as
    // the scalar loads: (x,y), (x,y+1), (x+1,y), (x+1,y+1).
    float4 row0 = band_row4(layer, cx, cy);
    float4 row1 = band_row4(layer, cx, cy + 1);
    precise float4 t0a = float4(row0.x, row1.x, row0.y, row1.y);
    precise float4 t0b = float4(row0.z, row1.z, row0.w, row1.w);

    precise float4 s0 = t0a * band_lq[sub].y;
    precise float4 s1 = t0b * band_lq[sub].y;

    float m_sub = max(max(abs(s0.x), abs(s0.y)), max(abs(s0.z), abs(s0.w)));
    m_sub = max(m_sub, max(max(abs(s1.x), abs(s1.y)), max(abs(s1.z), abs(s1.w))));
    gs_max[gi] = m_sub;

    // Barrier 1
    GroupMemoryBarrierWithGroupSync();

    // ------------------------------------------------------------------
    // P1: m_wave, code, qs, scaled s, ints, msb, active, early(0)
    // ------------------------------------------------------------------
    // Cluster max across all 8 lanes. The old xor only read four originals,
    // so lanes 3 and 7 of a partial 8x8 (the zero 4x2s that do not touch a
    // live lane) kept m_wave of 0. They reserved no payload, the code word
    // still claimed the cluster quality planes, and pack read the next 4x2
    // as this one. That is the scrambled tail of a 512x288 frame.
    uint max_base = cluster * 8u;
    float m_wave = gs_max[max_base];
    m_wave = max(m_wave, gs_max[max_base + 1u]);
    m_wave = max(m_wave, gs_max[max_base + 2u]);
    m_wave = max(m_wave, gs_max[max_base + 3u]);
    m_wave = max(m_wave, gs_max[max_base + 4u]);
    m_wave = max(m_wave, gs_max[max_base + 5u]);
    m_wave = max(m_wave, gs_max[max_base + 6u]);
    m_wave = max(m_wave, gs_max[max_base + 7u]);

    uint quant_code = 6;
    float quant_scale = 1.0f;

    if (in_bounds && m_wave >= 1.0f) {
        float y = m_wave - 0.25f;
        uint y_bits = asuint(y);
        int e = int((y_bits >> 23) & 0xffu) - 126;
        float target = float(int(1) << e) - 0.25f;
        float inv = m_wave / target;
        quant_code = uint(ceil((inv - 0.25f) * 8.0f));
        if (quant_code > 15) quant_code = 15;
        quant_scale = asfloat(fs_q2_qs_bits[quant_code]);
    }

    precise float4 s0s = s0 * quant_scale;
    precise float4 s1s = s1 * quant_scale;

    int4 a0 = abs(int4(s0s));
    int4 a1 = abs(int4(s1s));

    int max_abs = int(m_wave * quant_scale);
    int b4x2 = int(m_sub * quant_scale);

    bool active = in_bounds && (max_abs != 0);
    gs_early[gi] = 0;

    if (lane == 0u && in_bounds && !active) {
        u0.Store(int(block_linear * 8u), 0u);
        u0.Store(int(block_linear * 8u + 4u), 0u);
        u1.Store(int(block_linear * 64u), 0u);
        u1.Store(int(block_linear * 64u + 4u), 0u);
    }

    int msb = active ? msb_of(uint(max_abs)) : -1;

    // Compute early(0) for this thread (q=0)
    int early_t = 0;
    int qp_t = 0;
    int sh_t = b4x2;

    if (active) {
        int block4x2_max = b4x2;
        int msb_q = msb;
        int quality_planes = 0;
        if (msb_q >= 3) {
            quality_planes = msb_q - 2;
            block4x2_max >>= quality_planes;
        }
        // Match wavelet_quant.comp: once quality planes exist, the sign byte
        // replaces the (block>0) bit. Adding quality_planes on top drops that
        // byte for lanes whose own max shifts to zero, and the next lane's
        // payload overlaps.
        int encode_cost_early = (block4x2_max > 0) ? 1 : 0;
        if (msb_q >= 3) {
            encode_cost_early = quality_planes + 1;
        }
        encode_cost_early += msb_of(uint(block4x2_max)) + 1;
        early_t = encode_cost_early;
        qp_t = quality_planes;
        sh_t = block4x2_max;
    }

    gs_early[gi] = early_t;

    // Barrier 2
    GroupMemoryBarrierWithGroupSync();

    // Exclusive prefix of the 8 lane costs. The old xor scan mixed lanes 2 and 3.
    int scan_excl = 0;
    int cluster_sum = 0;
    {
        uint base_lane = gi & ~7u;
        [unroll] for (uint sum_i = 0u; sum_i < 8u; sum_i++) cluster_sum += gs_early[base_lane + sum_i];
        [unroll] for (uint pref_i = 0u; pref_i < lane; pref_i++) scan_excl += gs_early[base_lane + pref_i];
    }
    if (lane == 7u) gs_ctotal[cluster] = uint(cluster_sum);
    GroupMemoryBarrierWithGroupSync();

    // ------------------------------------------------------------------
    // P3: round the cluster up to 4 bytes and park it in a private slot.
    // One InterlockedAdd per 8x8 on address 0 serialized the HD 6670: a
    // busy 720p frame has tens of thousands of those. The slot index is
    // only the scratch address pack copies from. Stripe the byte count
    // across 32 bins so clamp still sees the same total.
    // ------------------------------------------------------------------
    int group_base = 0;

    if (lane == 0u) {
        uint total = uint(gs_ctotal[cluster]);
        if (total > 0u) total = (total + 3u) & ~3u;
        gs_payload_counter[cluster] = total;
        gs_group_base[cluster] = (total > 0u)
            ? int(PAYLOAD_SLOT0 + block_linear * uint(CLUSTER_BYTES))
            : 0;
    }

    // Barrier 4
    GroupMemoryBarrierWithGroupSync();

    if (gi == 0u) {
        uint sum = 0u;
        [unroll] for (int c = 0; c < 8; c++) sum += gs_payload_counter[c];
        if (sum > 0u) {
            uint bin = (gid.x + gid.y * 3u + gid.z * 5u) & 31u;
            uint prior = 0u;
            u2.InterlockedAdd(8u + bin * 4u, sum, prior);
            gs_ctotal[8] = int(prior);
        }
    }
    group_base = gs_group_base[cluster];

    // Zero only the words this cluster will publish. Clearing all 640 words
    // per cluster was most of the group, and the tail is never stored.
    {
        int clear_words = int(gs_payload_counter[cluster]) >> 2;
        for (int ci = int(lane); ci < clear_words && ci < CLUSTER_WORDS; ci += 8)
            gs_staging[cluster * uint(CLUSTER_WORDS) + uint(ci)] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // ------------------------------------------------------------------
    // P4: cluster_base, payload bytes, meta
    // ------------------------------------------------------------------
    int cluster_base = group_base;
    if (active) {
        // Write payload bytes
        bool need_sign = (sh_t != 0) || (qp_t != 0);
        if (need_sign && early_t > 0) {
            uint sign_bits = 0u;
            sign_bits |= (s0s.x < 0.0f) ? (1u << 0) : 0u;
            sign_bits |= (s0s.y < 0.0f) ? (1u << 1) : 0u;
            sign_bits |= (s0s.z < 0.0f) ? (1u << 2) : 0u;
            sign_bits |= (s0s.w < 0.0f) ? (1u << 3) : 0u;
            sign_bits |= (s1s.x < 0.0f) ? (1u << 4) : 0u;
            sign_bits |= (s1s.y < 0.0f) ? (1u << 5) : 0u;
            sign_bits |= (s1s.z < 0.0f) ? (1u << 6) : 0u;
            sign_bits |= (s1s.w < 0.0f) ? (1u << 7) : 0u;

            uint acc = 0u;
            uint fill = 0u;
            uint pos = uint(scan_excl);
            if (pos < uint(CLUSTER_BYTES)) emit_staged(acc, fill, pos, cluster, sign_bits);

            int plane_code = msb_of(uint(sh_t)) + 1;
            int plane_iterations = qp_t + plane_code;

            for (int q = plane_iterations - 1; q >= 0; q--) {
                uint plane_bits = 0u;
                plane_bits |= (uint(a0.x >> q) & 1u) << 0;
                plane_bits |= (uint(a0.y >> q) & 1u) << 1;
                plane_bits |= (uint(a0.z >> q) & 1u) << 2;
                plane_bits |= (uint(a0.w >> q) & 1u) << 3;
                plane_bits |= (uint(a1.x >> q) & 1u) << 4;
                plane_bits |= (uint(a1.y >> q) & 1u) << 5;
                plane_bits |= (uint(a1.z >> q) & 1u) << 6;
                plane_bits |= (uint(a1.w >> q) & 1u) << 7;

                if (pos < uint(CLUSTER_BYTES)) emit_staged(acc, fill, pos, cluster, plane_bits);
            }
            for (uint tail = 0u; tail < fill; tail++) {
                if (pos >= uint(CLUSTER_BYTES)) break;
                uint word_idx = cluster * uint(CLUSTER_WORDS) + (pos >> 2u);
                uint bit_shift = (pos & 3u) << 3u;
                InterlockedOr(gs_staging[word_idx], ((acc >> (tail * 8u)) & 0xffu) << bit_shift);
                pos++;
            }
        }
    }

    // Each lane publishes its own 2-bit plane code. Lane 0 merges the cluster.
    gs_pcode[gi] = 0u;
    if (active) {
        int pc = msb_of(uint(sh_t)) + 1;
        if (pc < 0) pc = 0;
        gs_pcode[gi] = uint(pc);
    }
    GroupMemoryBarrierWithGroupSync();

    // Lane 0 writes meta plus the RDO error table analyze/resolve consume.
    if (lane == 0u && active) {
        uint block_index = block_linear;

        uint code_word = uint(qp_t) << Q_PLANES_OFFSET;
        code_word |= (quant_code << QUANT_SCALE_OFFSET);

        uint merged = 0u;
        [unroll] for (uint lane_i = 0u; lane_i < 8u; lane_i++)
            merged |= gs_pcode[gi + lane_i] << (2u * lane_i);
        code_word |= merged;

        u0.Store(int(block_index * 8u), code_word);
        u0.Store(int(block_index * 8u + 4u), uint(cluster_base));

        int mag_bytes = 0;
        {
            uint base_lane = gi & ~7u;
            [unroll] for (uint mb = 0u; mb < 8u; mb++)
                mag_bytes += max(gs_early[base_lane + mb] - 1, 0);
        }
        // 64 is the max significant count of an 8x8. Over-counting here
        // still feeds leftover clamp when a busy frame is over budget.
        int late_bits_0 = 8 * mag_bytes + 64;
        uint errors_0 = uint(late_bits_0 & 0xffff) << 16;
        uint num_planes = uint(msb + 1);
        u1.Store4(int(block_index * 64 + 0), uint4(num_planes, errors_0, 0, 0));
    }

    // Barrier 5
    GroupMemoryBarrierWithGroupSync();

    // ------------------------------------------------------------------
    // P5: Copy staging words to u2
    // ------------------------------------------------------------------
    int my_words = int(gs_payload_counter[cluster]) >> 2;
    for (int ci = int(lane); ci < my_words; ci += 8) {
        if (ci < CLUSTER_WORDS) u2.Store(int(8 + group_base + ci * 4), gs_staging[cluster * uint(CLUSTER_WORDS) + uint(ci)]);
    }

    // Flush
    GroupMemoryBarrierWithGroupSync();
}
