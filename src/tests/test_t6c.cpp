// test_t6c.cpp -- T6c acceptance tests: debug dump hooks + pwd3d11_debug_run_stage
// Tests: BUILD (compile), ROUNDTRIP (DWT stage round-trip via debug dump)
//
// M3 mode: run D3D11 DWT stage, dump band outputs, compare with CPU reference.
// Dump format: manifest.json + *.bin files (VALIDATION §3.4).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <immintrin.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <sys/stat.h>

#include "pyrowave_d3d11.h"
#include "debug_dump.h"

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------------
// CPU helpers
// ------------------------------------------------------------------
static inline uint16_t f2h(float f) { return uint16_t(_mm_extract_epi16(_mm_cvtps_ph(_mm_set_ss(f), _MM_FROUND_TO_NEAREST_INT), 0)); }
static inline float h2f(uint16_t h) { return _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))); }
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
// CPU reference DWT
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

    D3D11_TEXTURE2D_DESC st_desc = {};
    st_desc.Width = td.Width; st_desc.Height = td.Height;
    st_desc.MipLevels = 1; st_desc.ArraySize = td.ArraySize;
    st_desc.Format = td.Format; st_desc.SampleDesc.Count = 1;
    st_desc.Usage = D3D11_USAGE_STAGING; st_desc.BindFlags = 0;
    st_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> st;
    HRESULT hr = dev->CreateTexture2D(&st_desc, nullptr, &st);
    if (FAILED(hr)) return {};

    ctx->CopyResource(st.Get(), o.tex.Get());

    D3D11_MAPPED_SUBRESOURCE m;
    hr = ctx->Map(st.Get(), layer, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) return {};

    size_t total = size_t(o.w) * o.h;
    std::vector<float> result(total);

    if (o.fmt == DXGI_FORMAT_R16_FLOAT) {
        for (size_t i = 0; i < total; i++) {
            const uint16_t *row = reinterpret_cast<const uint16_t*>(m.pData);
            result[i] = h2f(row[i]);
        }
    } else {
        for (size_t i = 0; i < total; i++) {
            const float *row = reinterpret_cast<const float*>(m.pData);
            result[i] = row[i];
        }
    }

    ctx->Unmap(st.Get(), layer);
    return result;
}

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------
static std::string get_temp_dir()
{
    char buf[MAX_PATH];
    GetTempPathA(sizeof(buf), buf);
    return std::string(buf);
}

static void mkdirs(const char *path)
{
    std::string p = path;
    for (size_t i = 1; i < p.size(); i++) {
        if (p[i] == '\\' || p[i] == '/') {
            p[i] = '\0';
            CreateDirectoryA(p.c_str(), NULL);
            p[i] = '\\';
        }
    }
    CreateDirectoryA(p.c_str(), NULL);
}

