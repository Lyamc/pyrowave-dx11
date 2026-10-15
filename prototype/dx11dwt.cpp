// dx11dwt: feasibility prototype for porting PyroWave's forward DWT to D3D11 (FL 11_0, cs_5_0).
// Build (MSVC x64): cl /O2 /EHsc /arch:AVX2 dx11dwt.cpp   (links d3d11, d3dcompiler, dxgi)
// Run: dx11dwt.exe [runs=200]   (dwt.hlsl must be next to the exe or in cwd)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <immintrin.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
using Microsoft::WRL::ComPtr;

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { fprintf(stderr, "FAILED 0x%08lx: %s (line %d)\n", (unsigned long)hr_, #x, __LINE__); exit(1); } } while (0)

static const int Levels = 5;
static inline uint16_t f2h(float f) { return uint16_t(_mm_extract_epi16(_mm_cvtps_ph(_mm_set_ss(f), _MM_FROUND_TO_NEAREST_INT), 0)); }
static inline float h2f(uint16_t h) { return _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))); }
static float rh(float f) { return h2f(f2h(f)); } // round-to-nearest-even through fp16

// ---------------- CPU reference (same transform, same edge addressing) ----------------
struct Plane { int w = 0, h = 0; std::vector<float> d; float at(int x, int y) const { return d[size_t(y) * w + x]; } };
struct Bands { int w = 0, h = 0; Plane b[4]; };

static int mirror_tex(int i, int R) { int p = 2 * R; int t = ((i % p) + p) % p; return t >= R ? p - 1 - t : t; }
// Replicates generate_mirror_uv() + textureGather footprint + MIRRORED_REPEAT for absolute coord c.
static int map_coord(int c, int R, int A)
{
    int c0 = c & ~1;
    int g = c0;
    if (g < 0) g -= 1;
    g += 1;
    int endc = 2 * A - R;
    int past = g + 2 * (R - A) + 1;
    g = (g >= endc) ? past : std::min(g, R);
    return (c & 1) ? mirror_tex(g, R) : mirror_tex(g - 1, R);
}

static const float ALPHA = -1.586134342059924f, BETA = -0.052980118572961f, GAMMA = 0.882911075530934f,
                   DELTA = 0.443506852043971f, KK = 1.230174104914001f, INV_K = float(1.0 / 1.230174104914001);

static void lift(float *v, int n, int s, bool emu)
{
    for (int i = 1; i < n - 1; i += 2) v[i * s] += ALPHA * (v[(i - 1) * s] + v[(i + 1) * s]);
    for (int i = 2; i < n - 2; i += 2) v[i * s] += BETA * (v[(i - 1) * s] + v[(i + 1) * s]);
    for (int i = 3; i < n - 3; i += 2) v[i * s] += GAMMA * (v[(i - 1) * s] + v[(i + 1) * s]);
    for (int i = 4; i < n - 4; i += 2) v[i * s] += DELTA * (v[(i - 1) * s] + v[(i + 1) * s]);
    for (int i = 0; i < n; i++) { float f = v[i * s] * ((i & 1) ? KK : INV_K); v[i * s] = emu ? rh(f) : f; }
}

// One analysis level. src has resolution R (=src.w/h); A = aligned resolution; output A/2.
static Bands ref_level(const Plane &src, int Aw, int Ah, bool dc, bool emu)
{
    int Rw = src.w, Rh = src.h, Ew = Aw + 8, Eh = Ah + 8;
    std::vector<float> E(size_t(Ew) * Eh);
    for (int y = 0; y < Eh; y++)
    {
        int sy = map_coord(y - 4, Rh, Ah);
        for (int x = 0; x < Ew; x++)
        {
            float v = src.at(map_coord(x - 4, Rw, Aw), sy);
            if (dc) v -= 0.5f;
            E[size_t(y) * Ew + x] = emu ? rh(v) : v;
        }
    }
    for (int y = 0; y < Eh; y++) lift(&E[size_t(y) * Ew], Ew, 1, emu);
    for (int x = 0; x < Ew; x++) lift(&E[x], Eh, Ew, emu);
    Bands o; o.w = Aw / 2; o.h = Ah / 2;
    for (int b = 0; b < 4; b++) { o.b[b].w = o.w; o.b[b].h = o.h; o.b[b].d.resize(size_t(o.w) * o.h); }
    for (int y = 0; y < o.h; y++)
        for (int x = 0; x < o.w; x++)
        {
            int ex = 4 + 2 * x, ey = 4 + 2 * y;
            size_t i = size_t(y) * o.w + x;
            o.b[0].d[i] = E[size_t(ey) * Ew + ex];
            o.b[1].d[i] = E[size_t(ey) * Ew + ex + 1];
            o.b[2].d[i] = E[size_t(ey + 1) * Ew + ex];
            o.b[3].d[i] = E[size_t(ey + 1) * Ew + ex + 1];
        }
    return o;
}

