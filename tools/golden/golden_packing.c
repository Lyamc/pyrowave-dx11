// golden_packing.c -- Stage 5: CPU golden model of block packing
// Mirrors R/shaders/block_packing.comp (bp:N)

#include "golden_common.h"
#include <stdlib.h>
#include <string.h>

typedef struct golden_bp_output {
    uint8_t *bitstream;
    golden_bitstream_packet *meta;
    const golden_block_meta *block_meta;
    const golden_block_stats *block_stats;
    const uint8_t *payload;
    const uint32_t *quant;
    int block_offset_32x32;
    int block_stride_32x32;
    int block_offset_8x8;
    int block_stride_8x8;
    int resolution_32x32_bx, resolution_32x32_by;
    int resolution_8x8_bx, resolution_8x8_by;
    uint32_t quant_resolution_code;
    uint32_t sequence_count;
    int payload_counter;
} golden_bp_output;

void golden_pack_32x32_block(int group_x, int group_y,
                               golden_bp_output *out) {
    int blocks_32x32_x = out->resolution_32x32_bx;
    int blocks_32x32_y = out->resolution_32x32_by;

    for (int blk32_local = 0; blk32_local < 4; blk32_local++) {
        int x32 = group_x * 2 + (blk32_local & 1);
        int y32 = group_y * 2 + (blk32_local >> 1);
        int blk32_global = out->block_offset_32x32 + y32 * out->block_stride_32x32 + x32;
        int in32 = (x32 < blocks_32x32_x) && (y32 < blocks_32x32_y);
        if (!in32) continue;

        int quant = (int)out->quant[blk32_global];

        uint32_t ballot = 0;
        int plane_bytes[16] = {0};
        int sign_bits[16] = {0};
        int bits_meta[16] = {0};
        int active[16] = {0};
        int cw[16] = {0};
        int b8_arr[16] = {0};

        for (int lb = 0; lb < 16; lb++) {
            int lx = lb & 3, ly = lb >> 2;
            int x8 = x32 * 4 + lx, y8 = y32 * 4 + ly;
            b8_arr[lb] = out->block_offset_8x8 + y8 * out->block_stride_8x8 + x8;
            int in8 = (x8 < out->resolution_8x8_bx) && (y8 < out->resolution_8x8_by);
            if (!in8) { active[lb] = 0; continue; }

            uint32_t meta_cw = out->block_meta[b8_arr[lb]].code_word;
            int np = (int)out->block_stats[b8_arr[lb]].num_planes;
            int si = (np < quant) ? np : quant;
            uint32_t e = (uint32_t)out->block_stats[b8_arr[lb]].stats[si].encode_cost_bits
                       | ((uint32_t)out->block_stats[b8_arr[lb]].stats[si].square_error_fp16 << 16);
            int nb = (int)(e >> 16);
            cw[lb] = (int)golden_quantize_code_word(meta_cw, quant);
            active[lb] = ((cw[lb] & 0xffff) != 0) ? 1 : 0;
            plane_bytes[lb] = golden_compute_required_8x8_size((uint32_t)cw[lb]);
            sign_bits[lb] = nb - plane_bytes[lb] * 8;
            if (sign_bits[lb] < 0) sign_bits[lb] = 0;
            bits_meta[lb] = nb != 0 ? nb + 24 : 0;
            if (active[lb]) ballot |= (uint32_t)(1u << lb);
        }

        int N = golden_popcount(ballot);
        int total_bm = 0;
        for (int lb = 0; lb < 16; lb++) total_bm += bits_meta[lb];
        int words = total_bm / 32;
        if (words != 0) words += 2;

        int base = out->payload_counter;
        out->payload_counter += words;

        out->meta[blk32_global].offset_u32 = (uint32_t)base;
        out->meta[blk32_global].num_words = (uint32_t)words;

        if (words == 0) continue;

        int hdr_off = base * 4;
        uint32_t word0 = ballot;
        word0 |= ((uint32_t)words & 0xfff) << 16;
        word0 |= ((uint32_t)(out->sequence_count & 7)) << 28;

        uint32_t word1 = (uint32_t)golden_modify_quant_code(
            (uint8_t)out->quant_resolution_code, quant);
        word1 |= ((uint32_t)blk32_global) << 8;

        memcpy(out->bitstream + hdr_off, &word0, 4);
        memcpy(out->bitstream + hdr_off + 4, &word1, 4);

        int data_off = hdr_off + 8;
        int poff_arr[16], poff = 0;
        for (int lb = 0; lb < 16; lb++) { poff_arr[lb] = poff; poff += plane_bytes[lb]; }

        int hdr_bit = 0;
        for (int lb = 0; lb < 16; lb++) {
            if (!(ballot & (1u << lb))) continue;
            uint16_t cw16 = (uint16_t)(cw[lb] & 0xffff);
            memcpy(out->bitstream + data_off + 2 * hdr_bit, &cw16, 2);
            uint8_t qbyte = (uint8_t)((cw[lb] >> 16) & 0xff);
            memcpy(out->bitstream + data_off + 2 * N + hdr_bit, &qbyte, 1);
            hdr_bit++;
        }

        int plane_data_off = data_off + 3 * N;

        for (int lb = 0; lb < 16; lb++) {
            if (!(ballot & (1u << lb))) continue;
            uint32_t orig_cw = out->block_meta[b8_arr[lb]].code_word;
            int q_bits = (int)((uint32_t)cw[lb] >> 20) & 0xf;
            int in_off = (int)out->block_meta[b8_arr[lb]].offset;
            for (int sb = 0; sb < 8; sb++) {
                int orig_planes = (int)((orig_cw >> (2 * sb)) & 3);
                int out_planes = (int)((cw[lb] >> (2 * sb)) & 3) + q_bits;
                if (out_planes > 0) {
                    for (int p = 0; p < out_planes; p++) {
                        if (in_off + p < out->payload_counter) {
                            out->bitstream[plane_data_off + poff_arr[lb] + p] =
                                out->payload[in_off + p];
                        }
                    }
                }
                in_off += orig_planes + ((orig_planes > 0) ? 1 : 0);
            }
        }
    }
}

