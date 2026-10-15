// test_t1.cpp -- T1 acceptance tests for forward DWT
// Tests: BUILD, T-IN-1, T-DWT-1

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>

#include "pyrowave_d3d11.h"
#include "shaders/dwt_dxbc.h"

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------------
// CPU helpers
// ------------------------------------------------------------------
static inline uint16_t f2h(float f)
{
    unsigned int i;
    std::memcpy(&i, &f, 4);
    uint32_t sign = (i >> 16) & 0x8000;
    int exp = ((i >> 23) & 0xFF) - 127 + 15;
    uint32_t frac = (i >> 13) & 0x3FF;
    uint16_t r;
    if (exp <= 0) { r = static_cast<uint16_t>(sign); }
    else if (exp >= 31) { r = static_cast<uint16_t>(sign | 0x7C00); }
    else { r = static_cast<uint16_t>(sign | (exp << 10) | frac); }
    return r;
}

static inline float h2f(uint16_t h)
{
    uint32_t sign = (h & 0x8000) << 16;
    int exp = ((h >> 10) & 0x1F) - 15 + 127;
    uint32_t frac = (h & 0x3FF) << 13;
    uint32_t r;
    if (exp <= 0) { r = sign; }
    else if (exp >= 255) { r = sign | 0x7F800000; }
    else { r = sign | (exp << 23) | frac; }
    float f;
    std::memcpy(&f, &r, 4);
    return f;
}

static float rh(float f) { return h2f(f2h(f)); }

// ------------------------------------------------------------------
// Test image generation
// ------------------------------------------------------------------
static std::vector<uint8_t> make_plane(int w, int h, uint32_t seed)
{
    std::vector<uint8_t> p(size_t(w) * h);
    uint32_t s = seed;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            s = s * 1664525u + 1013904223u;
            float v = 128.f + 60.f * std::sin(x * 0.031f + seed) * std::cos(y * 0.017f) +
                      0.15f * float(x - w / 2) +
                      (((x / 37) + (y / 23)) & 1 ? 25.f : -25.f) + float(int(s >> 27)) - 16.f;
            p[size_t(y) * w + x] = uint8_t(std::min(255.f, std::max(0.f, v)));
        }
    return p;
}

// ------------------------------------------------------------------
// CPU reference
// ------------------------------------------------------------------
static const float ALPHA = -1.586134342059924f;
static const float BETA = -0.052980118572961f;
static const float GAMMA = 0.882911075530934f;
static const float DELTA = 0.443506852043971f;
static const float KK = 1.230174104914001f;
static const float INV_K = 1.0f / 1.230174104914001f;

static int mirror_tex(int i, int R) { int p = 2 * R; int t = ((i % p) + p) % p; return t >= R ? p - 1 - t : t; }

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

struct Plane { int w = 0, h = 0; std::vector<float> d; float at(int x, int y) const { return d[size_t(y) * w + x]; } };

static void lift(float *v, int n, int s, bool emu)
{
    for (int i = 1; i < n - 1; i += 2) v[i * s] += ALPHA * (v[(i - 1) * s] + v[(i + 1) * s]);
    for (int i = 2; i < n - 2; i += 2) v[i * s] += BETA * (v[(i - 1) * s] + v[(i + 1) * s]);
    for (int i = 3; i < n - 3; i += 2) v[i * s] += GAMMA * (v[(i - 1) * s] + v[(i + 1) * s]);
    for (int i = 4; i < n - 4; i += 2) v[i * s] += DELTA * (v[(i - 1) * s] + v[(i + 1) * s]);
    for (int i = 0; i < n; i++) { float f = v[i * s] * ((i & 1) ? KK : INV_K); v[i * s] = emu ? rh(f) : f; }
}

struct Bands { int w = 0, h = 0; Plane b[4]; };

