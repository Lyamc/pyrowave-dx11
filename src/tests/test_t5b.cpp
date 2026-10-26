// test_t5b.cpp -- T5b acceptance test: full encode pipeline
// Tests: ENCODE_FRAME (basic encode of a 64x64 YUV frame)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>

#include "pyrowave_d3d11.h"

using Microsoft::WRL::ComPtr;

static bool test_encode_frame()
{
    printf("    [1] creating device...\n");
    // Create device
    pwd3d11_device_create_info di = {};
    di.struct_size = sizeof(di);
    di.adapter_index = -1;
    di.flags = 0;
    pwd3d11_device dev = nullptr;
    pwd3d11_result r = pwd3d11_create_device(&di, &dev);
    if (r != PWD3D11_SUCCESS || !dev) {
        printf("  ENCODE_FRAME: device creation failed: %d\n", (int)r);
        return false;
    }
    printf("    [2] device created\n");

    // Create encoder (64x64 YUV 4:2:0)
    pwd3d11_encoder_create_info ei = {};
    ei.device = dev;
    ei.width = 64;
    ei.height = 64;
    ei.chroma = PWD3D11_CHROMA_SUBSAMPLING_420;
    pwd3d11_encoder enc = nullptr;
    r = pwd3d11_encoder_create(&ei, &enc);
    if (r != PWD3D11_SUCCESS || !enc) {
        printf("  ENCODE_FRAME: encoder creation failed: %d\n", (int)r);
        pwd3d11_device_destroy(dev);
        return false;
    }
    printf("    [3] encoder created\n");

    // Create input frame: solid gray Y + flat Cb/Cr
    const int W = 64, H = 64;
    std::vector<uint8_t> y_plane(W * H);
    std::vector<uint8_t> cb_plane(W / 2 * H / 2);
    std::vector<uint8_t> cr_plane(W / 2 * H / 2);
    for (int i = 0; i < (int)y_plane.size(); i++) y_plane[i] = 128;
    for (int i = 0; i < (int)cb_plane.size(); i++) cb_plane[i] = 128;
    for (int i = 0; i < (int)cr_plane.size(); i++) cr_plane[i] = 128;

    pwd3d11_cpu_buffer buf = {};
    buf.width = W;
    buf.height = H;
    buf.format = PWD3D11_CPU_BUFFER_FORMAT_YUV420P;
    buf.data[0] = y_plane.data();
    buf.data[1] = cb_plane.data();
    buf.data[2] = cr_plane.data();
    buf.row_stride_in_bytes[0] = W;
    buf.row_stride_in_bytes[1] = W / 2;
    buf.row_stride_in_bytes[2] = W / 2;
    buf.plane_size_in_bytes[0] = y_plane.size();
    buf.plane_size_in_bytes[1] = cb_plane.size();
    buf.plane_size_in_bytes[2] = cr_plane.size();

    pwd3d11_rate_control rc = {};
    rc.maximum_bitstream_size = 65536;  // 64 KB

    printf("  ENCODE_FRAME: encoding 64x64 gray frame...\n");
    r = pwd3d11_encoder_encode_cpu(enc, &buf, &rc);
    if (r != PWD3D11_SUCCESS) {
        printf("  ENCODE_FRAME: encode_cpu failed: %d\n", (int)r);
        pwd3d11_encoder_destroy(enc);
        pwd3d11_device_destroy(dev);
        return false;
    }

    // Compute num packets
    size_t num_packets = 0;
    r = pwd3d11_encoder_compute_num_packets(enc, 4096, &num_packets);
    if (r != PWD3D11_SUCCESS) {
        printf("  ENCODE_FRAME: compute_num_packets failed: %d\n", (int)r);
    } else {
        printf("  ENCODE_FRAME: computed %zu packets\n", num_packets);
    }

    pwd3d11_encoder_destroy(enc);
    pwd3d11_device_destroy(dev);
    return true;
}

// Word 0 of a packed block. Same bitfields the packer writes.
struct PwdBlockHeader {
    uint16_t ballot;
    uint16_t payload_words : 12;
    uint16_t sequence : 3;
    uint16_t extended : 1;
    uint32_t quant_code : 8;
    uint32_t block_index : 24;
};
static_assert(sizeof(PwdBlockHeader) == 8, "block header");

struct PwdSeqHeader {
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
};
static_assert(sizeof(PwdSeqHeader) == 8, "sequence header");

struct SeenBlock {
    uint32_t index;
    uint32_t words;
    uint32_t sequence;
    std::vector<uint8_t> bytes;
};

