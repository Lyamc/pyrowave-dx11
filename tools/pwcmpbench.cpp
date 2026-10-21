// pwcmpbench: wall-time, process CPU, and working-set comparison for one PyroWave encode path.
//
// dx11:  pwcmpbench dx11 <pyrowave_d3d11.dll> W H <adapter substring or ->
// vk:    pwcmpbench vk <libpyrowave-shared-1.dll> <vulkan-1.dll or -> W H <vendor hex or 0>
//
// Latency is QueryPerformanceCounter around encode_cpu + compute_num_packets + packetize.
// CPU and memory are sampled every 50 ms on the encoder process during the measured run.
// CPU percent is process time (kernel+user) divided by wall time, as a share of the machine
// (100 means every logical processor is busy in this process). ONECORE is the same delta
// before dividing by the processor count (100 means one logical processor is full).
//
// Measured region: 20 warmup frames, then at least 200 frames and 5 seconds, capped at
// 8000 frames or 60 seconds. The frame is the pwbench synthetic I420 pattern. The rate
// cap is max(W*H*3/16, 32768), the etherquic default_frame_bytes budget. Packetize uses
// one packet boundary equal to the output buffer, matching prototype/pwbench.cpp.
//
// Build (x64 Native Tools):
//   cl /nologo /O2 /EHsc /std:c++17 /MT /W4 pwcmpbench.cpp /link psapi.lib

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static void *load_lib(const char *path)
{
    return LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

static void *sym(void *mod, const char *name)
{
    return (void *)GetProcAddress((HMODULE)mod, name);
}

struct Stats {
    double min_v = 0, max_v = 0, sum = 0, sumsq = 0;
    int n = 0;
    void add(double x)
    {
        if (n == 0 || x < min_v) min_v = x;
        if (n == 0 || x > max_v) max_v = x;
        sum += x;
        sumsq += x * x;
        n++;
    }
    double avg() const { return n ? sum / n : 0; }
    double stdev() const
    {
        if (n < 2) return 0;
        double v = (sumsq - (sum * sum) / n) / (n - 1);
        if (v < 0) v = 0;
        return std::sqrt(v);
    }
};

static void print_stats(const char *key, const Stats &s, const char *unit)
{
    std::printf("%s min=%.4f avg=%.4f max=%.4f std=%.4f n=%d unit=%s\n",
                key, s.min_v, s.avg(), s.max_v, s.stdev(), s.n, unit);
}

static double qpc_ms()
{
    static LARGE_INTEGER freq = []() {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * 1000.0 / double(freq.QuadPart);
}

static uint64_t filetime_100ns(FILETIME f)
{
    ULARGE_INTEGER u;
    u.LowPart = f.dwLowDateTime;
    u.HighPart = f.dwHighDateTime;
    return u.QuadPart;
}

static uint64_t process_cpu_100ns()
{
    FILETIME create_t, exit_t, kernel_t, user_t;
    if (!GetProcessTimes(GetCurrentProcess(), &create_t, &exit_t, &kernel_t, &user_t)) return 0;
    return filetime_100ns(kernel_t) + filetime_100ns(user_t);
}

struct MemSample {
    double cpu_machine_pct;
    double cpu_onecore_pct;
    double ws_mib;
    double priv_mib;
};

static int logical_processors()
{
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? int(n) : 1;
}

static int env_int(const char *name, int fallback)
{
    char buf[64];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return fallback;
    return std::atoi(buf);
}

static void read_memory(double *ws_mib, double *priv_mib)
{
    PROCESS_MEMORY_COUNTERS_EX pmc;
    std::memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc))) {
        *ws_mib = 0;
        *priv_mib = 0;
        return;
    }
    *ws_mib = double(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    *priv_mib = double(pmc.PrivateUsage) / (1024.0 * 1024.0);
}