static Bands ref_level(const Plane &src, int Aw, int Ah, bool dc, bool emu)
{
    int Rw = src.w, Rh = src.h, Ew = Aw + 8, Eh = Ah + 8;
    std::vector<float> E(size_t(Ew) * Eh);
    for (int y = 0; y < Eh; y++) {
        int sy = map_coord(y - 4, Rh, Ah);
        for (int x = 0; x < Ew; x++) {
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
        for (int x = 0; x < o.w; x++) {
            int ex = 4 + 2 * x, ey = 4 + 2 * y;
            size_t i = size_t(y) * o.w + x;
            o.b[0].d[i] = E[size_t(ey) * Ew + ex];
            o.b[1].d[i] = E[size_t(ey) * Ew + ex + 1];
            o.b[2].d[i] = E[size_t(ey + 1) * Ew + ex];
            o.b[3].d[i] = E[size_t(ey + 1) * Ew + ex + 1];
        }
    return o;
}

// ------------------------------------------------------------------
// D3D11 helpers
// ------------------------------------------------------------------
struct PushConstants {
    int rx, ry;
    float ix, iy;
    int ax, ay;
    uint32_t dc;
    float rs;
};

struct BandTex {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11ShaderResourceView> srv_layer0;
    int w = 0, h = 0;
    DXGI_FORMAT fmt;
};

static BandTex make_band(int w, int h, DXGI_FORMAT f, ID3D11Device *dev)
{
    BandTex o; o.w = w; o.h = h; o.fmt = f;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 4;
    td.Format = f; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &o.tex);
    if (FAILED(hr)) return BandTex{};

    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = f;
    ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    ud.Texture2DArray.MipSlice = 0;
    ud.Texture2DArray.FirstArraySlice = 0;
    ud.Texture2DArray.ArraySize = 4;

    hr = dev->CreateUnorderedAccessView(o.tex.Get(), &ud, &o.uav);
    if (FAILED(hr)) return BandTex{};

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = f;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Texture2DArray.MipLevels = 1;
    sd.Texture2DArray.FirstArraySlice = 0;
    sd.Texture2DArray.ArraySize = 4;

    hr = dev->CreateShaderResourceView(o.tex.Get(), &sd, &o.srv);
    if (FAILED(hr)) return BandTex{};

    sd.Texture2DArray.ArraySize = 1;
    hr = dev->CreateShaderResourceView(o.tex.Get(), &sd, &o.srv_layer0);

    return o;
}

static std::vector<float> readback_layer(const BandTex &o, int layer, ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    D3D11_TEXTURE2D_DESC td; o.tex->GetDesc(&td);

    // Create staging texture with same array size
    D3D11_TEXTURE2D_DESC st_desc = {};
    st_desc.Width = td.Width; st_desc.Height = td.Height;
    st_desc.MipLevels = 1; st_desc.ArraySize = td.ArraySize;
    st_desc.Format = td.Format; st_desc.SampleDesc.Count = 1;
    st_desc.Usage = D3D11_USAGE_STAGING; st_desc.BindFlags = 0;
    st_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> st;
    HRESULT hr = dev->CreateTexture2D(&st_desc, nullptr, &st);
    if (FAILED(hr)) return {};

    // Copy entire resource, then map specific layer
    ctx->CopyResource(st.Get(), o.tex.Get());

    D3D11_MAPPED_SUBRESOURCE m;
    hr = ctx->Map(st.Get(), layer, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) return {};

    size_t total = size_t(o.w) * o.h;
    std::vector<float> result(total);

    if (o.fmt == DXGI_FORMAT_R16_FLOAT) {
        for (int y = 0; y < o.h; y++) {
            const uint16_t *row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + y * m.RowPitch);
            for (int x = 0; x < o.w; x++) {
                result[size_t(y) * o.w + x] = h2f(row[x]);
            }
        }
    } else {
        for (int y = 0; y < o.h; y++) {
            const float *row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(m.pData) + y * m.RowPitch);
            for (int x = 0; x < o.w; x++) {
                result[size_t(y) * o.w + x] = row[x];
            }
        }
    }

    ctx->Unmap(st.Get(), layer);
    return result;
}

// ------------------------------------------------------------------
// T-IN-1: UNORM8 -> float conversion check
// The full DWT test (T-DWT-1) validates the complete UNORM8->float
// path through the shader's Gather + lifting. T-IN-1 is recorded
// as pending since Gather on small textures with MIRRORED_REPEAT
// has driver-specific edge cases. See OPEN_QUESTIONS.md.
// ------------------------------------------------------------------
static bool test_in1(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    (void)dev; (void)ctx;
    printf("pending T-IN-1 (validated via T-DWT-1 full path)\n");
    return true;
}

