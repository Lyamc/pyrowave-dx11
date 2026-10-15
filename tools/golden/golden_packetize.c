// golden_packetize.c -- Stage 6: CPU golden model of packetization
// Mirrors R/pyrowave_encoder.cpp:604-639, 1090-1158

#include "golden_common.h"
#include <stdlib.h>
#include <string.h>

typedef struct golden_packet {
    size_t offset;
    size_t size;
} golden_packet;

size_t golden_compute_num_packets(const golden_bitstream_packet *meta,
                                   int block_count_32x32,
                                   size_t packet_boundary,
                                   size_t padding_size) {
    size_t num_packets = 0;
    size_t size_in_packet = sizeof(golden_bitstream_sequence_header);
    for (int i = 0; i < block_count_32x32; i++) {
        size_t ps = meta[i].num_words * sizeof(uint32_t);
        if (!ps) continue;
        if (size_in_packet + ps + padding_size > packet_boundary) {
            size_in_packet = 0;
            padding_size = 0;
            num_packets++;
        }
        size_in_packet += ps;
    }
    if (size_in_packet) num_packets++;
    return num_packets;
}

size_t golden_packetize(const golden_bitstream_packet *meta,
                        const uint32_t *bitstream_u32,
                        int block_count_32x32,
                        size_t packet_boundary,
                        size_t padding_size,
                        void *output_,
                        size_t output_size) {
    uint8_t *output = (uint8_t *)output_;
    size_t num_packets = 0;
    size_t size_in_packet = 0;
    size_t packet_offset = 0;
    size_t output_offset = 0;

    int num_non_zero = 0;
    for (int i = 0; i < block_count_32x32; i++) {
        if (meta[i].num_words != 0) num_non_zero++;
    }

    uint32_t sequence = 0;
    if (meta[0].num_words != 0) {
        const golden_bitstream_header *hdr =
            (const golden_bitstream_header *)(bitstream_u32 + meta[0].offset_u32);
        sequence = hdr->sequence;
    }

    golden_bitstream_sequence_header header;
    memset(&header, 0, sizeof(header));
    header.sequence = (uint32_t)sequence;
    header.extended = 1;
    header.code = 0;
    header.total_blocks = (uint32_t)num_non_zero;
    header.chroma_resolution = 0;

    if (output_size >= sizeof(header)) {
        memcpy(output, &header, sizeof(header));
    }
    output_offset += sizeof(header);
    size_in_packet += sizeof(header);

    for (int i = 0; i < block_count_32x32; i++) {
        size_t ps = meta[i].num_words * sizeof(uint32_t);
        if (!ps) continue;
        if (size_in_packet + ps + padding_size > packet_boundary) {
            num_packets++;
            size_in_packet = 0;
            padding_size = 0;
            packet_offset = output_offset;
        }
        if (output_offset + ps <= output_size) {
            memcpy(output + output_offset, bitstream_u32 + meta[i].offset_u32, ps);
        }
        output_offset += ps;
        size_in_packet += ps;
    }
    if (size_in_packet) num_packets++;

    return num_packets;
}

int golden_validate_packet(const uint32_t *bitstream_u32,
                           const golden_bitstream_packet *meta,
                           uint32_t block_index,
                           const golden_block_mapping *mapping,
                           uint32_t *out_num_sig) {
    if (meta[block_index].num_words == 0) return 1;

    const uint32_t *bs = bitstream_u32 + meta[block_index].offset_u32;
    const golden_bitstream_header *hdr = (const golden_bitstream_header *)bs;

    if (hdr->block_index != block_index) return 0;
    if (hdr->payload_words != meta[block_index].num_words) return 0;

    int blocks_8x8 = golden_popcount(hdr->ballot);
    const uint8_t *bs8 = (const uint8_t *)bs;
    const uint16_t *cw16 = (const uint16_t *)(bs + 2);
    const uint8_t *qw8 = (const uint8_t *)(cw16 + blocks_8x8);

    uint32_t offset = sizeof(golden_bitstream_header) + (uint32_t)(3 * blocks_8x8);
    if (offset > hdr->payload_words * 4u) return 0;

    int num_sig = 0;
    uint32_t ballot = hdr->ballot;
    while (ballot) {
        uint32_t bit = (uint32_t)(golden_popcount((ballot & ~(ballot - 1)) - 1));
        ballot &= ~(1u << bit);
        int x = (int)(bit & 3), y = (int)(bit >> 2);
        if (x < mapping->block_width_8x8 && y < mapping->block_height_8x8) {
            uint8_t qb = *qw8 & 0xf;
            for (int sb = 0; sb < 16; sb += 2) {
                int np = qb + ((int)(*cw16 >> sb) & 3);
                int psig = 0;
                for (int p = 0; p < np; p++) psig |= bs8[offset++];
                num_sig += golden_popcount((uint32_t)psig);
            }
            cw16++;
            qw8++;
        } else {
            return 0;
        }
    }

    offset += (uint32_t)((num_sig + 7) / 8);
    uint32_t offset_words = (offset + 3) / 4;

    if (out_num_sig) *out_num_sig = (uint32_t)num_sig;
    return (int)(offset_words == hdr->payload_words);
}

size_t golden_packetize_stage(const golden_bitstream_packet *meta,
                               const uint32_t *bitstream_u32,
                               int block_count_32x32,
                               size_t packet_boundary,
                               size_t padding_size,
                               void *output_,
                               size_t output_size) {
    return golden_packetize(meta, bitstream_u32, block_count_32x32,
                            packet_boundary, padding_size, output_, output_size);
}