// ---------------- test image ----------------
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

// ---------------- D3D11 ----------------
static ComPtr<ID3D11Device> dev;
static ComPtr<ID3D11DeviceContext> ctx;

struct Push { int rx, ry; float ix, iy; int ax, ay; uint32_t dc; float rs; };
struct OutTex { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11UnorderedAccessView> uav; ComPtr<ID3D11ShaderResourceView> srv; int w = 0, h = 0; DXGI_FORMAT fmt{}; };
struct Disp { ID3D11ShaderResourceView *srv; ID3D11UnorderedAccessView *uav; ComPtr<ID3D11Buffer> cb; UINT gx, gy; int level; };

static ComPtr<ID3D11ShaderResourceView> make_srv_array1(ID3D11Texture2D *t, DXGI_FORMAT f)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = f; d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    d.Texture2DArray.MipLevels = 1; d.Texture2DArray.ArraySize = 1;
    ComPtr<ID3D11ShaderResourceView> v; CHECK(dev->CreateShaderResourceView(t, &d, &v)); return v;
}

static OutTex make_out(int w, int h, DXGI_FORMAT f)
{
    OutTex o; o.w = w; o.h = h; o.fmt = f;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 4; td.Format = f; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    CHECK(dev->CreateTexture2D(&td, nullptr, &o.tex));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = f; ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY; ud.Texture2DArray.ArraySize = 4;
    CHECK(dev->CreateUnorderedAccessView(o.tex.Get(), &ud, &o.uav));
    o.srv = make_srv_array1(o.tex.Get(), f);
    return o;
}

static std::vector<float> readback_layer(const OutTex &o, int layer)
{
    D3D11_TEXTURE2D_DESC td; o.tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st; CHECK(dev->CreateTexture2D(&td, nullptr, &st));
    ctx->CopyResource(st.Get(), o.tex.Get());
    D3D11_MAPPED_SUBRESOURCE m; CHECK(ctx->Map(st.Get(), layer, D3D11_MAP_READ, 0, &m));
    std::vector<float> r(size_t(o.w) * o.h);
    for (int y = 0; y < o.h; y++)
        for (int x = 0; x < o.w; x++)
        {
            const uint8_t *row = (const uint8_t *)m.pData + size_t(y) * m.RowPitch;
            r[size_t(y) * o.w + x] = o.fmt == DXGI_FORMAT_R16_FLOAT ? h2f(((const uint16_t *)row)[x]) : ((const float *)row)[x];
        }
    ctx->Unmap(st.Get(), layer);
    return r;
}

static double pct(std::vector<double> v, double p)
{
    std::sort(v.begin(), v.end());
    size_t i = size_t(std::ceil(p * v.size())) ; if (i > 0) i--; if (i >= v.size()) i = v.size() - 1;
    return v[i];
}

struct GpuTimer
{
    ComPtr<ID3D11Query> disjoint; std::vector<ComPtr<ID3D11Query>> ts;
    GpuTimer(int n)
    {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }; CHECK(dev->CreateQuery(&qd, &disjoint));
        ts.resize(n); qd.Query = D3D11_QUERY_TIMESTAMP; for (auto &q : ts) CHECK(dev->CreateQuery(&qd, &q));
    }
    // returns ms deltas between consecutive stamps, or empty if disjoint
    std::vector<double> collect()
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        while (ctx->GetData(disjoint.Get(), &dj, sizeof(dj), 0) != S_OK) {}
        std::vector<UINT64> t(ts.size());
        for (size_t i = 0; i < ts.size(); i++) while (ctx->GetData(ts[i].Get(), &t[i], sizeof(UINT64), 0) != S_OK) {}
        if (dj.Disjoint) return {};
        std::vector<double> r;
        for (size_t i = 1; i < t.size(); i++) r.push_back(double(t[i] - t[i - 1]) * 1000.0 / double(dj.Frequency));
        return r;
    }
};

