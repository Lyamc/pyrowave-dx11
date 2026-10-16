// pack.hlsl -- Block packing: produce bitstream bytes
// Port of R/shaders/block_packing.comp to cs_5_0
//
// Spec: docs/stages/05_block_packing.md
// Source: R/shaders/block_packing.comp

#include "common.hlsli"

// ------------------------------------------------------------------
// Constants
// ------------------------------------------------------------------
#define Q_PLANES_OFFSET 16
#define Q_PLANES_BITS 4
#define MAX_PACKET_WORDS 640  // Max words per 32x32 block

// ------------------------------------------------------------------
// Push constants (matches BlockPackingPushData, 48 bytes)
// ------------------------------------------------------------------
cbuffer PushConstants : register(b0) {
    int   resolution_x;               // @0
    int   resolution_y;               // @4
    int   resolution_32x32_blocks_x;  // @8
    int   resolution_32x32_blocks_y;  // @12
    int   resolution_8x8_blocks_x;    // @16
    int   resolution_8x8_blocks_y;    // @20
    uint  quant_resolution_code;      // @24
    uint  sequence_code;              // @28
    int   block_offset_32x32;         // @32
    int   block_stride_32x32;         // @36
    int   block_offset_8x8;           // @40
    int   block_stride_8x8;           // @44
};

// ------------------------------------------------------------------
// Resources
// ------------------------------------------------------------------
// u0: RWByteAddressBuffer = bitstream (packets)
RWByteAddressBuffer u0 : register(u0);

// u1: RWByteAddressBuffer = bitstream meta (BitstreamPacket per block)
RWByteAddressBuffer u1 : register(u1);

// t0: ByteAddressBuffer = block meta (stage 2)
ByteAddressBuffer t0 : register(t0);

// u2: RWByteAddressBuffer = payloads (counter @4, data @8)
RWByteAddressBuffer u2 : register(u2);

// t1: ByteAddressBuffer = block stats (stage 2)
ByteAddressBuffer t1 : register(t1);

// t2: ByteAddressBuffer = quant buffer (stage 4)
ByteAddressBuffer t2 : register(t2);

// ------------------------------------------------------------------
// Groupshared memory (global scope)
// ------------------------------------------------------------------
// gs_pkt: staging packet per 32x32 block. 4 blocks x 640 words = 2560 uints = 10 KB
groupshared uint gs_pkt[4 * MAX_PACKET_WORDS];

// gs_ballot: 4 uints per 32x32 block (16 bits each, stored in lower 16 bits)
groupshared uint gs_ballot[4];

// Per-lane data (64 threads x 16 lanes per block)
groupshared uint gs_plane_bytes[64];
groupshared uint gs_sign_bits[64];
groupshared uint gs_bits_meta[64];

// gs_base: broadcast base from lane 15
groupshared uint gs_base[4];

// Debug overflow counter
groupshared uint gs_overflow[1];

// ------------------------------------------------------------------
// Helper: compute_required_8x8_size (bp:97-105)
// ------------------------------------------------------------------
uint compute_required_8x8_size(uint control_word)
{
    int q_bits = int((control_word >> Q_PLANES_OFFSET) & 0xfu);
    uint lsbs = control_word & 0x5555u;
    uint msbs = control_word & 0xaaaau;
    uint msbs_shift = msbs >> 1;
    msbs |= msbs_shift;
    return countbits(lsbs) + countbits(msbs) + uint(q_bits) * 8u;
}

// ------------------------------------------------------------------
// Helper: quantize_code_word (bp:107-142)
// ------------------------------------------------------------------
uint quantize_code_word(uint control_word, int quant)
{
    if (quant != 0 && control_word != 0u) {
        int q_bits = int((control_word >> Q_PLANES_OFFSET) & 0xfu);
        int sub_quant = min(q_bits, quant);
        q_bits -= sub_quant;
        quant -= sub_quant;

        if (quant != 0) {
            quant = min(quant, 3);

            uint plane0 = control_word & 0x5555u;
            uint plane1 = (control_word & 0xaaaau) >> 1;
            uint plane2 = plane0 & plane1;

            while (quant != 0) {
                plane0 = plane1;
                plane1 = plane2;
                plane2 = 0u;
                quant--;
            }

            plane0 &= ~plane1;

            uint new_control_word = plane0 | (plane1 << 1);
            control_word = (control_word & ~0xffffu) | (new_control_word & 0xffffu);
        }

        control_word = (control_word & ~0xf0000u) | (uint(q_bits) << Q_PLANES_OFFSET);
    }

    return control_word;
}

