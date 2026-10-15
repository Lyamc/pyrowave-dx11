// golden_pipeline.h -- Public API for the CPU golden model (Stages 2-6)

#ifndef GOLDEN_PIPELINE_H
#define GOLDEN_PIPELINE_H

#include "golden_common.h"

/* Stage 2 */
typedef struct golden_band {
    float *data;
    int width, height;
} golden_band;

typedef struct golden_quant_params {
    int resolution_x, resolution_y;
    int resolution_8x8_blocks_x, resolution_8x8_blocks_y;
    float quant_resolution;
    int block_offset;
    int block_stride;
    float rdo_distortion_scale;
    const float *band_data;
} golden_quant_params;

typedef struct golden_stage2_output {
    golden_block_meta *meta;
    golden_block_stats *stats;
    uint8_t *payload;
    int payload_bytes;
    int block_count_8x8;
} golden_stage2_output;

void golden_quant_block(int x8, int y8, const golden_quant_params *params,
                        golden_stage2_output *out);
void golden_compute_quant_scale(float max_wave_texels, uint8_t *qc, float *qs);

/* Stage 3 */
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

void golden_analyze_stage(const golden_analyze_params *params,
                          golden_analyze_bucket_buffer *bucket,
                          int windowed_finalize);

/* Stage 4 */
typedef struct golden_resolve_params {
    const uint32_t *total_savings;
    const golden_rd_operation *rdo_ops;
    const uint32_t *bucket_consumed;
    uint32_t target_payload_size;
    int per_sub;
    int num_blocks_32x32;
    int *quant;
} golden_resolve_params;

void golden_resolve_stage(const golden_resolve_params *params);

/* Stage 5 */
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

void golden_pack_stage(const golden_pack_params *params);

/* Stage 6 */
typedef struct golden_packet {
    size_t offset;
    size_t size;
} golden_packet;

size_t golden_compute_num_packets(const golden_bitstream_packet *meta,
                                   int block_count_32x32,
                                   size_t packet_boundary,
                                   size_t padding_size);
size_t golden_packetize(const golden_bitstream_packet *meta,
                        const uint32_t *bitstream_u32,
                        int block_count_32x32,
                        size_t packet_boundary,
                        size_t padding_size,
                        void *output_,
                        size_t output_size);
int golden_validate_packet(const uint32_t *bitstream_u32,
                           const golden_bitstream_packet *meta,
                           uint32_t block_index,
                           const golden_block_mapping *mapping,
                           uint32_t *out_num_sig);
size_t golden_packetize_stage(const golden_bitstream_packet *meta,
                               const uint32_t *bitstream_u32,
                               int block_count_32x32,
                               size_t packet_boundary,
                               size_t padding_size,
                               void *output_,
                               size_t output_size);

/* Block mapping */
void golden_build_block_mapping(int bx8, int by8,
                                int *count_8x8_out,
                                int *count_32x32_out,
                                golden_block_mapping **mapping_out);

#endif /* GOLDEN_PIPELINE_H */
