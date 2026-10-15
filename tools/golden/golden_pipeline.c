// golden_pipeline.c -- Block mapping helper (R/pyrowave_common.cpp:191-211)

#include "golden_pipeline.h"
#include <stdlib.h>
#include <string.h>

void golden_build_block_mapping(int bx8, int by8,
                                int *count_8x8_out,
                                int *count_32x32_out,
                                golden_block_mapping **mapping_out) {
    int bx32 = (bx8 + 3) / 4;
    int by32 = (by8 + 3) / 4;
    int count = bx32 * by32;

    *mapping_out = (golden_block_mapping *)malloc(count * sizeof(golden_block_mapping));
    if (!*mapping_out) { *count_8x8_out = 0; *count_32x32_out = 0; return; }

    int b8 = 0, b32 = 0;
    for (int y = 0; y < by32; y++) {
        for (int x = 0; x < bx32; x++) {
            (*mapping_out)[b32].block_offset_8x8 = b8 + 4 * y * bx8 + 4 * x;
            (*mapping_out)[b32].block_stride_8x8 = bx8;
            (*mapping_out)[b32].block_width_8x8 = (bx8 - 4 * x < 4) ? (bx8 - 4 * x) : 4;
            (*mapping_out)[b32].block_height_8x8 = (by8 - 4 * y < 4) ? (by8 - 4 * y) : 4;
            b32++;
        }
    }
    b8 += bx8 * by8;

    *count_8x8_out = b8;
    *count_32x32_out = b32;
}
