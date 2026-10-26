// Time pwd3d11_encoder_encode_cpu + compute_num_packets + packetize.
// Frame and cap match prototype/pwbench.cpp and comparison.md.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pyrowave_d3d11.h"

static std::vector<uint8_t> make_plane(int w, int h, uint32_t seed)
{
    std::vector<uint8_t> p(size_t(w) * size_t(h));
    uint32_t s = seed;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            s = s * 1664525u + 1013904223u;
            float v = 128.f + 60.f * std::sin(x * 0.031f + seed) * std::cos(y * 0.017f) + 0.15f * float(x - w / 2) +
                      (((x / 37) + (y / 23)) & 1 ? 25.f : -25.f) + float(int(s >> 27)) - 16.f;
            float clamped = v < 0.f ? 0.f : (v > 255.f ? 255.f : v);
            p[size_t(y) * size_t(w) + x] = uint8_t(clamped);
        }
    }
    return p;
}

static double qpc_ms(LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER freq)
{
    return double(b.QuadPart - a.QuadPart) * 1000.0 / double(freq.QuadPart);
}

static int run_size(pwd3d11_device dev, int W, int H, int warmup, int iters)
{
    pwd3d11_encoder_create_info ei = {};
    ei.device = dev;
    ei.width = W;
    ei.height = H;
    ei.chroma = PWD3D11_CHROMA_SUBSAMPLING_420;
    pwd3d11_encoder enc = nullptr;
    if (pwd3d11_encoder_create(&ei, &enc) != PWD3D11_SUCCESS) {
        std::fprintf(stderr, "encoder_create %dx%d failed\n", W, H);
        return 1;
    }

    std::vector<uint8_t> y = make_plane(W, H, 1);
    std::vector<uint8_t> u = make_plane(W / 2, H / 2, 2);
    std::vector<uint8_t> v = make_plane(W / 2, H / 2, 3);
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

    size_t cap = size_t(W) * size_t(H) * 3u / 16u;
    if (cap < 32768u) cap = 32768u;
    pwd3d11_rate_control rc = {};
    rc.maximum_bitstream_size = cap;
    const size_t boundary = cap + 65536u;
    std::vector<uint8_t> bits(boundary);
    std::vector<pwd3d11_packet> packets(8192);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double sum = 0;
    size_t nbytes = 0, np = 0;
    for (int i = 0; i < warmup + iters; i++) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        if (pwd3d11_encoder_encode_cpu(enc, &buf, &rc) != PWD3D11_SUCCESS) {
            std::fprintf(stderr, "encode %dx%d failed\n", W, H);
            pwd3d11_encoder_destroy(enc);
            return 1;
        }
        size_t got_np = 0;
        if (pwd3d11_encoder_compute_num_packets(enc, boundary, &got_np) != PWD3D11_SUCCESS || got_np == 0) {
            std::fprintf(stderr, "num_packets %dx%d failed\n", W, H);
            pwd3d11_encoder_destroy(enc);
            return 1;
        }
        if (got_np > packets.size()) packets.resize(got_np);
        size_t got = 0;
        if (pwd3d11_encoder_packetize(enc, packets.data(), boundary, &got, bits.data(), bits.size()) != PWD3D11_SUCCESS) {
            std::fprintf(stderr, "packetize %dx%d failed\n", W, H);
            pwd3d11_encoder_destroy(enc);
            return 1;
        }
        QueryPerformanceCounter(&t1);
        if (i < warmup) continue;
        sum += qpc_ms(t0, t1, freq);
        nbytes = 0;
        for (size_t p = 0; p < got; p++) nbytes += packets[p].size;
        np = got;
    }
    pwd3d11_encoder_destroy(enc);
    std::printf("%dx%d avg_ms=%.3f bytes=%zu packets=%zu\n", W, H, sum / double(iters), nbytes, np);
    return 0;
}

static pwd3d11_device open_rtx3060()
{
    auto try_index = [](int32_t index, std::string *name) -> pwd3d11_device {
        pwd3d11_device_create_info di = {};
        di.struct_size = sizeof(di);
        di.adapter_index = index;
        di.flags = 0;
        pwd3d11_device dev = nullptr;
        if (pwd3d11_create_device(&di, &dev) != PWD3D11_SUCCESS) return nullptr;
        pwd3d11_adapter_info info = {};
        pwd3d11_device_get_adapter_info(dev, &info);
        *name = info.name;
        return dev;
    };
    std::string name;
    pwd3d11_device dev = try_index(-1, &name);
    if (dev && name.find("RTX 3060") != std::string::npos) {
        std::printf("adapter: %s\n", name.c_str());
        return dev;
    }
    if (dev) pwd3d11_device_destroy(dev);
    for (int i = 0; i < 8; i++) {
        dev = try_index(i, &name);
        if (!dev) continue;
        if (name.find("RTX 3060") != std::string::npos) {
            std::printf("adapter: %s\n", name.c_str());
            return dev;
        }
        std::fprintf(stderr, "skip adapter %d: %s\n", i, name.c_str());
        pwd3d11_device_destroy(dev);
    }
    std::fprintf(stderr, "RTX 3060 not found\n");
    return nullptr;
}

int main()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    pwd3d11_device dev = open_rtx3060();
    if (!dev) {
        CoUninitialize();
        return 1;
    }
    int rc = run_size(dev, 1280, 720, 5, 40);
    if (rc == 0) rc = run_size(dev, 1920, 1080, 5, 40);
    pwd3d11_device_destroy(dev);
    CoUninitialize();
    return rc;
}