static std::vector<uint8_t> make_plane(int w, int h, uint32_t seed)
{
    std::vector<uint8_t> p(size_t(w) * size_t(h));
    uint32_t s = seed;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            s = s * 1664525u + 1013904223u;
            float v = 128.f + 60.f * std::sin(x * 0.031f + float(seed)) * std::cos(y * 0.017f) +
                      0.15f * float(x - w / 2) + (((x / 37) + (y / 23)) & 1 ? 25.f : -25.f) +
                      float(int(s >> 27)) - 16.f;
            if (v < 0.f) v = 0.f;
            if (v > 255.f) v = 255.f;
            p[size_t(y) * size_t(w) + size_t(x)] = uint8_t(v);
        }
    }
    return p;
}

static std::string narrow(const wchar_t *ws)
{
    if (!ws) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), n, nullptr, nullptr);
    return out;
}

static bool contains_ci(const std::string &hay, const std::string &needle)
{
    if (needle.empty() || needle == "-") return true;
    std::string h = hay, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return h.find(n) != std::string::npos;
}

// --- D3D11 ABI (src/include/pyrowave_d3d11.h), loaded at runtime ---

enum { PWD3D11_SUCCESS = 0, PWD3D11_CHROMA_420 = 0, PWD3D11_FMT_YUV420P = 1 };

struct DxDeviceInfo {
    uint32_t struct_size;
    int32_t adapter_index;
    void *d3d11_device;
    uint32_t flags;
};

struct DxAdapterInfo {
    char name[128];
    uint32_t vendor_id;
    uint32_t device_id;
    uint64_t dedicated_video_memory;
};

struct DxEncoderInfo {
    void *device;
    int width;
    int height;
    int chroma;
};

struct DxBuffer {
    void *data[3];
    size_t row_stride[3];
    size_t plane_size[3];
    int width;
    int height;
    int format;
};

struct DxRate {
    size_t maximum_bitstream_size;
};

struct DxPacket {
    size_t offset;
    size_t size;
};

using PFN_dx_create = int(__cdecl *)(const DxDeviceInfo *, void **);
using PFN_dx_adapter = int(__cdecl *)(void *, DxAdapterInfo *);
using PFN_dx_destroy_dev = void(__cdecl *)(void *);
using PFN_dx_enc_create = int(__cdecl *)(const DxEncoderInfo *, void **);
using PFN_dx_encode = int(__cdecl *)(void *, const DxBuffer *, const DxRate *);
using PFN_dx_num = int(__cdecl *)(void *, size_t, size_t *);
using PFN_dx_packet = int(__cdecl *)(void *, DxPacket *, size_t, size_t *, void *, size_t);
using PFN_dx_enc_destroy = void(__cdecl *)(void *);

// --- Vulkan PyroWave ABI, loaded at runtime ---

struct VkEncInfo {
    void *device;
    int width;
    int height;
    int chroma;
};

struct VkBuffer {
    void *data[3];
    size_t row_stride[3];
    size_t plane_size[3];
    int width;
    int height;
    int format;
};

struct VkRate {
    size_t maximum_bitstream_size;
};

struct VkPacket {
    size_t offset;
    size_t size;
};

using PFN_vk_default = int(__cdecl *)(void **);
using PFN_vk_compat = int(__cdecl *)(uint32_t, uint32_t, const void *, const void *, const void *, int, void **);
using PFN_vk_enc_create = int(__cdecl *)(const VkEncInfo *, void **);
using PFN_vk_encode = int(__cdecl *)(void *, const VkBuffer *, const VkRate *);
using PFN_vk_num = int(__cdecl *)(void *, size_t, size_t *);
using PFN_vk_packet = int(__cdecl *)(void *, VkPacket *, size_t, size_t *, void *, size_t);
using PFN_vk_enc_destroy = void(__cdecl *)(void *);
using PFN_vk_dev_destroy = void(__cdecl *)(void *);

static int g_procs = 1;
static std::atomic<int> g_sample{0};
static std::mutex g_sample_mu;
static std::vector<MemSample> g_samples;

