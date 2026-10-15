// golden_test.c -- Unit tests for the CPU golden model

#include "golden_pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define TEST(name) do { tests_run++; printf("TEST: %s ... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { tests_failed++; printf("FAIL: %s\n", msg); } while(0)

static void test_f16_roundtrip(void) {
    TEST(f16_roundtrip);
    float tv[] = {0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 65504.0f, 3.14159f, -3.14159f, 1.5f, 2.0f, 4.0f, 0.25f, 100.0f};
    int n = sizeof(tv) / sizeof(tv[0]);
    for (int i = 0; i < n; i++) {
        uint16_t h = golden_f32tof16(tv[i]);
        float r = golden_f16tof32(h);
        float diff = fabsf(r - tv[i]);
        float ulp = fabsf(tv[i]) * 0.001f;
        if (ulp < 0.001f) ulp = 0.001f;
        if (diff > ulp && diff > 0.01f) { FAIL("f16 roundtrip"); return; }
    }
    PASS();
}

static void test_quant_encode_decode(void) {
    TEST(quant_encode_decode);
    uint8_t code = golden_encode_quant(1.0f);
    float decoded = golden_decode_quant(code);
    if (fabsf(decoded - 1.0f) > 0.01f) { FAIL("quant encode/decode"); return; }
    if (golden_decode_quant_scale(6) != 1.0f) { FAIL("decode_quant_scale(6)"); return; }
    PASS();
}

static void test_firstbithigh(void) {
    TEST(firstbithigh);
    if (golden_firstbithigh(0u) != 31) { FAIL("fbh(0)"); return; }
    if (golden_firstbithigh(1u) != 0) { FAIL("fbh(1)"); return; }
    if (golden_firstbithigh(2u) != 1) { FAIL("fbh(2)"); return; }
    if (golden_firstbithigh(0x80000000u) != 31) { FAIL("fbh(0x80000000)"); return; }
    PASS();
}

static void test_popcount(void) {
    TEST(popcount);
    if (golden_popcount(0u) != 0) { FAIL("pc(0)"); return; }
    if (golden_popcount(1u) != 1) { FAIL("pc(1)"); return; }
    if (golden_popcount(0xffu) != 8) { FAIL("pc(0xff)"); return; }
    if (golden_popcount(0xaaaaaaaau) != 16) { FAIL("pc(0xaaaaaaaa)"); return; }
    PASS();
}

static void test_align(void) {
    TEST(align);
    if (golden_align(0, 32) != 0) { FAIL("align(0)"); return; }
    if (golden_align(1, 32) != 32) { FAIL("align(1)"); return; }
    if (golden_align(32, 32) != 32) { FAIL("align(32)"); return; }
    if (golden_align(33, 32) != 64) { FAIL("align(33)"); return; }
    PASS();
}

static void test_next_pow2(void) {
    TEST(next_pow2);
    if (golden_next_pow2(0) != 1) { FAIL("np2(0)"); return; }
    if (golden_next_pow2(1) != 1) { FAIL("np2(1)"); return; }
    if (golden_next_pow2(3) != 4) { FAIL("np2(3)"); return; }
    if (golden_next_pow2(16) != 16) { FAIL("np2(16)"); return; }
    if (golden_next_pow2(17) != 32) { FAIL("np2(17)"); return; }
    PASS();
}

static void test_modify_quant_code(void) {
    TEST(modify_quant_code);
    if (golden_modify_quant_code(0x18, 1) != 0x10) { FAIL("mqc1"); return; }
    if (golden_modify_quant_code(0x18, 10) != 0x00) { FAIL("mqc2"); return; }
    PASS();
}

static void test_quantize_code_word(void) {
    TEST(quantize_code_word);
    if (golden_quantize_code_word(0, 0) != 0) { FAIL("qcw1"); return; }
    uint32_t r = golden_quantize_code_word(0x00100000, 0);
    if ((r >> 20) != 1) { FAIL("qcw2"); return; }
    PASS();
}

static void test_compute_required_8x8_size(void) {
    TEST(compute_required_8x8_size);
    if (golden_compute_required_8x8_size(0) != 0) { FAIL("cr8(0)"); return; }
    /* 0x00000055 = planes=1 for subblocks 0-3, q_bits=0 -> 4*1=4 */
    if (golden_compute_required_8x8_size(0x00000055u) != 4) { FAIL("cr8(0x55)"); return; }
    /* 0x000000FF = planes=3 for subblocks 0-3, q_bits=0 -> 4*3=12 */
    if (golden_compute_required_8x8_size(0x000000FFu) != 12) { FAIL("cr8(0xFF)"); return; }
    /* All 8 subblocks planes=3, q_bits=0 -> 8*3=24 */
    if (golden_compute_required_8x8_size(0x0000FFFFu) != 24) { FAIL("cr8(0xFFFF)"); return; }
    PASS();
}