static void run_resolution(int W, int H, int runs, ID3D11ComputeShader *cs, ID3D11SamplerState *smp)
{
    const int Aw = (W + 31) & ~31, Ah = (H + 31) & ~31;
    const int CW = W / 2, CH = H / 2;
    printf("\n=== %dx%d YCbCr 4:2:0 (aligned %dx%d), %d levels ===\n", W, H, Aw, Ah, Levels);

    // Source planes (R8_UNORM), as uploaded by an encoder.
    std::vector<uint8_t> src[3] = { make_plane(W, H, 1), make_plane(CW, CH, 2), make_plane(CW, CH, 3) };
    int pw[3] = { W, CW, CW }, ph[3] = { H, CH, CH };
    ComPtr<ID3D11Texture2D> plane[3], staging[3];
    ComPtr<ID3D11ShaderResourceView> plane_srv[3];
    for (int c = 0; c < 3; c++)
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = pw[c]; td.Height = ph[c]; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = { src[c].data(), UINT(pw[c]), 0 };
        CHECK(dev->CreateTexture2D(&td, &sd, &plane[c]));
        plane_srv[c] = make_srv_array1(plane[c].Get(), DXGI_FORMAT_R8_UNORM);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        CHECK(dev->CreateTexture2D(&td, nullptr, &staging[c]));
    }

    // Wavelet outputs: level 0-1 R16F, levels 2-4 R32F (PyroWave PRECISION=1 layout). Chroma has no level 0 for 4:2:0.
    OutTex out[3][Levels];
    for (int c = 0; c < 3; c++)
        for (int l = (c ? 1 : 0); l < Levels; l++)
            out[c][l] = make_out(Aw >> (l + 1), Ah >> (l + 1), l < 2 ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT);

    // Dispatch list, identical order/params to Encoder::Impl::dwt().
    std::vector<Disp> disps;
    auto add = [&](ID3D11ShaderResourceView *s, ID3D11UnorderedAccessView *u, int rx, int ry, int ax, int ay, bool dc, int level) {
        Push p = { rx, ry, 1.f / rx, 1.f / ry, ax, ay, dc ? 1u : 0u, 1.f };
        D3D11_BUFFER_DESC bd = { sizeof(Push), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA sd = { &p, 0, 0 };
        Disp d; d.srv = s; d.uav = u; d.level = level; CHECK(dev->CreateBuffer(&bd, &sd, &d.cb));
        d.gx = UINT((ax + 31) / 32); d.gy = UINT((ay + 31) / 32);
        disps.push_back(d);
    };
    add(plane_srv[0].Get(), out[0][0].uav.Get(), W, H, Aw, Ah, true, 0);
    for (int l = 1; l < Levels; l++)
        for (int c = 0; c < 3; c++)
        {
            if (c && l == 1) add(plane_srv[c].Get(), out[c][1].uav.Get(), CW, CH, Aw >> 1, Ah >> 1, true, 1);
            else { auto &p = out[c][l - 1]; add(p.srv.Get(), out[c][l].uav.Get(), p.w, p.h, p.w, p.h, false, l); }
        }

    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetSamplers(0, 1, &smp);
    ID3D11ShaderResourceView *nullsrv = nullptr; ID3D11UnorderedAccessView *nulluav = nullptr;

    GpuTimer timer(Levels + 1);
    auto record = [&](bool timed) {
        if (timed) { ctx->Begin(timer.disjoint.Get()); ctx->End(timer.ts[0].Get()); }
        int cur = 0;
        for (auto &d : disps)
        {
            if (d.level != cur) { if (timed) ctx->End(timer.ts[cur + 1].Get()); cur = d.level; }
            ctx->CSSetUnorderedAccessViews(0, 1, &d.uav, nullptr);
            ctx->CSSetShaderResources(0, 1, &d.srv);
            ctx->CSSetConstantBuffers(0, 1, d.cb.GetAddressOf());
            ctx->Dispatch(d.gx, d.gy, 1);
        }
        ctx->CSSetShaderResources(0, 1, &nullsrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &nulluav, nullptr);
        if (timed) { ctx->End(timer.ts[Levels].Get()); ctx->End(timer.disjoint.Get()); }
    };

    // ---- correctness ----
    record(false);
    Plane sp[3];
    for (int c = 0; c < 3; c++) { sp[c].w = pw[c]; sp[c].h = ph[c]; sp[c].d.resize(src[c].size()); for (size_t i = 0; i < src[c].size(); i++) sp[c].d[i] = src[c][i] / 255.f; }
    printf("Correctness vs CPU reference (max abs error; ref_fp16 = CPU emulating fp16 shared/storage rounding, ref_fp32 = pure fp32 CPU):\n");
    double worst16 = 0, worst32 = 0;
    for (int c = 0; c < 3; c++)
    {
        Plane in16 = sp[c], in32 = sp[c];
        for (int l = (c ? 1 : 0); l < Levels; l++)
        {
            bool first = (l == (c ? 1 : 0));
            int ax = first ? (c ? Aw >> 1 : Aw) : in16.w, ay = first ? (c ? Ah >> 1 : Ah) : in16.h;
            Bands r16 = ref_level(in16, ax, ay, first, true);
            Bands r32 = ref_level(in32, ax, ay, first, false);
            double e16 = 0, e32 = 0, maxmag = 0;
            for (int b = 0; b < 4; b++)
            {
                auto g = readback_layer(out[c][l], b);
                for (size_t i = 0; i < g.size(); i++)
                {
                    e16 = std::max(e16, double(std::fabs(g[i] - r16.b[b].d[i])));
                    e32 = std::max(e32, double(std::fabs(g[i] - r32.b[b].d[i])));
                    maxmag = std::max(maxmag, double(std::fabs(r32.b[b].d[i])));
                }
            }
            printf("  comp %d level %d (%4dx%-4d x4 bands, max|coef| %7.3f): vs ref_fp16 %.3e   vs ref_fp32 %.3e\n", c, l, r16.w, r16.h, maxmag, e16, e32);
            worst16 = std::max(worst16, e16); worst32 = std::max(worst32, e32);
            in16 = r16.b[0]; in32 = r32.b[0];
        }
    }
    printf("  WORST: vs ref_fp16 %.3e, vs ref_fp32 %.3e\n", worst16, worst32);

    // ---- DWT timing (no upload) ----
    for (int i = 0; i < 20; i++) { record(true); timer.collect(); }
    std::vector<double> total, per[Levels];
    int disjoint = 0;
    for (int i = 0; i < runs; i++)
    {
        record(true);
        auto r = timer.collect();
        if (r.empty()) { disjoint++; continue; }
        double t = 0; for (int l = 0; l < Levels; l++) { per[l].push_back(r[l]); t += r[l]; }
        total.push_back(t);
    }
    printf("DWT GPU time over %zu runs (%d disjoint dropped): median %.3f ms, p90 %.3f ms, min %.3f ms\n",
           total.size(), disjoint, pct(total, 0.5), pct(total, 0.9), pct(total, 0.0));
    for (int l = 0; l < Levels; l++) printf("  level %d: median %.3f ms  p90 %.3f ms\n", l, pct(per[l], 0.5), pct(per[l], 0.9));

    // ---- Upload timing: staging Map+memcpy (CPU) + CopyResource (GPU) ----
    GpuTimer ut(2);
    ComPtr<ID3D11Query> ev; D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 }; CHECK(dev->CreateQuery(&qd, &ev));
    std::vector<double> up_cpu, up_gpu, up_wall, us_wall;
    for (int i = 0; i < runs + 10; i++)
    {
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int c = 0; c < 3; c++)
        {
            D3D11_MAPPED_SUBRESOURCE m; CHECK(ctx->Map(staging[c].Get(), 0, D3D11_MAP_WRITE, 0, &m));
            for (int y = 0; y < ph[c]; y++) memcpy((uint8_t *)m.pData + size_t(y) * m.RowPitch, &src[c][size_t(y) * pw[c]], pw[c]);
            ctx->Unmap(staging[c].Get(), 0);
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        ctx->Begin(ut.disjoint.Get()); ctx->End(ut.ts[0].Get());
        for (int c = 0; c < 3; c++) ctx->CopyResource(plane[c].Get(), staging[c].Get());
        ctx->End(ut.ts[1].Get()); ctx->End(ut.disjoint.Get()); ctx->End(ev.Get());
        while (ctx->GetData(ev.Get(), nullptr, 0, 0) != S_OK) {}
        auto t2 = std::chrono::high_resolution_clock::now();
        auto g = ut.collect();
        // UpdateSubresource path (driver-managed staging), wall time to GPU completion
        auto t3 = std::chrono::high_resolution_clock::now();
        for (int c = 0; c < 3; c++) ctx->UpdateSubresource(plane[c].Get(), 0, nullptr, src[c].data(), pw[c], 0);
        ctx->End(ev.Get());
        while (ctx->GetData(ev.Get(), nullptr, 0, 0) != S_OK) {}
        auto t4 = std::chrono::high_resolution_clock::now();
        if (i < 10 || g.empty()) continue;
        up_cpu.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        up_wall.push_back(std::chrono::duration<double, std::milli>(t2 - t0).count());
        up_gpu.push_back(g[0]);
        us_wall.push_back(std::chrono::duration<double, std::milli>(t4 - t3).count());
    }
    double mb = (W * H + 2.0 * CW * CH) / 1e6;
    printf("Upload of one 8-bit 4:2:0 frame (%.2f MB):\n", mb);
    printf("  staging Map+memcpy (CPU)        median %.3f ms  p90 %.3f ms\n", pct(up_cpu, 0.5), pct(up_cpu, 0.9));
    printf("  CopyResource staging->VRAM (GPU) median %.3f ms  p90 %.3f ms  (%.2f GB/s)\n", pct(up_gpu, 0.5), pct(up_gpu, 0.9), mb / pct(up_gpu, 0.5));
    printf("  staging path wall, to GPU done  median %.3f ms  p90 %.3f ms\n", pct(up_wall, 0.5), pct(up_wall, 0.9));
    printf("  UpdateSubresource wall, to done median %.3f ms  p90 %.3f ms\n", pct(us_wall, 0.5), pct(us_wall, 0.9));
}