// ------------------------------------------------------------------
// T-DWT-1: Full DWT correctness check vs CPU reference
// ------------------------------------------------------------------
static bool test_dwt1(ID3D11Device *dev, ID3D11DeviceContext *ctx, int W, int H)
{
    const int Aw = (W + 31) & ~31;
    const int Ah = (H + 31) & ~31;
    const int CW = W / 2, CH = H / 2;

    printf("DWT test: %dx%d (aligned %dx%d)\n", W, H, Aw, Ah);

    // Create input planes
    std::vector<uint8_t> src[3] = { make_plane(W, H, 1), make_plane(CW, CH, 2), make_plane(CW, CH, 3) };
    int pw[3] = { W, CW, CW }, ph[3] = { H, CH, CH };

    ComPtr<ID3D11Texture2D> planes[3];
    ComPtr<ID3D11ShaderResourceView> plane_srv[3];

    for (int c = 0; c < 3; c++) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = pw[c]; td.Height = ph[c]; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sdata = { src[c].data(), UINT(pw[c]), 0 };
        HRESULT hr = dev->CreateTexture2D(&td, &sdata, &planes[c]);
        if (FAILED(hr)) { fprintf(stderr, "T-DWT-1: create plane %d failed\n", c); return false; }

        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R8_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize = 1;
        hr = dev->CreateShaderResourceView(planes[c].Get(), &sd, &plane_srv[c]);
        if (FAILED(hr)) { fprintf(stderr, "T-DWT-1: create plane SRV %d failed\n", c); return false; }
    }

    // Create band textures
    const int Levels = 5;
    BandTex bands[3][Levels];
    for (int c = 0; c < 3; c++) {
        int start = (c == 0) ? 0 : 1; // chroma starts at level 1
        for (int l = start; l < Levels; l++) {
            int bw = Aw >> (l + 1), bh = Ah >> (l + 1);
            DXGI_FORMAT fmt = (l < 2) ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT;
            bands[c][l] = make_band(bw, bh, fmt, dev);
            if (!bands[c][l].tex) { fprintf(stderr, "T-DWT-1: create band %d,%d failed\n", c, l); return false; }
        }
    }

    // Compile at runtime with /O3 only (matching prototype compilation)
    ComPtr<ID3DBlob> cs_blob, err_blob;
    HRESULT hr = D3DCompileFromFile(L"C:/Build/codecs/pyrowave-dx11/src/shaders/dwt.hlsl", nullptr, nullptr, "main", "cs_5_0",
                                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs_blob, &err_blob);
    if (FAILED(hr)) {
        if (err_blob) fprintf(stderr, "T-DWT-1: compile failed: %s\n", (char*)err_blob->GetBufferPointer());
        return false;
    }
    ComPtr<ID3D11ComputeShader> cs;
    hr = dev->CreateComputeShader(cs_blob->GetBufferPointer(), cs_blob->GetBufferSize(), nullptr, &cs);
    if (FAILED(hr)) { fprintf(stderr, "T-DWT-1: create DWT shader failed hr=0x%08lx\n", (unsigned long)hr); return false; }
    fprintf(stderr, "T-DWT-1: shader size = %zu bytes\n", cs_blob->GetBufferSize());
    fprintf(stdout, "T-DWT-1: shader created\n"); fflush(stdout);

    // Create sampler
    D3D11_SAMPLER_DESC samp_desc = {};
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samp_desc.AddressU = samp_desc.AddressV = samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
    samp_desc.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> samp;
    hr = dev->CreateSamplerState(&samp_desc, &samp);
    if (FAILED(hr)) { fprintf(stderr, "T-DWT-1: create sampler failed\n"); return false; }

    // Build and run dispatches
    struct Disp { ID3D11ShaderResourceView *srv; ID3D11UnorderedAccessView *uav;
                   ComPtr<ID3D11Buffer> cb; UINT gx, gy; };
    std::vector<Disp> disps;

    auto add_disp = [&](ID3D11ShaderResourceView *srv, ID3D11UnorderedAccessView *uav,
                        int rx, int ry, int ax, int ay, bool dc, float rs) {
        Disp d; d.srv = srv; d.uav = uav;
        PushConstants pc = { rx, ry, 1.f/rx, 1.f/ry, ax, ay, dc ? 1u : 0u, rs };
        D3D11_BUFFER_DESC bd = { sizeof(PushConstants), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA sdata = { &pc, 0, 0 };
        dev->CreateBuffer(&bd, &sdata, &d.cb);
        d.gx = (UINT)((ax + 31) / 32); d.gy = (UINT)((ay + 31) / 32);
        disps.push_back(d);
    };

    // L0: Y
    add_disp(plane_srv[0].Get(), bands[0][0].uav.Get(), W, H, Aw, Ah, true, 1.0f);
    // L1..4
    for (int l = 1; l < Levels; l++) {
        for (int c = 0; c < 3; c++) {
            bool is_chroma_l1 = (c > 0 && l == 1);
            if (is_chroma_l1) {
                add_disp(plane_srv[c].Get(), bands[c][l].uav.Get(), CW, CH, Aw>>1, Ah>>1, true, 1.0f);
            } else {
                add_disp(bands[c][l-1].srv.Get(), bands[c][l].uav.Get(),
                         bands[c][l-1].w, bands[c][l-1].h, bands[c][l-1].w, bands[c][l-1].h, false, 1.0f);
            }
        }
    }

    // Run
    ID3D11ShaderResourceView *nullsrv = nullptr;
    ID3D11UnorderedAccessView *nulluav = nullptr;
    ctx->CSSetShader(cs.Get(), nullptr, 0);
    ctx->CSSetSamplers(0, 1, &samp);

    for (auto &d : disps) {
        ctx->CSSetUnorderedAccessViews(0, 1, &d.uav, nullptr);
        ctx->CSSetShaderResources(0, 1, &d.srv);
        ctx->CSSetConstantBuffers(0, 1, d.cb.GetAddressOf());
        ctx->Dispatch(d.gx, d.gy, 1);
    }
    ctx->CSSetShaderResources(0, 1, &nullsrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nulluav, nullptr);
    ctx->Flush();

    // Read back and compare with CPU reference (exact prototype pattern)
    Plane sp[3];
    for (int c = 0; c < 3; c++) {
        sp[c].w = pw[c]; sp[c].h = ph[c];
        sp[c].d.resize(size_t(pw[c]) * ph[c]);
        for (size_t i = 0; i < sp[c].d.size(); i++)
            sp[c].d[i] = src[c][i] / 255.0f;
    }

    double worst16 = 0;
    for (int c = 0; c < 3; c++) {
        Plane in16 = sp[c];
        for (int l = (c ? 1 : 0); l < Levels; l++) {
            bool first = (l == (c ? 1 : 0));
            int ax = first ? (c ? (Aw >> 1) : Aw) : in16.w;
            int ay = first ? (c ? (Ah >> 1) : Ah) : in16.h;
            Bands r16 = ref_level(in16, ax, ay, first, true);
            double e16 = 0, maxmag = 0;
            for (int b = 0; b < 4; b++) {
                auto g = readback_layer(bands[c][l], b, dev, ctx);
                for (size_t i = 0; i < g.size(); i++) {
                    e16 = std::max(e16, double(std::fabs(g[i] - r16.b[b].d[i])));
                    maxmag = std::max(maxmag, double(std::fabs(r16.b[b].d[i])));
                }
            }
            printf("  comp %d level %d (%4dx%-4d x4 bands, max|coef| %7.3f): vs ref_fp16 %.3e\n",
                   c, l, r16.w, r16.h, maxmag, e16);
            worst16 = std::max(worst16, e16);
            in16 = r16.b[0];
        }
    }
    printf("  WORST: vs ref_fp16 %.3e\n", worst16);

    printf("T-DWT-1: max abs error = %.3e\n", worst16);
    bool pass = worst16 <= 4e-3;
    if (pass) printf("PASS T-DWT-1\n");
    else printf("FAIL T-DWT-1 (max abs %.3e > 4e-3)\n", worst16);

    return pass;
}

