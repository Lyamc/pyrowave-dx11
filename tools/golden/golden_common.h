// golden_common.h -- Common types and utilities for the CPU golden model
// Mirrors reference structs; checked against R/pyrowave_common.hpp, R/pyrowave_encoder.cpp

#ifndef GOLDEN_COMMON_H
#define GOLDEN_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Struct mirrors of R/pyrowave.h / R/pyrowave_common.hpp              */
/* ------------------------------------------------------------------ */

typedef struct golden_bitstream_packet {
    uint32_t offset_u32;
    uint32_t num_words;
} golden_bitstream_packet;

typedef struct golden_bitstream_sequence_header {
    uint32_t width_minus_1 : 14;
    uint32_t height_minus_1 : 14;
    uint32_t sequence : 3;
    uint32_t extended : 1;
    uint32_t total_blocks : 24;
    uint32_t code : 2;
    uint32_t chroma_resolution : 1;
    uint32_t color_primaries : 1;
    uint32_t transfer_function : 1;
    uint32_t ycbcr_transform : 1;
    uint32_t ycbcr_range : 1;
    uint32_t chroma_siting : 1;
} golden_bitstream_sequence_header;

typedef struct golden_bitstream_header {
    uint16_t ballot;
    uint16_t payload_words : 12;
    uint16_t sequence : 3;
    uint16_t extended : 1;
    uint32_t quant_code : 8;
    uint32_t block_index : 24;
} golden_bitstream_header;

typedef struct golden_quant_stats {
    uint16_t square_error_fp16;
    uint16_t encode_cost_bits;
} golden_quant_stats;

typedef struct golden_block_stats {
    uint32_t num_planes;
    golden_quant_stats stats[15];
} golden_block_stats;

typedef struct golden_block_meta {
    uint32_t code_word;
    uint32_t offset;
} golden_block_meta;

typedef struct golden_rd_operation {
    int32_t quant;
    uint16_t block_offset;
    uint16_t block_saving;
} golden_rd_operation;

typedef struct golden_block_mapping {
    int block_offset_8x8;
    int block_stride_8x8;
    int block_width_8x8;
    int block_height_8x8;
} golden_block_mapping;

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

#define GOLDEN_DECOMP_LEVELS 5
#define GOLDEN_ALIGNMENT 32
#define GOLDEN_MINIMUM_IMAGE_SIZE 64
#define GOLDEN_NUM_COMPONENTS 3
#define GOLDEN_NUM_BANDS_PER_LEVEL 4
#define GOLDEN_BLOCK_SPACE_SUBDIVISION 16
#define GOLDEN_NUM_RDO_BUCKETS 128
#define GOLDEN_RDO_BUCKET_OFFSET 64

/* ------------------------------------------------------------------ */
/* Utility functions                                                  */
/* ------------------------------------------------------------------ */

static inline int golden_align(int value, int align) {
    return (value + align - 1) & ~(align - 1);
}

static inline int golden_next_pow2(int v) {
    if (v <= 1) return 1;
    v--; v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16; v++;
    return v;
}

static inline int golden_popcount(uint32_t v) {
    int c = 0;
    while (v) { c += (int)(v & 1); v >>= 1; }
    return c;
}

static inline int golden_firstbithigh(uint32_t v) {
    if (v == 0) return 31;
    int r = 31;
    while ((v & 0x80000000u) == 0) { v <<= 1; r--; }
    return r;
}

static inline float golden_asfloat(uint32_t v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static inline uint16_t golden_f32tof16(float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    int sign = (v >> 31) & 1;
    int exp = ((v >> 23) & 0xff) - 127;
    int mant = v & 0x7fffff;
    if (exp >= 16) return (uint16_t)((sign << 15) | 0x7c00);
    if (exp < -14) return (uint16_t)(sign << 15);
    uint32_t h = (uint32_t)(exp + 15) << 10;
    h |= mant >> 13;
    int round_bit = (mant >> 12) & 1;
    int sticky = (mant & 0x1fff) != 0;
    if (round_bit && (sticky || (h & 1))) h++;
    return (uint16_t)((sign << 15) | (h & 0x7fff));
}

static inline float golden_f16tof32(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) & 1;
    int exp = (h >> 10) & 0x1f;
    int mant = h & 0x3ff;
    uint32_t v;
    if (exp == 0) { v = sign << 31; }
    else { exp = exp - 15 + 127; v = (sign << 31) | ((uint32_t)exp << 23) | (mant << 13); }
    float result;
    memcpy(&result, &v, 4);
    return result;
}

static inline uint8_t golden_encode_quant(float decoder_q_scale) {
    uint32_t v;
    memcpy(&v, &decoder_q_scale, 4);
    int e = ((v >> 23) & 0xff) - 127 - 4;
    int m = (v >> 20) & 0x7;
    e = -e;
    return (uint8_t)((e << 3) | m);
}

static inline float golden_decode_quant(uint8_t quant_code) {
    int e = 4 - (quant_code >> 3);
    int m = quant_code & 0x7;
    return (1.0f / (8.0f * 1024.0f * 1024.0f)) * (float)((8 + m) * (1 << (20 + e)));
}

static inline float golden_decode_quant_scale(uint32_t qcode) {
    return (float)qcode / 8.0f + 0.25f;
}

static inline uint32_t golden_encode_quant_scale(float scale) {
    return (uint32_t)(ceilf((scale - 0.25f) * 8.0f));
}

static inline uint8_t golden_modify_quant_code(uint8_t c, int quant) {
    int e = (c >> 3) & 31;
    e = e - quant;
    if (e < 0) e = 0;
    return (uint8_t)((c & 0x07) | (e << 3));
}

static inline uint32_t golden_quantize_code_word(uint32_t cw, int quant) {
    uint32_t result = cw;
    result &= ~(0x1fU << 20);
    uint8_t modified = golden_modify_quant_code((uint8_t)((cw >> 20) & 0xff), quant);
    result |= ((uint32_t)modified) << 20;
    return result;
}

static inline int golden_compute_required_8x8_size(uint32_t cw) {
    int total = 0;
    for (int sb = 0; sb < 8; sb++) {
        int planes = (int)((cw >> (2 * sb)) & 3);
        int q_bits = (int)((cw >> 20) & 0xf);
        total += planes + q_bits;
    }
    return total;
}

#endif /* GOLDEN_COMMON_H */