static void sampler_main()
{
    uint64_t prev_cpu = process_cpu_100ns();
    double prev_t = qpc_ms();
    while (g_sample.load(std::memory_order_relaxed)) {
        Sleep(50);
        if (!g_sample.load(std::memory_order_relaxed)) break;
        uint64_t now_cpu = process_cpu_100ns();
        double now_t = qpc_ms();
        double dt_s = (now_t - prev_t) / 1000.0;
        double cpu_s = double(now_cpu - prev_cpu) / 1.0e7;
        double onecore = dt_s > 0 ? (cpu_s / dt_s) * 100.0 : 0;
        double ws = 0, priv = 0;
        read_memory(&ws, &priv);
        MemSample s;
        s.cpu_onecore_pct = onecore;
        s.cpu_machine_pct = onecore / double(g_procs);
        s.ws_mib = ws;
        s.priv_mib = priv;
        {
            std::lock_guard<std::mutex> lock(g_sample_mu);
            g_samples.push_back(s);
        }
        prev_cpu = now_cpu;
        prev_t = now_t;
    }
}

struct RunResult {
    Stats lat;
    Stats cpu;
    Stats onecore;
    Stats ws;
    Stats priv;
    int frames = 0;
    double seconds = 0;
    size_t last_bytes = 0;
    int encode_rc = 0;
};

template <typename EncodeFn>
static RunResult measure(EncodeFn encode, int min_frames)
{
    const int warmup = env_int("PWCMP_WARMUP", 20);
    const double min_sec = double(env_int("PWCMP_MIN_SEC", 5));
    const double max_sec = double(env_int("PWCMP_MAX_SEC", 60));
    const int max_frames = env_int("PWCMP_MAX_FRAMES", 8000);
    min_frames = env_int("PWCMP_MIN_FRAMES", min_frames);
    RunResult rr;
    for (int i = 0; i < warmup; i++) {
        int rc = encode(&rr.last_bytes);
        if (rc != 0) {
            rr.encode_rc = rc;
            std::fprintf(stderr, "warmup encode failed on frame %d rc=%d\n", i, rc);
            return rr;
        }
    }
    g_samples.clear();
    g_sample.store(1, std::memory_order_relaxed);
    std::thread sampler(sampler_main);
    double t_begin = qpc_ms();
    for (int i = 0; i < max_frames; i++) {
        double t0 = qpc_ms();
        int rc = encode(&rr.last_bytes);
        double t1 = qpc_ms();
        if (rc != 0) {
            rr.encode_rc = rc;
            std::fprintf(stderr, "encode failed on measured frame %d rc=%d\n", i, rc);
            break;
        }
        rr.lat.add(t1 - t0);
        rr.frames++;
        rr.seconds = (t1 - t_begin) / 1000.0;
        if (rr.frames >= min_frames && rr.seconds >= min_sec) break;
        if (rr.seconds >= max_sec) break;
    }
    g_sample.store(0, std::memory_order_relaxed);
    sampler.join();
    std::lock_guard<std::mutex> lock(g_sample_mu);
    for (const MemSample &s : g_samples) {
        rr.cpu.add(s.cpu_machine_pct);
        rr.onecore.add(s.cpu_onecore_pct);
        rr.ws.add(s.ws_mib);
        rr.priv.add(s.priv_mib);
    }
    return rr;
}

static void emit(const char *backend, int W, int H, size_t budget, const std::string &device, const RunResult &rr)
{
    std::printf("RESULT backend=%s W=%d H=%d budget=%zu device=\"%s\" frames=%d seconds=%.3f last_bytes=%zu encode_rc=%d procs=%d\n",
                backend, W, H, budget, device.c_str(), rr.frames, rr.seconds, rr.last_bytes, rr.encode_rc, g_procs);
    print_stats("LAT_MS", rr.lat, "ms");
    print_stats("CPU_MACHINE_PCT", rr.cpu, "pct_machine");
    print_stats("CPU_ONECORE_PCT", rr.onecore, "pct_onecore");
    print_stats("WS_MIB", rr.ws, "MiB");
    print_stats("PRIV_MIB", rr.priv, "MiB");
}