static void test_block_mapping(void) {
    TEST(block_mapping);
    int c8, c32;
    golden_block_mapping *m;
    golden_build_block_mapping(4, 4, &c8, &c32, &m);
    if (c8 != 16) { FAIL("bm c8"); free(m); return; }
    if (c32 != 1) { FAIL("bm c32"); free(m); return; }
    if (m[0].block_width_8x8 != 4 || m[0].block_height_8x8 != 4) { FAIL("bm dims"); free(m); return; }
    free(m);
    PASS();
}

static void test_block_mapping_non_square(void) {
    TEST(block_mapping_non_square);
    int c8, c32;
    golden_block_mapping *m;
    golden_build_block_mapping(5, 3, &c8, &c32, &m);
    if (c8 != 15) { FAIL("bms c8"); free(m); return; }
    if (c32 != 2) { FAIL("bms c32"); free(m); return; }
    if (m[0].block_width_8x8 != 4 || m[0].block_height_8x8 != 3) { FAIL("bms d0"); free(m); return; }
    if (m[1].block_width_8x8 != 1 || m[1].block_height_8x8 != 3) { FAIL("bms d1"); free(m); return; }
    free(m);
    PASS();
}

static void test_compute_num_packets(void) {
    TEST(compute_num_packets);
    golden_bitstream_packet meta[10];
    memset(meta, 0, sizeof(meta));
    if (golden_compute_num_packets(meta, 10, 4096, 0) != 1) { FAIL("cnp z"); return; }
    meta[0].num_words = 10;
    if (golden_compute_num_packets(meta, 10, 4096, 0) != 1) { FAIL("cnp 1"); return; }
    meta[0].num_words = 500;
    meta[5].num_words = 500;
    if (golden_compute_num_packets(meta, 10, 4096, 0) < 1) { FAIL("cnp s"); return; }
    PASS();
}

static void test_validate_packet(void) {
    TEST(validate_packet);
    uint32_t bs[16];
    memset(bs, 0, sizeof(bs));
    golden_bitstream_packet meta;
    memset(&meta, 0, sizeof(meta));
    meta.offset_u32 = 0;
    meta.num_words = 3;
    golden_bitstream_header *hdr = (golden_bitstream_header *)&bs[0];
    hdr->ballot = 1;
    hdr->payload_words = 3;
    hdr->block_index = 0;
    golden_block_mapping map;
    memset(&map, 0, sizeof(map));
    map.block_width_8x8 = 4;
    map.block_height_8x8 = 4;
    uint32_t ns;
    if (!golden_validate_packet(bs, &meta, 0, &map, &ns)) { FAIL("vp"); return; }
    PASS();
}

static void test_quant_zero_block(void) {
    TEST(quant_zero_block);
    float bd[64];
    memset(bd, 0, sizeof(bd));
    golden_block_meta meta[1];
    golden_block_stats stats[1];
    uint8_t payload[256];
    memset(payload, 0, sizeof(payload));
    golden_stage2_output out;
    memset(&out, 0, sizeof(out));
    out.meta = meta;
    out.stats = stats;
    out.payload = payload;
    out.payload_bytes = 0;
    out.block_count_8x8 = 1;
    golden_quant_params params;
    memset(&params, 0, sizeof(params));
    params.resolution_x = 8;
    params.resolution_y = 8;
    params.resolution_8x8_blocks_x = 1;
    params.resolution_8x8_blocks_y = 1;
    params.quant_resolution = 1.0f;
    params.rdo_distortion_scale = 1.0f;
    params.block_offset = 0;
    params.block_stride = 1;
    params.band_data = bd;
    golden_quant_block(0, 0, &params, &out);
    if (meta[0].code_word != 0) { FAIL("qzb cw"); return; }
    if (stats[0].num_planes != 0) { FAIL("qzb np"); return; }
    PASS();
}

static void test_quant_identity(void) {
    TEST(quant_identity);
    uint8_t code;
    float scale;
    golden_compute_quant_scale(0.5f, &code, &scale);
    if (code != 6) { FAIL("qi code"); return; }
    if (scale != 1.0f) { FAIL("qi scale"); return; }
    PASS();
}

int main(void) {
    printf("Golden model unit tests\n=======================\n\n");
    test_f16_roundtrip();
    test_quant_encode_decode();
    test_firstbithigh();
    test_popcount();
    test_align();
    test_next_pow2();
    test_modify_quant_code();
    test_quantize_code_word();
    test_compute_required_8x8_size();
    test_block_mapping();
    test_block_mapping_non_square();
    test_compute_num_packets();
    test_validate_packet();
    test_quant_zero_block();
    test_quant_identity();
    printf("\n=======================\n");
    printf("Results: %d run, %d passed, %d failed\n", tests_run, tests_passed, tests_failed);
    if (tests_failed == 0) { printf("PASS ALL\n"); return 0; }
    printf("FAIL SOME\n");
    return 1;
}