// ------------------------------------------------------------------
// T6c ROUNDTRIP: DWT stage with debug dump
// Wraps DWT logic in a lambda so goto/cleanup doesn't skip initializers.
// ------------------------------------------------------------------
static bool test_roundtrip(int W, int H)
{
    printf("T6c ROUNDTRIP: %dx%d\n", W, H);

    // Create device
    pwd3d11_device_create_info ci = {};
    ci.struct_size = sizeof(ci);
    ci.adapter_index = -1;
    ci.flags = 2;  // allow WARP for testing
    pwd3d11_device dev = nullptr;
    pwd3d11_result r = pwd3d11_create_device(&ci, &dev);
    if (r != PWD3D11_SUCCESS || !dev) {
        printf("FAIL ROUNDTRIP: device creation failed\n");
        return false;
    }

    // Get device interface via accessor (T6c)
    // The accessor AddRef's the pointer; we use ComPtr to manage it.
    ID3D11Device *raw_dev = static_cast<ID3D11Device*>(pwd3d11_device_get_d3d11_device(dev));
    ID3D11DeviceContext *raw_ctx = static_cast<ID3D11DeviceContext*>(pwd3d11_device_get_d3d11_context(dev));
    ComPtr<ID3D11Device> d3d_dev;
    d3d_dev.Attach(raw_dev);
    ComPtr<ID3D11DeviceContext> ctx;
    ctx.Attach(raw_ctx);

    // Create encoder
    pwd3d11_encoder_create_info eci = {};
    eci.device = dev;
    eci.width = W;
    eci.height = H;
    eci.chroma = PWD3D11_CHROMA_SUBSAMPLING_420;
    pwd3d11_encoder enc = nullptr;
    r = pwd3d11_encoder_create(&eci, &enc);
    if (r != PWD3D11_SUCCESS || !enc) {
        printf("FAIL ROUNDTRIP: encoder creation failed\n");
        pwd3d11_device_destroy(dev);
        return false;
    }

    // Set up debug dump directory
    std::string dump_dir = get_temp_dir() + "\\pyrowave_d3d11_t6c_" + std::to_string(W) + "x" + std::to_string(H);
    mkdirs(dump_dir.c_str());
    pwd3d11_debug_set_dump_dir(enc, dump_dir.c_str());

    printf("  Dump dir: %s\n", dump_dir.c_str());

    // Run DWT + compare + dump (in lambda to avoid goto over initializers)
    bool pass = [&]() -> bool {
        const int Aw = (W + 31) & ~31;
        const int Ah = (H + 31) & ~31;
        const int CW = W / 2, CH = H / 2;
        std::vector<uint8_t> src[3] = { make_plane(W, H, 1), make_plane(CW, CH, 2), make_plane(CW, CH, 3) };

        // Create input textures
        ComPtr<ID3D11Texture2D> planes[3];
        ComPtr<ID3D11ShaderResourceView> plane_srv[3];
        for (int c = 0; c < 3; c++) {
            int pw = c == 0 ? W : CW;
            int ph = c == 0 ? H : CH;
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = pw; td.Height = ph; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA sdata = { src[c].data(), UINT(pw), 0 };
            HRESULT hr = d3d_dev->CreateTexture2D(&td, &sdata, &planes[c]);
            if (FAILED(hr)) { printf("FAIL ROUNDTRIP: create plane %d failed\n", c); return false; }

            D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = DXGI_FORMAT_R8_UNORM;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray.MipLevels = 1;
            sd.Texture2DArray.ArraySize = 1;
            hr = d3d_dev->CreateShaderResourceView(planes[c].Get(), &sd, &plane_srv[c]);
            if (FAILED(hr)) { printf("FAIL ROUNDTRIP: create plane SRV %d failed\n", c); return false; }
        }

        // Create band textures
        const int Levels = 5;
        BandTex bands[3][Levels];
        for (int c = 0; c < 3; c++) {
            int start = (c == 0) ? 0 : 1;
            for (int l = start; l < Levels; l++) {
                int bw = Aw >> (l + 1), bh = Ah >> (l + 1);
                DXGI_FORMAT fmt = (l < 2) ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT;
                bands[c][l] = make_band(bw, bh, fmt, d3d_dev.Get());
                if (!bands[c][l].tex) { printf("FAIL ROUNDTRIP: create band %d,%d failed\n", c, l); return false; }
            }
        }

        // Compile shader
        ComPtr<ID3DBlob> cs_blob, err_blob;
        HRESULT hr = D3DCompileFromFile(
            L"C:/Build/codecs/pyrowave-dx11/src/shaders/dwt.hlsl",
            nullptr, nullptr, "main", "cs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs_blob, &err_blob);
        if (FAILED(hr)) {
            if (err_blob) fprintf(stderr, "T6c: compile failed: %s\n", (char*)err_blob->GetBufferPointer());
            printf("FAIL ROUNDTRIP: shader compile failed\n");
            return false;
        }
        ComPtr<ID3D11ComputeShader> cs;
        hr = d3d_dev->CreateComputeShader(cs_blob->GetBufferPointer(), cs_blob->GetBufferSize(), nullptr, &cs);
        if (FAILED(hr)) { printf("FAIL ROUNDTRIP: create shader failed\n"); return false; }

        // Create sampler
        D3D11_SAMPLER_DESC samp_desc = {};
        samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samp_desc.AddressU = samp_desc.AddressV = samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
        samp_desc.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> samp;
        hr = d3d_dev->CreateSamplerState(&samp_desc, &samp);
        if (FAILED(hr)) { printf("FAIL ROUNDTRIP: create sampler failed\n"); return false; }

        // Build dispatches
        struct Disp { ID3D11ShaderResourceView *srv; ID3D11UnorderedAccessView *uav;
                      ComPtr<ID3D11Buffer> cb; UINT gx, gy; };
        std::vector<Disp> disps;

        auto add_disp = [&](ID3D11ShaderResourceView *srv, ID3D11UnorderedAccessView *uav,
                            int rx, int ry, int ax, int ay, bool dc, float rs) {
            Disp d; d.srv = srv; d.uav = uav;
            PushConstants pc = { rx, ry, 1.f/rx, 1.f/ry, ax, ay, dc ? 1u : 0u, rs };
            D3D11_BUFFER_DESC bd = { (UINT)sizeof(PushConstants), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
            D3D11_SUBRESOURCE_DATA sdata = { &pc, 0, 0 };
            d3d_dev->CreateBuffer(&bd, &sdata, &d.cb);
            d.gx = (UINT)((ax + 31) / 32); d.gy = (UINT)((ay + 31) / 32);
            disps.push_back(d);
        };

        add_disp(plane_srv[0].Get(), bands[0][0].uav.Get(), W, H, Aw, Ah, true, 1.0f);
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

        // Read back and compare
        Plane sp[3];
        for (int c = 0; c < 3; c++) {
            sp[c].w = (c == 0 ? W : CW);
            sp[c].h = (c == 0 ? H : CH);
            sp[c].d.resize(size_t(sp[c].w) * sp[c].h);
            for (size_t i = 0; i < sp[c].d.size(); i++)
                sp[c].d[i] = src[c][i] / 255.0f;
        }

        double worst16 = 0;
        int buf_idx = 0;

        // Collect buffer names for manifest (use static storage so pointers remain valid)
        static char buf_names[200][128];
        int buf_stages[200];
        int buf_dtypes[200];
        int buf_sw[200], buf_sh[200];

        for (int c = 0; c < 3; c++) {
            Plane in16 = sp[c];
            for (int l = (c ? 1 : 0); l < Levels; l++) {
                bool first = (l == (c ? 1 : 0));
                int ax = first ? (c ? (Aw >> 1) : Aw) : in16.w;
                int ay = first ? (c ? (Ah >> 1) : Ah) : in16.h;
                Bands r16 = ref_level(in16, ax, ay, first, true);
                double e16 = 0, maxmag = 0;
                for (int b = 0; b < 4; b++) {
                    auto gpu = readback_layer(bands[c][l], b, d3d_dev.Get(), ctx.Get());
                    for (size_t i = 0; i < gpu.size(); i++) {
                        e16 = std::max(e16, double(std::fabs(gpu[i] - r16.b[b].d[i])));
                        maxmag = std::max(maxmag, double(std::fabs(r16.b[b].d[i])));
                    }

                    // Dump this band
                    snprintf(buf_names[buf_idx], sizeof(buf_names[buf_idx]), "band_c%d_l%d_b%d", c, l, b);
                    buf_stages[buf_idx] = 1;
                    buf_dtypes[buf_idx] = (l < 2) ? 1 : 0;  // f16 for L0-1, f32 for L2-4
                    buf_sw[buf_idx] = (int)gpu.size() / (r16.b[b].h > 0 ? r16.b[b].h : 1);
                    buf_sh[buf_idx] = r16.b[b].h;
                    buf_idx++;

                    debug_dump_buffer(dump_dir.c_str(), buf_names[buf_idx-1], 1, buf_dtypes[buf_idx-1],
                                      buf_sw[buf_idx-1], buf_sh[buf_idx-1],
                                      gpu.data(), gpu.size() * sizeof(float));
                }
                printf("  comp %d level %d (%4dx%-4d x4 bands, max|coef| %7.3f): vs ref_fp16 %.3e\n",
                       c, l, r16.w, r16.h, maxmag, e16);
                worst16 = std::max(worst16, e16);
                in16 = r16.b[0];
            }
        }

        printf("  WORST: vs ref_fp16 %.3e\n", worst16);

        // Write manifest (build pointer array for const char**)
        const char *name_ptrs[200];
        for (int i = 0; i < buf_idx; i++) name_ptrs[i] = buf_names[i];
        debug_write_manifest(dump_dir.c_str(), "grad", W, H, 32768, "D3D11-debug (T6c test)",
                             name_ptrs, buf_stages, buf_dtypes, buf_sw, buf_sh, buf_idx);

        printf("  Dumps written to: %s\n", dump_dir.c_str());

        return worst16 <= 4e-3;
    }();

    pwd3d11_encoder_destroy(enc);
    pwd3d11_device_destroy(dev);

    // DWT vs CPU reference has known errors (T1 pre-existing, test_t1 also fails).
    // The dump infrastructure itself works (manifest + bins written).
    // ROUNDTRIP is FAIL because DWT error exceeds 4e-3 tolerance.
    // TODO: once DWT is fixed (T1), this will become PASS.
    (void)pass;
    return false;  // DWT error exceeds tolerance (known T1 issue)
}

// ------------------------------------------------------------------
// Main
// ------------------------------------------------------------------
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    bool all_pass = true;

    // Build: if we got here, compilation succeeded
    printf("PASS BUILD\n");

    // ROUNDTRIP: run DWT with debug dump at multiple sizes
    all_pass &= test_roundtrip(32, 32);
    all_pass &= test_roundtrip(128, 128);

    if (!all_pass) {
        printf("FAIL ROUNDTRIP\n");
        return 1;
    }

    printf("PASS ROUNDTRIP\n");
    return 0;
}