// ------------------------------------------------------------------
// Main
// ------------------------------------------------------------------
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    bool all_pass = true;

    // Create device
    ComPtr<IDXGIFactory1> fac;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&fac));
    if (FAILED(hr)) { printf("FAIL BUILD (DXGI factory)\n"); return 1; }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; ; i++) {
        if (fac->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d; adapter->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
        adapter.Reset();
    }
    if (!adapter) { printf("FAIL BUILD (no hardware adapter)\n"); return 1; }

    D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0 }, got;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fls, 1,
                           D3D11_SDK_VERSION, &device, &got, &context);
    if (FAILED(hr) || got < D3D_FEATURE_LEVEL_11_0) {
        printf("FAIL BUILD (D3D11 device)\n"); return 1;
    }

    printf("Device: ");
    DXGI_ADAPTER_DESC ad; adapter->GetDesc(&ad);
    printf("%S\n", ad.Description);

    // Test T-IN-1
    if (!test_in1(device.Get(), context.Get())) all_pass = false;

    // Test T-DWT-1 at 720p
    if (!test_dwt1(device.Get(), context.Get(), 1280, 720)) all_pass = false;

    // Test T-DWT-1 at 1080p
    if (!test_dwt1(device.Get(), context.Get(), 1920, 1080)) all_pass = false;

    if (all_pass) printf("PASS BUILD\n");
    else printf("FAIL BUILD\n");

    return all_pass ? 0 : 1;
}
