// pwbench: time PyroWave's own encoder (pinned a0d2e71) through its C API, loaded at runtime.
// Reports wall time per frame and PyroWave's internal per-stage GPU timestamps (DWT, Analyze, Resolve, Quant, Packing).
// Windows (lavapipe): pwbench.exe <libpyrowave-shared-1.dll> <vulkan-cpu\vulkan-1.dll or -> W H frames [max_bytes]
// Linux:              ./pwbench <libpyrowave-shared.so> - W H frames [max_bytes]
#include <vulkan/vulkan.h>
#include "pyrowave.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
static void *load(const char *p) { return LoadLibraryExA(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH); }
static void *sym(void *h, const char *n) { return (void *)GetProcAddress((HMODULE)h, n); }
#else
#include <dlfcn.h>
static void *load(const char *p) { void *h = dlopen(p, RTLD_NOW | RTLD_GLOBAL); if (!h) fprintf(stderr, "dlopen: %s\n", dlerror()); return h; }
static void *sym(void *h, const char *n) { return dlsym(h, n); }
#endif

#define FN(name) static decltype(&name) p_##name;
FN(pyrowave_create_default_device) FN(pyrowave_encoder_create) FN(pyrowave_encoder_encode_cpu)
FN(pyrowave_encoder_compute_num_packets) FN(pyrowave_encoder_packetize) FN(pyrowave_device_report_performance_stats)
FN(pyrowave_encoder_destroy) FN(pyrowave_device_destroy)

static std::vector<uint8_t> make_plane(int w, int h, uint32_t seed)
{
    std::vector<uint8_t> p(size_t(w) * h);
    uint32_t s = seed;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            s = s * 1664525u + 1013904223u;
            float v = 128.f + 60.f * std::sin(x * 0.031f + seed) * std::cos(y * 0.017f) + 0.15f * float(x - w / 2) +
                      (((x / 37) + (y / 23)) & 1 ? 25.f : -25.f) + float(int(s >> 27)) - 16.f;
            p[size_t(y) * w + x] = uint8_t(std::min(255.f, std::max(0.f, v)));
        }
    return p;
}

static double pct(std::vector<double> v, double p)
{
    std::sort(v.begin(), v.end());
    size_t i = size_t(std::ceil(p * v.size())); if (i > 0) i--; if (i >= v.size()) i = v.size() - 1;
    return v[i];
}

int main(int argc, char **argv)
{
    if (argc < 6) { fprintf(stderr, "usage: pwbench <pyrowave lib> <vulkan loader to preload or -> W H frames [max_bytes]\n"); return 1; }
    if (strcmp(argv[2], "-") != 0 && !load(argv[2])) { fprintf(stderr, "failed to preload %s\n", argv[2]); return 1; }
    void *lib = load(argv[1]);
    if (!lib) { fprintf(stderr, "failed to load %s\n", argv[1]); return 1; }
#define LOADFN(name) p_##name = (decltype(p_##name))sym(lib, #name); if (!p_##name) { fprintf(stderr, "missing %s\n", #name); return 1; }
    LOADFN(pyrowave_create_default_device) LOADFN(pyrowave_encoder_create) LOADFN(pyrowave_encoder_encode_cpu)
    LOADFN(pyrowave_encoder_compute_num_packets) LOADFN(pyrowave_encoder_packetize) LOADFN(pyrowave_device_report_performance_stats)
    LOADFN(pyrowave_encoder_destroy) LOADFN(pyrowave_device_destroy)

    int W = atoi(argv[3]), H = atoi(argv[4]), frames = atoi(argv[5]);
    size_t max_bytes = argc > 6 ? strtoull(argv[6], nullptr, 0) : 250000;

    pyrowave_device dev = nullptr;
    pyrowave_result r = p_pyrowave_create_default_device(&dev);
    if (r != PYROWAVE_SUCCESS) { fprintf(stderr, "create_default_device failed %d\n", int(r)); return 1; }
    pyrowave_encoder_create_info ci = { dev, W, H, PYROWAVE_CHROMA_SUBSAMPLING_420 };
    pyrowave_encoder enc = nullptr;
    if ((r = p_pyrowave_encoder_create(&ci, &enc)) != PYROWAVE_SUCCESS) { fprintf(stderr, "encoder_create failed %d\n", int(r)); return 1; }

    std::vector<uint8_t> planes[3] = { make_plane(W, H, 1), make_plane(W / 2, H / 2, 2), make_plane(W / 2, H / 2, 3) };
    pyrowave_cpu_buffer buf = {};
    for (int i = 0; i < 3; i++)
    {
        buf.data[i] = planes[i].data();
        buf.row_stride_in_bytes[i] = i ? W / 2 : W;
        buf.plane_size_in_bytes[i] = planes[i].size();
    }
    buf.width = W; buf.height = H; buf.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    pyrowave_rate_control rc = { max_bytes };
    std::vector<uint8_t> out(max_bytes + 65536);
    std::vector<pyrowave_packet> pk(4096);

    auto stats = [](void *, const char *msg) { printf("  %s\n", msg); };
    std::vector<double> wall;
    size_t last_size = 0;
    int warm = std::min(5, frames);
    for (int f = 0; f < frames + warm; f++)
    {
        if (f == warm) p_pyrowave_device_report_performance_stats(dev, [](void *, const char *) {}, nullptr, true);
        auto t0 = std::chrono::high_resolution_clock::now();
        if ((r = p_pyrowave_encoder_encode_cpu(enc, &buf, &rc)) != PYROWAVE_SUCCESS) { fprintf(stderr, "encode failed %d\n", int(r)); return 1; }
        size_t n = 0, np = 0;
        p_pyrowave_encoder_compute_num_packets(enc, max_bytes + 65536, &n);
        p_pyrowave_encoder_packetize(enc, pk.data(), max_bytes + 65536, &np, out.data(), out.size());
        auto t1 = std::chrono::high_resolution_clock::now();
        if (f >= warm) wall.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        last_size = np ? pk[np - 1].offset + pk[np - 1].size : 0;
    }
    printf("PyroWave encode %dx%d 4:2:0, %d frames, cap %zu B, last frame %zu B\n", W, H, frames, max_bytes, last_size);
    printf("  wall per frame (encode_cpu incl. upload + packetize): median %.3f ms, p90 %.3f ms, min %.3f ms\n",
           pct(wall, 0.5), pct(wall, 0.9), pct(wall, 0.0));
    printf("  PyroWave GPU timestamp report:\n");
    p_pyrowave_device_report_performance_stats(dev, stats, nullptr, true);
    p_pyrowave_encoder_destroy(enc);
    p_pyrowave_device_destroy(dev);
    return 0;
}