static bool encode_packet(pwd3d11_encoder enc, const pwd3d11_cpu_buffer *buf,
                          std::vector<uint8_t> *out, std::vector<SeenBlock> *blocks)
{
    pwd3d11_rate_control rc = {};
    rc.maximum_bitstream_size = 65536;
    if (pwd3d11_encoder_encode_cpu(enc, buf, &rc) != PWD3D11_SUCCESS) return false;
    const size_t boundary = 65536 + 65536;
    size_t np = 0;
    if (pwd3d11_encoder_compute_num_packets(enc, boundary, &np) != PWD3D11_SUCCESS || np == 0)
        return false;
    std::vector<pwd3d11_packet> packets(np);
    std::vector<uint8_t> bits(boundary);
    size_t got = 0;
    if (pwd3d11_encoder_packetize(enc, packets.data(), boundary, &got, bits.data(), bits.size()) != PWD3D11_SUCCESS)
        return false;
    size_t nbytes = 0;
    for (size_t i = 0; i < got; i++) nbytes += packets[i].size;
    if (nbytes < sizeof(PwdSeqHeader) || nbytes > bits.size()) return false;
    out->assign(bits.begin(), bits.begin() + nbytes);
    blocks->clear();
    PwdSeqHeader seq = {};
    std::memcpy(&seq, out->data(), sizeof(seq));
    size_t off = sizeof(seq);
    while (off + sizeof(PwdBlockHeader) <= out->size()) {
        PwdBlockHeader hdr = {};
        std::memcpy(&hdr, out->data() + off, sizeof(hdr));
        size_t n = size_t(hdr.payload_words) * 4u;
        if (hdr.payload_words < 2 || off + n > out->size()) return false;
        if (hdr.sequence != seq.sequence) {
            std::printf("  EMPTY_BLOCK: block %u sequence %u != packet %u\n",
                        hdr.block_index, hdr.sequence, seq.sequence);
            return false;
        }
        SeenBlock b;
        b.index = hdr.block_index;
        b.words = hdr.payload_words;
        b.sequence = hdr.sequence;
        b.bytes.assign(out->data() + off, out->data() + off + n);
        blocks->push_back(b);
        off += n;
    }
    return off == out->size();
}

// Frame 1 occupies detail blocks. Frame 2 is flat, so those blocks are empty.
// Packetize must not copy the previous num_words and payload.
static bool test_empty_block_cleared()
{
    pwd3d11_device_create_info di = {};
    di.struct_size = sizeof(di);
    di.adapter_index = -1;
    di.flags = 0;
    pwd3d11_device dev = nullptr;
    if (pwd3d11_create_device(&di, &dev) != PWD3D11_SUCCESS || !dev) {
        std::printf("  EMPTY_BLOCK: device creation failed\n");
        return false;
    }
    pwd3d11_encoder_create_info ei = {};
    ei.device = dev;
    ei.width = 64;
    ei.height = 64;
    ei.chroma = PWD3D11_CHROMA_SUBSAMPLING_420;
    pwd3d11_encoder enc = nullptr;
    if (pwd3d11_encoder_create(&ei, &enc) != PWD3D11_SUCCESS || !enc) {
        std::printf("  EMPTY_BLOCK: encoder creation failed\n");
        pwd3d11_device_destroy(dev);
        return false;
    }

    const int W = 64, H = 64;
    std::vector<uint8_t> y(size_t(W) * H), u(size_t(W / 2) * (H / 2), 128), v = u;
    for (int row = 0; row < H; row++) {
        for (int col = 0; col < W; col++)
            y[size_t(row) * W + col] = (col < 32) ? 0 : 255;
    }
    pwd3d11_cpu_buffer buf = {};
    buf.data[0] = y.data();
    buf.data[1] = u.data();
    buf.data[2] = v.data();
    buf.row_stride_in_bytes[0] = size_t(W);
    buf.row_stride_in_bytes[1] = size_t(W / 2);
    buf.row_stride_in_bytes[2] = size_t(W / 2);
    buf.plane_size_in_bytes[0] = y.size();
    buf.plane_size_in_bytes[1] = u.size();
    buf.plane_size_in_bytes[2] = v.size();
    buf.width = W;
    buf.height = H;
    buf.format = PWD3D11_CPU_BUFFER_FORMAT_YUV420P;

    std::vector<uint8_t> bits1;
    std::vector<SeenBlock> blocks1;
    if (!encode_packet(enc, &buf, &bits1, &blocks1)) {
        std::printf("  EMPTY_BLOCK: contrast frame failed\n");
        pwd3d11_encoder_destroy(enc);
        pwd3d11_device_destroy(dev);
        return false;
    }

    std::fill(y.begin(), y.end(), uint8_t(128));
    std::vector<uint8_t> bits2;
    std::vector<SeenBlock> blocks2;
    if (!encode_packet(enc, &buf, &bits2, &blocks2)) {
        std::printf("  EMPTY_BLOCK: flat frame failed\n");
        pwd3d11_encoder_destroy(enc);
        pwd3d11_device_destroy(dev);
        return false;
    }

    int occupied = 0;
    int dropped = 0;
    int stale = 0;
    for (const auto &a : blocks1) {
        if (a.words <= 2) continue;
        occupied++;
        const SeenBlock *same = nullptr;
        for (const auto &b : blocks2) {
            if (b.index == a.index) { same = &b; break; }
        }
        if (!same || same->words != a.words) dropped++;
        if (same && same->bytes == a.bytes) stale++;
    }
    std::printf("  EMPTY_BLOCK: contrast_blocks=%d flat_blocks=%zu occupied=%d dropped=%d stale=%d\n",
                int(blocks1.size()), blocks2.size(), occupied, dropped, stale);
    pwd3d11_encoder_destroy(enc);
    pwd3d11_device_destroy(dev);
    if (occupied < 1 || dropped < 1 || stale != 0) {
        std::printf("  EMPTY_BLOCK: flat frame reused a previous block\n");
        return false;
    }
    return true;
}

int main()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int passed = 0;
    int total = 0;

    {
        ++total;
        printf("TEST: ENCODE_FRAME ... ");
        if (test_encode_frame()) {
            printf("PASS\n");
            ++passed;
        } else {
            printf("FAIL\n");
        }
    }
    {
        ++total;
        printf("TEST: EMPTY_BLOCK ... ");
        if (test_empty_block_cleared()) {
            printf("PASS\n");
            ++passed;
        } else {
            printf("FAIL\n");
        }
    }

    printf("\nResults: %d/%d tests passed\n", passed, total);
    CoUninitialize();
    return (passed == total) ? 0 : 1;
}
