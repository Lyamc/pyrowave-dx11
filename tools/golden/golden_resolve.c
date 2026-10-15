// golden_resolve.c -- Stage 4: CPU golden model of rate-control resolve
// Mirrors R/shaders/resolve_rate_control.comp (rr:N)

#include "golden_common.h"
#include <stdlib.h>
#include <string.h>

typedef struct golden_resolve_params {
    const uint32_t *total_savings;
    const golden_rd_operation *rdo_ops;
    const uint32_t *bucket_consumed;
    uint32_t target_payload_size;
    int per_sub;
    int num_blocks_32x32;
    int *quant;
} golden_resolve_params;

void golden_resolve_stage(const golden_resolve_params *params) {
    int num_entries = GOLDEN_NUM_RDO_BUCKETS * GOLDEN_BLOCK_SPACE_SUBDIVISION;
    uint32_t consumed = params->bucket_consumed ? params->bucket_consumed[0] : 0;
    uint32_t req_base = consumed - params->target_payload_size;

    for (int x = 0; x < num_entries; x++) {
        uint32_t prev = (x > 0) ? params->total_savings[x - 1] : 0;
        uint32_t total_x = params->total_savings[x];
        if (total_x == prev) continue;

        uint32_t req = (x == 0) ? req_base : (req_base - prev);
        if (req <= 0) continue;

        uint32_t total_saved = 0;
        for (int i = 0; i < params->per_sub; i += 64) {
            int saving_arr[64] = {0};
            int incl_arr[64] = {0};
            for (int t = 0; t < 64; t++) {
                int idx = i + t;
                if (idx < params->per_sub) {
                    int opi = x * params->per_sub + idx;
                    saving_arr[t] = (int)params->rdo_ops[opi].block_saving;
                }
            }
            int running = 0;
            for (int t = 0; t < 64; t++) {
                running += saving_arr[t];
                incl_arr[t] = running;
            }
            for (int t = 0; t < 64; t++) {
                int idx = i + t;
                if (idx >= params->per_sub) break;
                int opi = x * params->per_sub + idx;
                int saving = saving_arr[t];
                int blk = (int)params->rdo_ops[opi].block_offset;
                int qv = (int)params->rdo_ops[opi].quant;
                int incl = incl_arr[t];
                if (saving != 0 && (total_saved + (uint32_t)incl - (uint32_t)saving) < req) {
                    if (blk >= 0 && blk < params->num_blocks_32x32) {
                        if (qv > params->quant[blk]) params->quant[blk] = qv;
                    }
                }
            }
            total_saved += (uint32_t)incl_arr[63];
        }
    }
}
