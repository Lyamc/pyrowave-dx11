// golden_quant.c -- Stage 2: CPU golden model of quantization
// Mirrors R/shaders/wavelet_quant.comp (wq:N), R/shaders/dwt_quant_scale.h

#include "golden_common.h"
#include <stdlib.h>
#include <string.h>

/* Per-block quantization parameters */
typedef struct golden_quant_params {
    int resolution_x, resolution_y;
    int resolution_8x8_blocks_x, resolution_8x8_blocks_y;
    float quant_resolution;
    int block_offset;
    int block_stride;
    float rdo_distortion_scale;
    const float *band_data;
} golden_quant_params;

/* Output buffers for stage 2 */
typedef struct golden_stage2_output {
    golden_block_meta *meta;
    golden_block_stats *stats;
    uint8_t *payload;
    int payload_bytes;
    int block_count_8x8;
} golden_stage2_output;

/* Compute quant scale: wq:95-111 */
void golden_compute_quant_scale(float max_wave_texels, uint8_t *quant_code, float *quant_scale) {
    if (max_wave_texels < 1.0f) {
        *quant_code = 6;
        *quant_scale = 1.0f;
    } else {
        float y = max_wave_texels - 0.25f;
        uint32_t yv;
        memcpy(&yv, &y, 4);
        int e = (int)((yv >> 23) & 0xff) - 126;
        float target_max = (float)(1 << e) - 0.25f;
        float inv_scale = max_wave_texels / target_max;
        *quant_code = (uint8_t)golden_encode_quant_scale(inv_scale);
        *quant_scale = 1.0f / golden_decode_quant_scale(*quant_code);
    }
}