typedef struct golden_pack_params {
    const golden_block_meta *block_meta;
    const golden_block_stats *block_stats;
    const uint8_t *payload;
    const uint32_t *quant;
    int resolution_32x32_bx, resolution_32x32_by;
    int resolution_8x8_bx, resolution_8x8_by;
    int block_offset_32x32, block_stride_32x32;
    int block_offset_8x8, block_stride_8x8;
    uint32_t quant_resolution_code;
    uint32_t sequence_count;
    uint8_t *bitstream;
    golden_bitstream_packet *meta;
    int bitstream_size;
} golden_pack_params;

void golden_pack_stage(const golden_pack_params *params) {
    int bx32 = (params->resolution_8x8_bx + 31) / 32;
    int by32 = (params->resolution_8x8_by + 31) / 32;
    int gx = (bx32 + 1) / 2, gy = (by32 + 1) / 2;

    golden_bp_output out;
    memset(&out, 0, sizeof(out));
    out.block_meta = params->block_meta;
    out.block_stats = params->block_stats;
    out.payload = params->payload;
    out.quant = params->quant;
    out.block_offset_32x32 = params->block_offset_32x32;
    out.block_stride_32x32 = params->block_stride_32x32;
    out.block_offset_8x8 = params->block_offset_8x8;
    out.block_stride_8x8 = params->block_stride_8x8;
    out.resolution_32x32_bx = bx32;
    out.resolution_32x32_by = by32;
    out.resolution_8x8_bx = params->resolution_8x8_bx;
    out.resolution_8x8_by = params->resolution_8x8_by;
    out.quant_resolution_code = params->quant_resolution_code;
    out.sequence_count = params->sequence_count;
    out.bitstream = params->bitstream;
    out.meta = params->meta;
    out.payload_counter = 0;

    for (int ggy = 0; ggy < gy; ggy++) {
        for (int ggx = 0; ggx < gx; ggx++) {
            golden_pack_32x32_block(ggx, ggy, &out);
        }
    }
}