static size_t default_budget(int W, int H)
{
    size_t b = size_t(W) * size_t(H) * 3 / 16;
    if (b < 32768) b = 32768;
    return b;
}

static int run_dx11(int argc, char **argv)
{
    if (argc < 6) {
        std::fprintf(stderr, "usage: pwcmpbench dx11 <dll> W H <adapter-substr-or->\n");
        return 2;
    }
    const char *dll_path = argv[2];
    int W = std::atoi(argv[3]);
    int H = std::atoi(argv[4]);
    const char *filter = argv[5];
    if (W < 16 || H < 16) {
        std::fprintf(stderr, "bad size\n");
        return 2;
    }
    HMODULE mod = (HMODULE)load_lib(dll_path);
    if (!mod) {
        std::fprintf(stderr, "LoadLibrary %s failed %lu\n", dll_path, GetLastError());
        return 1;
    }
    auto create = (PFN_dx_create)sym(mod, "pwd3d11_create_device");
    auto adapter = (PFN_dx_adapter)sym(mod, "pwd3d11_device_get_adapter_info");
    auto destroy_dev = (PFN_dx_destroy_dev)sym(mod, "pwd3d11_device_destroy");
    auto enc_create = (PFN_dx_enc_create)sym(mod, "pwd3d11_encoder_create");
    auto encode = (PFN_dx_encode)sym(mod, "pwd3d11_encoder_encode_cpu");
    auto numpk = (PFN_dx_num)sym(mod, "pwd3d11_encoder_compute_num_packets");
    auto packetize = (PFN_dx_packet)sym(mod, "pwd3d11_encoder_packetize");
    auto enc_destroy = (PFN_dx_enc_destroy)sym(mod, "pwd3d11_encoder_destroy");
    if (!create || !adapter || !destroy_dev || !enc_create || !encode || !numpk || !packetize || !enc_destroy) {
        std::fprintf(stderr, "dx11 dll is missing an export\n");
        return 1;
    }

    std::string chosen = "first";
    int32_t index = -1;
    if (filter[0] != '-' || filter[1] != '\0') {
        IDXGIFactory1 *factory = nullptr;
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory));
        if (FAILED(hr) || !factory) {
            std::fprintf(stderr, "CreateDXGIFactory1 failed %08lx\n", (unsigned long)hr);
            return 1;
        }
        int found = 0;
        bool matched = false;
        for (UINT i = 0;; i++) {
            IDXGIAdapter *ad = nullptr;
            hr = factory->EnumAdapters(i, &ad);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr) || !ad) break;
            DXGI_ADAPTER_DESC desc = {};
            ad->GetDesc(&desc);
            std::string name = narrow(desc.Description);
            bool warp = desc.VendorId == 0x1414 && desc.DeviceId == 0x8c;
            std::printf("DXGI[%u] %s vendor=%04x device=%04x vram=%zu%s\n", i, name.c_str(), desc.VendorId,
                        desc.DeviceId, size_t(desc.DedicatedVideoMemory), warp ? " WARP" : "");
            if (!warp) {
                if (!matched && contains_ci(name, filter)) {
                    index = found;
                    chosen = name;
                    matched = true;
                }
                found++;
            }
            ad->Release();
        }
        factory->Release();
        if (!matched) {
            std::fprintf(stderr, "no adapter matched '%s'\n", filter);
            return 1;
        }
    }

    DxDeviceInfo di = {};
    di.struct_size = sizeof(di);
    di.adapter_index = index;
    void *dev = nullptr;
    int rc = create(&di, &dev);
    if (rc != PWD3D11_SUCCESS || !dev) {
        std::fprintf(stderr, "pwd3d11_create_device failed %d\n", rc);
        return 1;
    }
    DxAdapterInfo info = {};
    if (adapter(dev, &info) == PWD3D11_SUCCESS) {
        chosen = info.name;
        std::printf("DX11_DEVICE %s vendor=%04x device=%04x vram=%llu\n", info.name, info.vendor_id, info.device_id,
                    (unsigned long long)info.dedicated_video_memory);
    }
    DxEncoderInfo ei = {};
    ei.device = dev;
    ei.width = W;
    ei.height = H;
    ei.chroma = PWD3D11_CHROMA_420;
    void *enc = nullptr;
    rc = enc_create(&ei, &enc);
    if (rc != PWD3D11_SUCCESS || !enc) {
        std::fprintf(stderr, "pwd3d11_encoder_create failed %d\n", rc);
        destroy_dev(dev);
        return 1;
    }

    auto y = make_plane(W, H, 1);
    auto cb = make_plane(W / 2, H / 2, 2);
    auto cr = make_plane(W / 2, H / 2, 3);
    DxBuffer buf = {};
    buf.data[0] = y.data();
    buf.data[1] = cb.data();
    buf.data[2] = cr.data();
    buf.row_stride[0] = size_t(W);
    buf.row_stride[1] = size_t(W / 2);
    buf.row_stride[2] = size_t(W / 2);
    buf.plane_size[0] = y.size();
    buf.plane_size[1] = cb.size();
    buf.plane_size[2] = cr.size();
    buf.width = W;
    buf.height = H;
    buf.format = PWD3D11_FMT_YUV420P;
    size_t budget = default_budget(W, H);
    DxRate rate = {budget};
    std::vector<uint8_t> out(budget + (1u << 20));
    std::vector<DxPacket> pk(65536);

    auto step = [&](size_t *last) -> int {
        int r = encode(enc, &buf, &rate);
        if (r != 0) return r;
        size_t n = 0, np = 0;
        r = numpk(enc, out.size(), &n);
        if (r != 0) return r;
        if (n > pk.size()) return -100;
        r = packetize(enc, pk.data(), out.size(), &np, out.data(), out.size());
        if (r != 0) return r;
        size_t bytes = 0;
        if (np && np <= pk.size()) bytes = pk[np - 1].offset + pk[np - 1].size;
        *last = bytes;
        return 0;
    };

    std::printf("BEGIN dx11 %dx%d budget=%zu\n", W, H, budget);
    std::fflush(stdout);
    RunResult rr = measure(step, 200);
    emit("dx11", W, H, budget, chosen, rr);
    enc_destroy(enc);
    destroy_dev(dev);
    return rr.encode_rc ? 1 : 0;
}