/* Process one 8x8 block (wq:172-290 simplified for CPU) */
void golden_quant_block(int x8, int y8, const golden_quant_params *params,
                        golden_stage2_output *out) {
    int bx = params->resolution_8x8_blocks_x;
    int by = params->resolution_8x8_blocks_y;
    if (x8 >= bx || y8 >= by) return;

    int block_index = params->block_offset + y8 * params->block_stride + x8;
    int base_x = x8 * 8;
    int base_y = y8 * 8;
    int width = params->resolution_x;

    /* Read 8 texels at unswizzled positions: (0,0),(2,0),(4,0),(6,0),(0,2),(2,2),(4,2),(6,2) */
    float texels[8];
    int positions[8][2] = {{0,0},{2,0},{4,0},{6,0},{0,2},{2,2},{4,2},{6,2}};
    for (int i = 0; i < 8; i++) {
        int px = base_x + positions[i][0];
        int py = base_y + positions[i][1];
        if (px < 0 || px >= width || py < 0 || py >= params->resolution_y) {
            texels[i] = 0.0f;
        } else {
            texels[i] = params->band_data[py * width + px];
        }
    }

    /* Scale by quant_resolution */
    for (int i = 0; i < 8; i++) texels[i] *= params->quant_resolution;

    /* max of abs(texels) */
    float max_sub = 0.0f, max_wave = 0.0f;
    for (int i = 0; i < 8; i++) { float a = fabsf(texels[i]); if (a > max_sub) max_sub = a; }
    max_wave = max_sub;

    /* Quant code and scale */
    uint8_t quant_code;
    float quant_scale;
    golden_compute_quant_scale(max_wave, &quant_code, &quant_scale);

    /* Scale by quant_scale */
    for (int i = 0; i < 8; i++) texels[i] *= quant_scale;
    max_wave *= quant_scale;

    float inv_quant = 1.0f / (params->quant_resolution * quant_scale);
    float inv_quant_sq = inv_quant * inv_quant;

    int abs_quant[8];
    for (int i = 0; i < 8; i++) abs_quant[i] = (int)fabsf(texels[i]);

    int max_abs = (int)max_wave;

    /* Zero block */
    if (max_abs == 0) {
        out->meta[block_index].code_word = 0;
        out->meta[block_index].offset = 0;
        out->stats[block_index].num_planes = 0;
        out->stats[block_index].stats[0].square_error_fp16 = golden_f32tof16(0.0f);
        out->stats[block_index].stats[0].encode_cost_bits = 0;
        return;
    }

    int msb = golden_firstbithigh((uint32_t)max_abs);

    /* Compute stats for q=0 */
    float pow2q = golden_asfloat((uint32_t)((127 + 0) << 23));
    float inv_pow2q = golden_asfloat((uint32_t)((127 - 0) << 23));

    int num_sig = 0;
    float err_sum = 0.0f;
    float iv[8];
    for (int i = 0; i < 8; i++) {
        float quantized = floorf(fabsf(texels[i]) * inv_pow2q);
        if (quantized != 0.0f) { quantized += 0.5f; num_sig++; }
        quantized = floorf(quantized * pow2q);
        iv[i] = quantized;
        float err = fabsf(texels[i]) - iv[i];
        err_sum += err * err;
    }

    /* early cost: block4x2_max = max_abs, shifted = max_abs >> 0 = max_abs */
    int block4x2_shifted = max_abs;
    int early = block4x2_shifted > 0 ? 1 : 0;
    int msb_q = msb - 0;
    int quality_planes = 0;
    if (msb_q >= 3) {
        quality_planes = msb_q - 2;
        early = quality_planes + 1;
        block4x2_shifted >>= quality_planes;
    }
    early += golden_firstbithigh((uint32_t)block4x2_shifted) + 1;

    int late_bits = 8 * ((early > 1) ? (early - 1) : 0) + num_sig;

    /* Scan (exclusive) = early for single block */
    int scan = early;
    int global_offset = out->payload_bytes;
    out->payload_bytes += scan;
    scan -= early;

    /* Code word */
    uint32_t code_word = (uint32_t)quality_planes << 16;
    code_word |= ((uint32_t)quant_code) << 20;
    int plane_code = golden_firstbithigh((uint32_t)block4x2_shifted) + 1;
    code_word |= ((uint32_t)plane_code) << 0;

    /* Write meta */
    out->meta[block_index].code_word = code_word;
    out->meta[block_index].offset = (uint32_t)global_offset;
    out->stats[block_index].num_planes = (uint32_t)(msb + 1);
    out->stats[block_index].stats[0].square_error_fp16 = golden_f32tof16(0.0f);
    out->stats[block_index].stats[0].encode_cost_bits = (uint16_t)late_bits;

    /* Per-q stats q=1..msb */
    for (int q = 1; q <= msb; q++) {
        pow2q = golden_asfloat((uint32_t)((127 + q) << 23));
        inv_pow2q = golden_asfloat((uint32_t)((127 - q) << 23));

        int ns = 0;
        float es = 0.0f;
        for (int i = 0; i < 8; i++) {
            float quantized = floorf(fabsf(texels[i]) * inv_pow2q);
            if (quantized != 0.0f) { quantized += 0.5f; ns++; }
            quantized = floorf(quantized * pow2q);
            iv[i] = quantized;
            float err = fabsf(texels[i]) - quantized;
            es += err * err;
        }
        es *= params->rdo_distortion_scale * inv_quant_sq;

        int b4s = max_abs >> q;
        int e2 = b4s > 0 ? 1 : 0;
        int msb2 = msb - q;
        int qp2 = 0;
        if (msb2 >= 3) { qp2 = msb2 - 2; e2 = qp2 + 1; b4s >>= qp2; }
        e2 += golden_firstbithigh((uint32_t)b4s) + 1;
        int lb2 = 8 * ((e2 > 1) ? (e2 - 1) : 0) + ns;

        out->stats[block_index].stats[q].square_error_fp16 = golden_f32tof16(fminf(es, 60000.0f));
        out->stats[block_index].stats[q].encode_cost_bits = (uint16_t)lb2;
    }

    /* errors[msb+1]: no rdo_distortion_scale (OQ-6) */
    float total_sq = 0.0f;
    for (int i = 0; i < 8; i++) total_sq += texels[i] * texels[i];
    total_sq *= inv_quant_sq;
    out->stats[block_index].stats[msb + 1].square_error_fp16 =
        golden_f32tof16(fminf(60000.0f, total_sq));
    out->stats[block_index].stats[msb + 1].encode_cost_bits = 0;

    /* Payload */
    int need_sign = block4x2_shifted != 0 || quality_planes != 0;
    int byte_off = global_offset;

    if (need_sign) {
        uint8_t sign_byte = 0;
        for (int k = 0; k < 8; k++) if (texels[k] < 0.0f) sign_byte |= (uint8_t)(1u << k);
        out->payload[byte_off++] = sign_byte;

        int plane_iterations = quality_planes + plane_code;
        for (int p = plane_iterations - 1; p >= 0; p--) {
            uint8_t plane_byte = 0;
            for (int k = 0; k < 8; k++) {
                int bit = (abs_quant[k] >> p) & 1;
                plane_byte |= (uint8_t)(bit << k);
            }
            out->payload[byte_off++] = plane_byte;
        }
    }
}