// ------------------------------------------------------------------
// Helper: modify_quant_code (bp:161-167)
// ------------------------------------------------------------------
uint modify_quant_code(uint code, int quant)
{
    int e = int((code >> 3) & 0x1fu);
    e = max(e - quant, 0);
    return (code & ~0x3f8u) | (uint(e) << 3);
}

// ------------------------------------------------------------------
// Helper: load byte from payload buffer (bp:340)
// ------------------------------------------------------------------
uint load_payload_byte(RWByteAddressBuffer payload, uint idx)
{
    uint word = payload.Load((idx + 8u) & ~3u);
    return (word >> (8u * ((idx + 8u) & 3u))) & 0xffu;
}

// ------------------------------------------------------------------
// Main
// ------------------------------------------------------------------
[numthreads(64, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    // ------------------------------------------------------------------
    // P0: Zero staging arrays and ballot
    // ------------------------------------------------------------------
    // Each thread zeros 40 words of its block's packet
    uint blk = gi >> 4;  // Which 32x32 block (0..3)
    for (uint w = gi; w < MAX_PACKET_WORDS; w += 64u) {
        gs_pkt[blk * MAX_PACKET_WORDS + w] = 0u;
    }
    gs_ballot[blk] = 0u;
    gs_base[blk] = 0u;
    if (gi == 0u) gs_overflow[0] = 0u;

    GroupMemoryBarrierWithGroupSync();  // B1

    // ------------------------------------------------------------------
    // P1: Loads, quantize, ballot, per-lane data
    // ------------------------------------------------------------------
    uint linear_block_32x32_index = gi >> 4;
    uint local_block_index = gi & 15u;  // 0..15 within 32x32 block
    uint local_x = local_block_index & 3u;
    uint local_y = local_block_index >> 2;

    // Compute 32x32 block index
    int block32x32_x = 2 * gid.y + int(local_block_index & 1u);
    int block32x32_y = 2 * gid.x + (local_block_index >> 2);

    // 8x8 block index
    int block8x8_x = 4 * block32x32_x + int(local_x);
    int block8x8_y = 4 * block32x32_y + int(local_y);

    bool in_range_8x8 = (block8x8_x < resolution_8x8_blocks_x) && (block8x8_y < resolution_8x8_blocks_y);
    bool in_range_32x32 = (block32x32_x < resolution_32x32_blocks_x) && (block32x32_y < resolution_32x32_blocks_y);

    // Load quant
    int quant = 0;
    if (in_range_32x32) {
        int bo32 = block_offset_32x32 + block_stride_32x32 * block32x32_y + block32x32_x;
        quant = int(t2.Load(bo32 * 4));
    }

    // Load block meta and stats
    uint code_word = 0u;
    uint meta_offset = 0u;
    uint num_bits_for_q = 0u;

    if (in_range_8x8) {
        int b8 = block_offset_8x8 + block_stride_8x8 * block8x8_y + block8x8_x;
        code_word = t0.Load(b8 * 8 + 0);
        meta_offset = t0.Load(b8 * 8 + 4);

        // Stats: num_planes at offset 0, errors[min(num_planes, quant)].payload_cost at offset 8
        uint stats_base = uint(b8) * 64u;  // BlockStats = 64 bytes
        uint num_planes = t1.Load(int(stats_base));
        uint q = (quant < int(num_planes)) ? uint(quant) : num_planes;
        num_bits_for_q = (t1.Load(int(stats_base + 8u + q * 8u)) >> 16u) & 0xffffu;
    }

    // Quantize code word
    code_word = quantize_code_word(code_word, quant);
    bool active = (code_word & 0xffffu) != 0u;

    // Ballot: InterlockedOr on groupshared
    uint bit_pos = local_y * 4u + local_x;
    if (active) {
        InterlockedOr(gs_ballot[blk], 1u << local_block_index);
    }

    // Per-lane data
    uint plane_bytes = compute_required_8x8_size(code_word);
    uint sign_bits = (active && num_bits_for_q > plane_bytes * 8u) ? (num_bits_for_q - plane_bytes * 8u) : 0u;
    uint bits_meta = (num_bits_for_q != 0u) ? (num_bits_for_q + 24u) : 0u;

    gs_plane_bytes[gi] = plane_bytes;
    gs_sign_bits[gi] = sign_bits;
    gs_bits_meta[gi] = bits_meta;

    GroupMemoryBarrierWithGroupSync();  // B2

    // ------------------------------------------------------------------
    // P2: Sums and scans from groupshared; lane 15 allocates base
    // ------------------------------------------------------------------
    // Sum bits_meta over the 16 lanes of this block
    uint block_bits_meta = 0u;
    for (uint j = 0u; j < 16u; j++) {
        block_bits_meta += gs_bits_meta[blk * 16u + j];
    }

    uint total_words = (block_bits_meta + 31u) / 32u;
    if (total_words != 0u) total_words += 2u;  // HeaderSize = 2

    // Lane 15 of each block allocates
    bool writes_header = in_range_32x32 && (local_block_index == 15u);

    uint base = 0u;
    if (writes_header && total_words != 0u) {
        u2.InterlockedAdd(4, total_words, base);
    }

    // Broadcast base from lane 15 to all lanes of this block
    gs_base[blk] = base;
    GroupMemoryBarrierWithGroupSync();  // B3

    base = gs_base[blk];

    // Exclusive scan of plane_bytes and sign_bits over the 16 lanes
    uint exclusive_plane = 0u;
    for (uint jj = 0u; jj < local_block_index; jj++) {
        exclusive_plane += gs_plane_bytes[blk * 16u + jj];
    }

    uint exclusive_sign = 0u;
    for (uint kk = 0u; kk < local_block_index; kk++) {
        exclusive_sign += gs_sign_bits[blk * 16u + kk];
    }

    // Total sign bits for this block
    uint total_sign_bits = 0u;
    for (uint ll = 0u; ll < 16u; ll++) {
        total_sign_bits += gs_sign_bits[blk * 16u + ll];
    }
    uint total_sign_bytes = (total_sign_bits + 7u) / 8u;

    // Total plane bytes for this block
    uint total_plane_bytes = 0u;
    for (uint mm = 0u; mm < 16u; mm++) {
        total_plane_bytes += gs_plane_bytes[blk * 16u + mm];
    }

    // Total active blocks (popcount of ballot)
    uint N = countbits(gs_ballot[blk]);

    // Offsets
    uint plane_off = 4u * base + 3u * N + 8u + exclusive_plane;
    uint sign_off = 4u * base + 3u * N + 8u + total_plane_bytes;

    // ------------------------------------------------------------------
    // P3: Header words, code word, q byte, planes and signs into gs_pkt
    // ------------------------------------------------------------------
    // Lane 15 writes header (words 0 and 1)
    if (writes_header && total_words != 0u) {
        // Word 0: ballot (16) | payload_words (12) << 16 | sequence (3) << 28 | extended(=0) << 31
        uint ballot_val = gs_ballot[blk] & 0xffffu;
        gs_pkt[blk * MAX_PACKET_WORDS + 0] = ballot_val | (total_words << 16) | (sequence_code << 28);

        // Word 1: modify_quant_code(quant_resolution_code, quant) (8) | block_index (24) << 8
        int block_index = block_offset_32x32 + block_stride_32x32 * block32x32_y + block32x32_x;
        gs_pkt[blk * MAX_PACKET_WORDS + 1] = modify_quant_code(quant_resolution_code, quant) | (uint(block_index) << 8);

        // Write meta to u1
        uint meta_offset_addr = 8u + uint(block_index) * 8u;
        u1.Store(int(meta_offset_addr), int(base));
        u1.Store(int(meta_offset_addr + 4), int(total_words));
    }

    // Write code word, q byte, planes, and signs into gs_pkt
    if (active) {
        // Header offset within packet (position of this 8x8 block's data)
        uint hdr_offset = countbits(gs_ballot[blk] & ((1u << local_block_index) - 1u));

        // Write code word (u16) at 4*base + 8 + 2*hdr_offset
        uint cw_addr = 4u * base + 8u + 2u * hdr_offset;
        uint cw_low = code_word & 0xffffu;
        uint word_idx = cw_addr >> 2u;
        uint byte_offset = cw_addr & 3u;
        uint existing = gs_pkt[blk * MAX_PACKET_WORDS + word_idx];
        existing = (existing & ~(0xffffu << (byte_offset * 8u))) | (cw_low << (byte_offset * 8u));
        gs_pkt[blk * MAX_PACKET_WORDS + word_idx] = existing;

        // Write q byte (u8) at 4*base + 8 + 2*N + hdr_offset
        uint q_byte_addr = 4u * base + 8u + 2u * N + hdr_offset;
        uint q_byte = (code_word >> 16u) & 0xffu;
        uint word_idx2 = q_byte_addr >> 2u;
        uint byte_offset2 = q_byte_addr & 3u;
        uint existing2 = gs_pkt[blk * MAX_PACKET_WORDS + word_idx2];
        existing2 = (existing2 & ~(0xffu << (byte_offset2 * 8u))) | (uint(q_byte) << (byte_offset2 * 8u));
        gs_pkt[blk * MAX_PACKET_WORDS + word_idx2] = existing2;

        // Copy planes and extract significant masks for sign bits
        // meta_cw is the ORIGINAL code word (before quantization) for in_planes
        uint meta_cw = t0.Load((block_offset_8x8 + block_stride_8x8 * block8x8_y + block8x8_x) * 8);
        uint in_q_bits = (meta_cw >> Q_PLANES_OFFSET) & 0xfu;
        uint out_q_bits = (code_word >> Q_PLANES_OFFSET) & 0xfu;

        uint in_offset = meta_offset;
        uint out_offset = plane_off;
        uint sign_off_bits = sign_off * 8u + exclusive_sign;  // bit offset within sign region

        for (int sb = 0; sb < 16; sb += 2) {
            uint out_planes = ((code_word >> sb) & 3u) + out_q_bits;
            uint in_planes = ((meta_cw >> sb) & 3u) + in_q_bits;
            if (in_planes != 0u) in_planes++;

            if (in_planes != 0u && in_offset + in_planes * 4u < 65536u) {
                // Copy plane bytes and track significant mask
                uint significant_mask = 0u;

                for (uint p = 0u; p < out_planes && p < in_planes; p++) {
                    uint byte_idx = in_offset + p;
                    uint src_word = load_payload_byte(u2, byte_idx);
                    uint byte_val = (src_word >> (8u * ((byte_idx + 8u) & 3u))) & 0xffu;

                    // Store in packet staging
                    uint dst_word_idx = out_offset >> 2u;
                    uint dst_byte_in = out_offset & 3u;
                    uint existing_pkt = gs_pkt[blk * MAX_PACKET_WORDS + dst_word_idx];
                    existing_pkt = (existing_pkt & ~(0xffu << (dst_byte_in * 8u))) | (byte_val << (dst_byte_in * 8u));
                    gs_pkt[blk * MAX_PACKET_WORDS + dst_word_idx] = existing_pkt;

                    // Track significance: if any bit set, this byte is significant
                    significant_mask |= byte_val;
                    out_offset++;
                }

                // Write sign bits for each set bit in significant_mask
                // Each set bit k means coefficient k is significant
                uint sig_mask = significant_mask;
                while (sig_mask != 0u) {
                    int bit_pos = firstbitlow(sig_mask);
                    sig_mask &= sig_mask - 1u;  // Clear lowest set bit

                    // Write sign bit at sign_off_bits + bit_pos
                    uint sign_word_idx = (sign_off_bits + uint(bit_pos)) >> 5u;  // 32 bits per word
                    uint sign_bit_in_word = (sign_off_bits + uint(bit_pos)) & 31u;

                    // Read current word from staging
                    uint pkt_word_idx = sign_word_idx >> 2u;
                    uint pkt_byte_in = sign_word_idx & 3u;
                    uint existing_sign = gs_pkt[blk * MAX_PACKET_WORDS + pkt_word_idx];

                    // Set the bit
                    existing_sign |= (1u << sign_bit_in_word);
                    gs_pkt[blk * MAX_PACKET_WORDS + pkt_word_idx] = existing_sign;
                }
            }

            in_offset += in_planes;
        }
    }

    GroupMemoryBarrierWithGroupSync();  // B4

    // ------------------------------------------------------------------
    // P4: Copy-out of words words per block to u0
    // ------------------------------------------------------------------
    // Each thread copies 16 words (stride 16 within the block)
    if (total_words != 0u && total_words <= uint(MAX_PACKET_WORDS)) {
        for (uint w = local_block_index; w < total_words; w += 16u) {
            u0.Store(int(4 * (base + w)), gs_pkt[blk * MAX_PACKET_WORDS + w]);
        }
    }
}
