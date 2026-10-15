// golden_analyze.c -- Stage 3: CPU golden model of rate-control analysis
// Mirrors R/shaders/analyze_rate_control.comp (ar:N), analyze_rate_control_finalize.comp (arf:N)

#include "golden_common.h"
#include <stdlib.h>
#include <string.h>

/* Bucket buffer */
typedef struct golden_analyze_bucket_buffer {
    uint32_t *total_savings;
    golden_rd_operation *rdo_ops;
    uint32_t consumed_payload;
    int num_blocks_32x32;
    int num_blocks_aligned;
    int per_sub;
} golden_analyze_bucket_buffer;

typedef struct golden_analyze_params {
    const golden_block_stats *block_stats;
    int resolution_8x8_bx, resolution_8x8_by;
    int block_offset_8x8, block_stride_8x8;
    int block_offset_32x32, block_stride_32x32;
    int block_count_32x32;
    int num_blocks_aligned;
    int block_index_shamt;
} golden_analyze_params;

/* Butterfly float sum over 16 values */
static float golden_analyze_butterfly(float vals[16]) {
    float d[16];
    memcpy(d, vals, sizeof(d));
    for (int s = 1; s < 16; s *= 2) {
        for (int i = 0; i < 16; i++) {
            if (i & s) d[i] += d[i ^ s];
        }
    }
    return d[15];
}

/* Hillis-Steele max-scan over 16 values */
static void golden_analyze_max_scan(int vals[16]) {
    for (int s = 1; s < 16; s *= 2) {
        for (int t = 0; t < 16; t++) {
            if (t >= s) {
                int cand = vals[t - s] + s;
                if (cand > vals[t]) vals[t] = cand;
            }
        }
    }
}

void golden_analyze_32x32_block(int gx, int gy,
                                 int res_8x8_bx, int res_8x8_by,
                                 const golden_block_stats *block_stats,
                                 int boff8, int bstr8,
                                 int boff32, int bstr32,
                                 int nb32, int nb_aligned, int bshamt,
                                 golden_analyze_bucket_buffer *bucket) {
    int blk32 = boff32 + gy * bstr32 + gx;
    if (blk32 >= nb32) return;
    int sub = blk32 >> bshamt;

    int bx32 = (res_8x8_bx + 3) / 4;
    int by32 = (res_8x8_by + 3) / 4;
    if (gx >= bx32 || gy >= by32) return;

    /* Per-i cost and dist (summed over 4 sub-blocks) */
    int rate_int[16] = {0};
    float dist_per_i[16] = {0};

    for (int i = 0; i < 16; i++) {
        float d_sum = 0.0f;
        int c_sum = 0;
        for (int t = 0; t < 4; t++) {
            int idx = i * 4 + t;
            int lx = idx & 3, ly = idx >> 2;
            int b8 = boff8 + (gy * 4 + ly) * bstr8 + (gx * 4 + lx);
            int in8 = (gx * 4 + lx < res_8x8_bx) && (gy * 4 + ly < res_8x8_by);
            if (!in8) continue;

            int np = block_stats[b8].num_planes;
            int ei = (i < np) ? i : np;
            uint32_t e = (uint32_t)block_stats[b8].stats[ei].encode_cost_bits
                       | ((uint32_t)block_stats[b8].stats[ei].square_error_fp16 << 16);
            float d = golden_f16tof32((uint16_t)(e & 0xffff));
            int c = (int)(e >> 16);
            if (c != 0) c += 24;
            d_sum += d;
            c_sum += c;
        }
        dist_per_i[i] = d_sum;
        rate_int[i] = c_sum;
    }

    /* Butterfly sum of dist */
    (void)golden_analyze_butterfly(dist_per_i);

    /* Total cost */
    int total_cost = 0;
    for (int i = 0; i < 16; i++) total_cost += rate_int[i];
    if (total_cost != 0) total_cost += 64;

    /* rate[i] = (rate_int[i] + 31) >> 5 */
    float rate[16];
    for (int i = 0; i < 16; i++) rate[i] = (float)((rate_int[i] + 31) >> 5);

    float rate0 = rate[0];

    /* Compute bucket_raw[t] for t=0..15 */
    int bucket_raw[16];
    for (int t = 0; t < 16; t++) {
        if (t == 0) { bucket_raw[t] = 0; continue; }
        float ct = rate[t];
        if (ct == rate0) { bucket_raw[t] = 0; continue; }
        float diff = dist_per_i[t] - dist_per_i[0];
        float denom = rate0 - ct;
        float log2val = (diff > 0 && denom > 0) ? log2f(diff / denom) : 0.0f;
        int bt = (int)(60.0f + 2.0f * log2val + 0.5f);
        if (bt < 0) bt = 0;
        if (bt > 127) bt = 127;
        bucket_raw[t] = bt;
    }

    /* Hillis-Steele max-scan: v = min(bucket_raw[t], 112+t), then scan */
    int v_scan[16];
    for (int t = 0; t < 16; t++) {
        v_scan[t] = bucket_raw[t] < (112 + t) ? bucket_raw[t] : (112 + t);
    }
    golden_analyze_max_scan(v_scan);

    /* consumed_payload += rate[0] (only for first 32x32 block) */
    if (gx == 0 && gy == 0) {
        bucket->consumed_payload += (uint32_t)rate_int[0];
    }

    /* Emit RDO operations for t=1..15 */
    for (int t = 1; t < 16; t++) {
        int saving = rate_int[t - 1] - rate_int[t]; /* unsigned wrap */
        if (saving == 0) continue;

        int v_final = v_scan[t];
        int savings_idx = v_final * 16 + sub;
        if (savings_idx >= 0 && savings_idx < 2048) {
            bucket->total_savings[savings_idx] += (uint32_t)saving;
        }

        int op_idx = blk32 + v_final * nb_aligned;
        if (op_idx >= 0) {
            bucket->rdo_ops[op_idx].quant = t;
            bucket->rdo_ops[op_idx].block_offset = (uint16_t)blk32;
            bucket->rdo_ops[op_idx].block_saving = (uint16_t)((uint32_t)saving & 0xffff);
        }
    }
}

/* Finalize: windowed prefix sum (arf:14-39) */
void golden_analyze_finalize(golden_analyze_bucket_buffer *bucket, int windowed) {
    int n = windowed ? 256 : 512;
    int vals[256];
    for (int t = 0; t < 256; t++) {
        vals[t] = (int)bucket->total_savings[t];
    }
    for (int step = 1; step < n; step *= 2) {
        for (int t = 0; t < 256; t++) {
            if (t >= step) vals[t] += vals[t - step];
        }
    }
    for (int t = 0; t < 256; t++) {
        if (t < 2048) bucket->total_savings[t] = (uint32_t)vals[t];
    }
}

void golden_analyze_stage(const golden_analyze_params *params,
                          golden_analyze_bucket_buffer *bucket,
                          int windowed_finalize) {
    int bx32 = (params->resolution_8x8_bx + 3) / 4;
    int by32 = (params->resolution_8x8_by + 3) / 4;
    for (int gy = 0; gy < by32; gy++) {
        for (int gx = 0; gx < bx32; gx++) {
            golden_analyze_32x32_block(gx, gy,
                params->resolution_8x8_bx, params->resolution_8x8_by,
                params->block_stats,
                params->block_offset_8x8, params->block_stride_8x8,
                params->block_offset_32x32, params->block_stride_32x32,
                params->block_count_32x32,
                params->num_blocks_aligned, params->block_index_shamt,
                bucket);
        }
    }
    golden_analyze_finalize(bucket, windowed_finalize);
}