// Minimal Vulkan instance just to print the device the loader will hand out.
static void print_vk_devices(HMODULE vk)
{
    using PFN_vkGetInstanceProcAddr = void *(__stdcall *)(void *, const char *);
    using PFN_vkCreateInstance = int(__stdcall *)(const void *, const void *, void **);
    using PFN_vkDestroyInstance = void(__stdcall *)(void *, const void *);
    using PFN_vkEnum = int(__stdcall *)(void *, uint32_t *, void *);
    using PFN_vkProps = void(__stdcall *)(void *, void *);
    auto get_inst = (PFN_vkGetInstanceProcAddr)sym(vk, "vkGetInstanceProcAddr");
    if (!get_inst) {
        std::printf("VK_ENUM skipped (no vkGetInstanceProcAddr)\n");
        return;
    }
    // The Khronos loader exports vkGetInstanceProcAddr only. Global commands are queried with a null instance.
    auto create = (PFN_vkCreateInstance)get_inst(nullptr, "vkCreateInstance");
    if (!create) {
        std::printf("VK_ENUM vkCreateInstance unavailable\n");
        return;
    }
    struct App {
        uint32_t sType;
        const void *pNext;
        const char *pApplicationName;
        uint32_t applicationVersion;
        const char *pEngineName;
        uint32_t engineVersion;
        uint32_t apiVersion;
    } app = {0, nullptr, "pwcmpbench", 0, "pwcmpbench", 0, (1u << 22) | (2u << 12)};
    struct Inst {
        uint32_t sType;
        const void *pNext;
        uint32_t flags;
        const App *pApp;
        uint32_t layerCount;
        const char *const *layers;
        uint32_t extCount;
        const char *const *exts;
    } ci = {1, nullptr, 0, &app, 0, nullptr, 0, nullptr};
    void *instance = nullptr;
    int rc = create(&ci, nullptr, &instance);
    if (rc != 0 || !instance) {
        std::printf("VK_ENUM vkCreateInstance rc=%d\n", rc);
        return;
    }
    auto enumerate = (PFN_vkEnum)get_inst(instance, "vkEnumeratePhysicalDevices");
    auto props_fn = (PFN_vkProps)get_inst(instance, "vkGetPhysicalDeviceProperties");
    auto destroy = (PFN_vkDestroyInstance)get_inst(instance, "vkDestroyInstance");
    if (!enumerate || !props_fn) {
        std::printf("VK_ENUM missing enum procs\n");
        if (destroy) destroy(instance, nullptr);
        return;
    }
    uint32_t count = 0;
    enumerate(instance, &count, nullptr);
    std::vector<void *> gpus(count);
    if (count) enumerate(instance, &count, gpus.data());
    std::vector<unsigned char> blob(65536);
    for (uint32_t i = 0; i < count; i++) {
        std::memset(blob.data(), 0, blob.size());
        props_fn(gpus[i], blob.data());
        uint32_t vendor = 0, device = 0;
        std::memcpy(&vendor, blob.data() + 8, 4);
        std::memcpy(&device, blob.data() + 12, 4);
        char name[257] = {};
        std::memcpy(name, blob.data() + 20, 256);
        std::printf("VK[%u] %s vendor=%04x device=%04x\n", i, name, vendor, device);
    }
    if (destroy) destroy(instance, nullptr);
}