int main(int argc, char **argv)
{
    int runs = argc > 1 ? atoi(argv[1]) : 200;
    ComPtr<IDXGIFactory1> fac; CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&fac)));
    ComPtr<IDXGIAdapter1> chosen;
    for (UINT i = 0;; i++)
    {
        ComPtr<IDXGIAdapter1> a; if (fac->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
        printf("Adapter %u: %ls (VRAM %zu MB)%s\n", i, d.Description, d.DedicatedVideoMemory >> 20, (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " [software]" : "");
        if (!chosen && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) chosen = a;
    }
    D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0 }, got;
    CHECK(D3D11CreateDevice(chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fls, 1, D3D11_SDK_VERSION, &dev, &got, &ctx));
    DXGI_ADAPTER_DESC1 d; chosen->GetDesc1(&d);
    printf("Using: %ls, feature level 0x%x\n", d.Description, got);

    UINT fs = 0;
    dev->CheckFormatSupport(DXGI_FORMAT_R16_FLOAT, &fs);
    printf("R16_FLOAT typed UAV store: %s\n", (fs & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) ? "yes" : "NO");

    ComPtr<ID3DBlob> blob, err;
    const wchar_t *paths[] = { L"dwt.hlsl", L"proto\\dx11-dwt\\dwt.hlsl" };
    HRESULT hr = E_FAIL;
    for (auto p : paths) { hr = D3DCompileFromFile(p, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err); if (SUCCEEDED(hr) || err) break; }
    if (FAILED(hr)) { fprintf(stderr, "Compile failed: %s\n", err ? (const char *)err->GetBufferPointer() : "dwt.hlsl not found"); return 1; }
    ComPtr<ID3D11ComputeShader> cs; CHECK(dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs));
    printf("cs_5_0 DXBC size: %zu bytes\n", blob->GetBufferSize());

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
    sd.MaxLOD = D3D11_FLOAT32_MAX; sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ComPtr<ID3D11SamplerState> smp; CHECK(dev->CreateSamplerState(&sd, &smp));

    run_resolution(1280, 720, runs, cs.Get(), smp.Get());
    run_resolution(1920, 1080, runs, cs.Get(), smp.Get());
    return 0;
}
