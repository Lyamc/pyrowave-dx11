// test_t5b.cpp -- T5b acceptance test: full encode pipeline
// Tests: ENCODE_FRAME (basic encode of a 64x64 YUV frame)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
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

    printf("\nResults: %d/%d tests passed\n", passed, total);
    CoUninitialize();
    return (passed == total) ? 0 : 1;
}