static int run_vk(int argc, char **argv)
{
    if (argc < 7) {
        std::fprintf(stderr, "usage: pwcmpbench vk <pyrowave dll> <vulkan dll or -> W H <vendor hex or 0>\n");
        return 2;
    }
    const char *py_path = argv[2];
    const char *vk_path = argv[3];
    int W = std::atoi(argv[4]);
    int H = std::atoi(argv[5]);
    unsigned vendor = 0;
    std::sscanf(argv[6], "%x", &vendor);
    if (W < 16 || H < 16) return 2;

    HMODULE vkmod = nullptr;
    if (vk_path[0] == '-' && vk_path[1] == '\0') {
        vkmod = LoadLibraryA("vulkan-1.dll");
        if (!vkmod) {
            std::fprintf(stderr, "LoadLibrary vulkan-1.dll failed %lu\n", GetLastError());
            return 1;
        }
    } else {
        // Keep a side-by-side loader dependency resolvable beside vulkan-1.dll.
        char dir[MAX_PATH];
        std::snprintf(dir, sizeof(dir), "%s", vk_path);
        char *slash = std::strrchr(dir, '\\');
        if (!slash) slash = std::strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            SetDllDirectoryA(dir);
        }
        vkmod = (HMODULE)load_lib(vk_path);
        if (!vkmod) {
            std::fprintf(stderr, "preload %s failed %lu\n", vk_path, GetLastError());
            return 1;
        }
    }
    if (!getenv("PWCMP_SKIP_ENUM")) print_vk_devices(vkmod);

    HMODULE py = (HMODULE)load_lib(py_path);
    if (!py) {
        std::fprintf(stderr, "LoadLibrary %s failed %lu\n", py_path, GetLastError());
        return 1;
    }
    auto mk_default = (PFN_vk_default)sym(py, "pyrowave_create_default_device");
    auto mk_compat = (PFN_vk_compat)sym(py, "pyrowave_create_device_by_compat");
    auto enc_create = (PFN_vk_enc_create)sym(py, "pyrowave_encoder_create");
    auto encode = (PFN_vk_encode)sym(py, "pyrowave_encoder_encode_cpu");
    auto numpk = (PFN_vk_num)sym(py, "pyrowave_encoder_compute_num_packets");
    auto packetize = (PFN_vk_packet)sym(py, "pyrowave_encoder_packetize");
    auto enc_destroy = (PFN_vk_enc_destroy)sym(py, "pyrowave_encoder_destroy");
    auto dev_destroy = (PFN_vk_dev_destroy)sym(py, "pyrowave_device_destroy");
    if (!mk_default || !enc_create || !encode || !numpk || !packetize || !enc_destroy || !dev_destroy) {
        std::fprintf(stderr, "pyrowave dll is missing an export\n");
        return 1;
    }

    void *dev = nullptr;
    int rc;
    // VK_QUEUE_GLOBAL_PRIORITY_MEDIUM = 256.
    if (vendor && mk_compat) rc = mk_compat(vendor, 0, nullptr, nullptr, nullptr, 256, &dev);
    else rc = mk_default(&dev);
    if (rc != 0 || !dev) {
        std::fprintf(stderr, "create device failed %d (vendor %04x)\n", rc, vendor);
        return 1;
    }

    VkEncInfo ei = {};
    ei.device = dev;
    ei.width = W;
    ei.height = H;
    ei.chroma = 0;
    void *enc = nullptr;
    rc = enc_create(&ei, &enc);
    if (rc != 0 || !enc) {
        std::fprintf(stderr, "encoder_create failed %d\n", rc);
        dev_destroy(dev);
        return 1;
    }

    auto y = make_plane(W, H, 1);
    auto cb = make_plane(W / 2, H / 2, 2);
    auto cr = make_plane(W / 2, H / 2, 3);
    VkBuffer buf = {};
    buf.data[0] = y.data();
    buf.data[1] = cb.data();
    buf.data[2] = cr.data();
    buf.row_stride[0] = size_t(W);
    buf.row_stride[1] = size_t(W / 2);
    buf.row_stride[2] = size_t(W / 2);
    buf.plane_size[0] = y.size();
    buf.plane_size[1] = cb.size();
    buf.plane_size[2] = cr.size();
    buf.width = W;
    buf.height = H;
    buf.format = 1; // YUV420P
    size_t budget = default_budget(W, H);
    VkRate rate = {budget};
    std::vector<uint8_t> out(budget + (1u << 20));
    std::vector<VkPacket> pk(65536);

    auto step = [&](size_t *last) -> int {
        int r = encode(enc, &buf, &rate);
        if (r != 0) return r;
        size_t n = 0, np = 0;
        r = numpk(enc, out.size(), &n);
        if (r != 0) return r;
        if (n > pk.size()) return -100;
        r = packetize(enc, pk.data(), out.size(), &np, out.data(), out.size());
        if (r != 0) return r;
        size_t bytes = 0;
        if (np && np <= pk.size()) bytes = pk[np - 1].offset + pk[np - 1].size;
        *last = bytes;
        return 0;
    };

    const char *backend = "vulkan";
    if (argc >= 8 && argv[7][0]) backend = argv[7];
    else if (!(vk_path[0] == '-' && vk_path[1] == '\0')) backend = "cpu";
    char device_label[160];
    std::snprintf(device_label, sizeof(device_label), "vendor=%04x", vendor);
    std::printf("BEGIN %s %dx%d budget=%zu vendor=%04x\n", backend, W, H, budget, vendor);
    std::fflush(stdout);
    RunResult rr = measure(step, 200);
    emit(backend, W, H, budget, device_label, rr);
    enc_destroy(enc);
    dev_destroy(dev);
    return rr.encode_rc ? 1 : 0;
}

int main(int argc, char **argv)
{
    g_procs = logical_processors();
    char host[256] = {};
    DWORD n = sizeof(host);
    GetComputerNameA(host, &n);
    std::printf("HOST %s procs=%d\n", host, g_procs);
    if (argc < 2) {
        std::fprintf(stderr, "usage: pwcmpbench dx11|vk ...\n");
        return 2;
    }
    if (std::strcmp(argv[1], "dx11") == 0) return run_dx11(argc, argv);
    if (std::strcmp(argv[1], "vk") == 0) return run_vk(argc, argv);
    std::fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 2;
}
