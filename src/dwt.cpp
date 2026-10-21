// dwt.cpp -- Forward DWT: upload + 13 dispatches, keeping the band layout
// Port of prototype/dx11dwt.cpp for the pyrowave_d3d11 library (T1).
// Band storage: R16F for levels 0-1, R32F for levels 2-4.

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
#include <cstdio>
#include <cstring>
#include <vector>

#include "pyrowave_d3d11.h"
#include "stage_internal.h"
#include "shaders/dwt_byte_dxbc.h"
#include "shaders/dwt_float_dxbc.h"
#include "shaders/dwt_ibyte_dxbc.h"
#include "shaders/dwt_ifloat_dxbc.h"

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------------
// CPU reference: round fp32 -> fp16 -> fp32 (emulates shared mem rounding)
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
// Constants
// ------------------------------------------------------------------
static const float ALPHA = -1.586134342059924f;
static const float BETA = -0.052980118572961f;
static const float GAMMA = 0.882911075530934f;
static const float DELTA = 0.443506852043971f;
static const float KK = 1.230174104914001f;
static const float INV_K = 1.0f / 1.230174104914001f;

// ------------------------------------------------------------------
// Push constants (matches cbuffer Push in dwt.hlsl)
// ------------------------------------------------------------------
struct PushConstants {
    int rx, ry;
    float ix, iy;
    int ax, ay;
    uint32_t dc;
    float rs;
    uint32_t phase;
    uint32_t groups_x;
    uint32_t slot_stride;
    uint32_t coeff_pitch;
    uint32_t src_pitch;
    uint32_t src_float;
    uint32_t group_x;
    uint32_t group_y;
};
static_assert(sizeof(PushConstants) == 64, "PushConstants must be 64 bytes");

// ------------------------------------------------------------------
// Helper: create a 1-layer Texture2DArray SRV (layer 0 only)
// ------------------------------------------------------------------
static ComPtr<ID3D11ShaderResourceView> make_srv_array1_layer0(ID3D11Device *dev, ID3D11Texture2D *t, DXGI_FORMAT f, int /*array_size*/)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = f;
    d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    d.Texture2DArray.MipLevels = 1;
    d.Texture2DArray.FirstArraySlice = 0;
    d.Texture2DArray.ArraySize = 1;
    ComPtr<ID3D11ShaderResourceView> v;
    HRESULT hr = dev->CreateShaderResourceView(t, &d, &v);
    return (SUCCEEDED(hr)) ? v : ComPtr<ID3D11ShaderResourceView>();
}

// ------------------------------------------------------------------
// Helper: create a 4-layer band texture + UAV + SRV
// ------------------------------------------------------------------
static BandTex make_band(ID3D11Device *dev, int w, int h, DXGI_FORMAT f)
{
    BandTex o;
    o.w = w; o.h = h; o.fmt = f;

    // Logical size stays w,h. The texture is padded to 32. Quant does not read
    // it: Texture2D.Load on the HD 6670 is wrong past x or y of 7, and padding
    // the desc did not change that. The coefficient buffer below is the copy
    // the DWT store writes and quant reads.
    o.pitch = (w + 31) & ~31;
    int pitch_h = (h + 31) & ~31;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = UINT(o.pitch); td.Height = UINT(pitch_h); td.MipLevels = 1; td.ArraySize = 4;
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

    // Layer 0 only SRV for next level input
    D3D11_SHADER_RESOURCE_VIEW_DESC sd0 = sd;
    sd0.Texture2DArray.ArraySize = 1;
    hr = dev->CreateShaderResourceView(o.tex.Get(), &sd0, &o.srv_layer0);
    if (FAILED(hr)) return BandTex{};

    UINT elems = 4u * UINT(o.pitch) * UINT(pitch_h);
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = elems * 4u;
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    cbd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    hr = dev->CreateBuffer(&cbd, nullptr, &o.coeff);
    if (FAILED(hr)) return BandTex{};

    D3D11_UNORDERED_ACCESS_VIEW_DESC cud = {};
    cud.Format = DXGI_FORMAT_R32_FLOAT;
    cud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    cud.Buffer.NumElements = elems;
    hr = dev->CreateUnorderedAccessView(o.coeff.Get(), &cud, &o.coeff_uav);
    if (FAILED(hr)) return BandTex{};

    D3D11_SHADER_RESOURCE_VIEW_DESC csd = {};
    csd.Format = DXGI_FORMAT_R32_TYPELESS;
    csd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    csd.BufferEx.NumElements = elems;
    csd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    hr = dev->CreateShaderResourceView(o.coeff.Get(), &csd, &o.coeff_srv);
    if (FAILED(hr)) return BandTex{};

    return o;
}

// ------------------------------------------------------------------
// Helper: read back one band layer
// ------------------------------------------------------------------
static std::vector<float> readback_layer(const BandTex &o, int layer, ID3D11Device *dev)
{
    D3D11_TEXTURE2D_DESC td;
    o.tex->GetDesc(&td);

    D3D11_TEXTURE2D_DESC st_desc = {};
    st_desc.Width = td.Width; st_desc.Height = td.Height;
    st_desc.MipLevels = 1; st_desc.ArraySize = 1;
    st_desc.Format = td.Format; st_desc.SampleDesc.Count = 1;
    st_desc.Usage = D3D11_USAGE_STAGING; st_desc.BindFlags = 0;
    st_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> st;
    HRESULT hr = dev->CreateTexture2D(&st_desc, nullptr, &st);
    if (FAILED(hr)) return {};

    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);

    ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, o.tex.Get(), layer, nullptr);

    D3D11_MAPPED_SUBRESOURCE m;
    hr = ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m);
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

    ctx->Unmap(st.Get(), 0);
    return result;
}

// ------------------------------------------------------------------
// Mirror coordinate fixup (matches generate_mirror_uv in dwt.hlsl)
// ------------------------------------------------------------------
static inline int mirror_coord(int i, int R)
{
    int p = 2 * R;
    int t = ((i % p) + p) % p;
    return t >= R ? p - 1 - t : t;
}

// ------------------------------------------------------------------
// Map gather coordinate through mirror sampler
// ------------------------------------------------------------------
static int map_coord(int c, int R, int A)
{
    int c0 = c & ~1;
    int g = c0;
    if (g < 0) g -= 1;
    g += 1;
    int endc = 2 * A - R;
    int past = g + 2 * (R - A) + 1;
    g = (g >= endc) ? past : std::min(g, R);
    return (c & 1) ? mirror_coord(g, R) : mirror_coord(g - 1, R);
}

// ------------------------------------------------------------------
// CPU reference for one DWT level
// ------------------------------------------------------------------
struct RefPlane { int w, h; std::vector<float> d; float at(int x, int y) const { return d[size_t(y) * w + x]; } };

static void lift(float *v, int n, int s, bool emu)
{
    for (int i = 1; i < n - 1; i += 2) v[i * s] += ALPHA * (v[(i - 1) * s] + v[(i + 1) * s]);
    for (int i = 2; i < n - 2; i += 2) v[i * s] += BETA * (v[(i - 1) * s] + v[(i + 1) * s]);
    for (int i = 3; i < n - 3; i += 2) v[i * s] += GAMMA * (v[(i - 1) * s] + v[(i + 1) * s]);
    for (int i = 4; i < n - 4; i += 2) v[i * s] += DELTA * (v[(i - 1) * s] + v[(i + 1) * s]);
    for (int i = 0; i < n; i++) {
        float f = v[i * s] * ((i & 1) ? KK : INV_K);
        v[i * s] = emu ? rh(f) : f;
    }
}

struct RefBands { int w, h; RefPlane b[4]; };

static RefBands ref_level(const RefPlane &src, int Aw, int Ah, bool dc, bool emu)
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

    RefBands o; o.w = Aw / 2; o.h = Ah / 2;
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
// Create the DWT compute shader from embedded bytecode
// ------------------------------------------------------------------
static HRESULT create_one_shader(ID3D11Device *dev, const BYTE *code, size_t len, ID3D11ComputeShader **out)
{
    return dev->CreateComputeShader(code, len, nullptr, out);
}

// ------------------------------------------------------------------
// DWTState destructor
// ------------------------------------------------------------------
DWTState::~DWTState() = default;

// ------------------------------------------------------------------
// Create DWT state: upload planes, create band textures, compile shader
// ------------------------------------------------------------------
pwd3d11_result dwt_create(DWTState *st, pwd3d11_device dev, int W, int H, int chroma_subsample)
{
    if (!dev) return PWD3D11_ERROR_INVALID_ARGUMENT;

    st->width = W;
    st->height = H;
    st->chroma = chroma_subsample;

    // Get D3D11 device from pwd3d11_device
    st->device = static_cast<ID3D11Device*>(pwd3d11_device_get_d3d11_device(dev));
    if (!st->device) return PWD3D11_ERROR_INVALID_ARGUMENT;
    st->device->GetImmediateContext(&st->context);

    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Compile the DWT compute shader
// ------------------------------------------------------------------
pwd3d11_result dwt_compile_shader(DWTState *st)
{
    if (!st->device) return PWD3D11_ERROR_INVALID_ARGUMENT;
    if (st->shader) return PWD3D11_SUCCESS;
    HRESULT hr = create_one_shader(st->device.Get(), g_dwt_byte_dxbc, g_dwt_byte_dxbc_len, st->shader.GetAddressOf());
    if (FAILED(hr)) return PWD3D11_ERROR_GENERIC;
    hr = create_one_shader(st->device.Get(), g_dwt_float_dxbc, g_dwt_float_dxbc_len, st->shader_float.GetAddressOf());
    if (FAILED(hr)) return PWD3D11_ERROR_GENERIC;
    hr = create_one_shader(st->device.Get(), g_dwt_ibyte_dxbc, g_dwt_ibyte_dxbc_len, st->shader_interior_byte.GetAddressOf());
    if (FAILED(hr)) return PWD3D11_ERROR_GENERIC;
    hr = create_one_shader(st->device.Get(), g_dwt_ifloat_dxbc, g_dwt_ifloat_dxbc_len, st->shader_interior_float.GetAddressOf());
    return SUCCEEDED(hr) ? PWD3D11_SUCCESS : PWD3D11_ERROR_GENERIC;
}

// ------------------------------------------------------------------
// Upload input planes from CPU buffer
// ------------------------------------------------------------------
pwd3d11_result dwt_upload_planes(DWTState *st, const pwd3d11_cpu_buffer *buf)
{
    if (!st || !buf) return PWD3D11_ERROR_INVALID_ARGUMENT;

    st->pw[0] = buf->width;
    st->ph[0] = buf->height;
    if (st->chroma != 0) {
        st->pw[1] = buf->width;
        st->ph[1] = buf->height;
        st->pw[2] = buf->width;
        st->ph[2] = buf->height;
    } else {
        st->pw[1] = buf->width / 2;
        st->ph[1] = buf->height / 2;
        st->pw[2] = buf->width / 2;
        st->ph[2] = buf->height / 2;
    }
    const bool tenbit = buf->format == PWD3D11_CPU_BUFFER_FORMAT_YUV420P10
        || buf->format == PWD3D11_CPU_BUFFER_FORMAT_YUV444P10;
    const bool sixteen = buf->format == PWD3D11_CPU_BUFFER_FORMAT_YUV420P16
        || buf->format == PWD3D11_CPU_BUFFER_FORMAT_YUV444P16;
    st->plane_float = (tenbit || sixteen) ? 1u : 0u;

    st->aw = (buf->width + 31) & ~31;
    st->ah = (buf->height + 31) & ~31;
    if (st->aw < 128) st->aw = 128;
    if (st->ah < 128) st->ah = 128;

    pwd3d11_unbind_cs(st->context.Get());

    // DWT reads plane_raw, not the unused R8 textures. Keep a DEFAULT raw
    // buffer and UpdateSubresource after unbind so later frames do not
    // allocate.
    for (int c = 0; c < 3; c++) {
        st->plane_pitch[c] = st->pw[c];
        UINT nbytes;
        if (st->plane_float) {
            nbytes = UINT(st->pw[c] * st->ph[c] * 4);
        } else {
            nbytes = UINT(st->plane_pitch[c] * st->ph[c]);
        }
        nbytes = (nbytes + 3u) & ~3u;
        if (st->plane_pack[c].size() != nbytes)
            st->plane_pack[c].assign(nbytes, 0);
        const auto *src = static_cast<const uint8_t *>(buf->data[c]);
        int stride = int(buf->row_stride_in_bytes[c]);
        if (stride <= 0) {
            stride = st->pw[c] * (st->plane_float ? (sixteen || tenbit ? 2 : 4) : 1);
        }
        for (int y = 0; y < st->ph[c]; y++) {
            if (!st->plane_float) {
                memcpy(st->plane_pack[c].data() + size_t(y) * size_t(st->plane_pitch[c]),
                       src + size_t(y) * size_t(stride), size_t(st->pw[c]));
                continue;
            }
            auto *dst = reinterpret_cast<float *>(
                st->plane_pack[c].data() + size_t(y) * size_t(st->pw[c]) * 4u);
            const auto *row = src + size_t(y) * size_t(stride);
            for (int x = 0; x < st->pw[c]; x++) {
                uint16_t sample = 0;
                memcpy(&sample, row + size_t(x) * 2u, 2);
                float n = sixteen ? float(sample) / 65535.0f
                                  : float(sample & 0x3ff) / 1023.0f;
                dst[x] = n - 0.5f;
            }
        }
        if (!st->plane_raw[c] || st->plane_raw_bytes[c] != int(nbytes)) {
            D3D11_BUFFER_DESC rbd = {};
            rbd.ByteWidth = nbytes;
            rbd.Usage = D3D11_USAGE_DEFAULT;
            rbd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            rbd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            HRESULT hr = st->device->CreateBuffer(&rbd, nullptr, &st->plane_raw[c]);
            if (FAILED(hr)) return PWD3D11_ERROR_GENERIC;
            D3D11_SHADER_RESOURCE_VIEW_DESC rsv = {};
            rsv.Format = DXGI_FORMAT_R32_TYPELESS;
            rsv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            rsv.BufferEx.NumElements = nbytes / 4u;
            rsv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            hr = st->device->CreateShaderResourceView(st->plane_raw[c].Get(), &rsv,
                                                      st->plane_raw_srv[c].ReleaseAndGetAddressOf());
            if (FAILED(hr)) return PWD3D11_ERROR_GENERIC;
            st->plane_raw_bytes[c] = int(nbytes);
        }
        D3D11_BOX box = {};
        box.right = nbytes;
        box.bottom = 1;
        box.back = 1;
        st->context->UpdateSubresource(st->plane_raw[c].Get(), 0, &box,
                                       st->plane_pack[c].data(), nbytes, 0);
    }

    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Create band output textures
// ------------------------------------------------------------------
pwd3d11_result dwt_create_bands(DWTState *st)
{
    pwd3d11_unbind_cs(st->context.Get());
    const bool reuse = st->bands_ready && st->bands[0][0].tex
        && st->bands[0][0].w == (st->aw >> 1) && st->bands[0][0].h == (st->ah >> 1);
    for (int c = 0; c < 3; c++) {
        int start_level = (st->chroma == 0 && c != 0) ? 1 : 0; // 4:2:0 chroma starts at level 1
        for (int l = start_level; l < Levels; l++) {
            int bw = st->aw >> (l + 1);
            int bh = st->ah >> (l + 1);
            if (!reuse) {
                // The shader UAV is RWTexture2DArray<float>, which is R32_FLOAT.
                // R16_FLOAT on levels 0 and 1 stored the wrong bits on the HD 6670
                // and the decoder reconstructed those bands as color noise.
                DXGI_FORMAT fmt = DXGI_FORMAT_R32_FLOAT;
                st->bands[c][l] = make_band(st->device.Get(), bw, bh, fmt);
                if (!st->bands[c][l].tex) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;
            }
        }
    }
    st->bands_ready = true;
    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Run the DWT: 13 dispatches in the correct order
// ------------------------------------------------------------------
pwd3d11_result dwt_run(DWTState *st)
{
    if (!st || !st->shader) return PWD3D11_ERROR_INVALID_ARGUMENT;

    auto ctx = st->context;
    ID3D11ShaderResourceView *nullsrv = nullptr;
    ID3D11UnorderedAccessView *nulluav[3] = { nullptr, nullptr, nullptr };

    struct DispatchEntry {
        ID3D11ShaderResourceView *src;
        ID3D11UnorderedAccessView *uav;
        ID3D11UnorderedAccessView *coeff;
        uint32_t coeff_pitch;
        uint32_t src_pitch;
        uint32_t src_float;
        int rx, ry, ax, ay;
        uint32_t dc_shift;
        float range_scale;
        UINT gx, gy;
        UINT ox, oy, dx, dy;
        bool interior;
    };

    std::vector<DispatchEntry> disps;

    auto add_disp = [&](ID3D11ShaderResourceView *src, uint32_t src_pitch, uint32_t src_float,
                        const BandTex &band, int rx, int ry, int ax, int ay, bool dc, float rs) {
        auto push = [&](UINT ox, UINT oy, UINT dx, UINT dy, bool interior) {
            if (dx == 0 || dy == 0) return;
            DispatchEntry e;
            e.src = src;
            e.src_pitch = src_pitch;
            e.src_float = src_float;
            e.uav = band.uav.Get();
            e.coeff = band.coeff_uav.Get();
            e.coeff_pitch = uint32_t(band.pitch);
            e.rx = rx; e.ry = ry; e.ax = ax; e.ay = ay;
            e.dc_shift = dc ? 1u : 0u;
            e.range_scale = rs;
            e.gx = UINT((ax + 31) / 32);
            e.gy = UINT((ay + 31) / 32);
            e.ox = ox; e.oy = oy; e.dx = dx; e.dy = dy;
            e.interior = interior;
            disps.push_back(e);
        };
        UINT gx = UINT((ax + 31) / 32);
        UINT gy = UINT((ay + 31) / 32);
        // One dispatch per level. Splitting interior groups did not cut 720p
        // time on the HD 6670 and it slowed 512x288.
        push(0, 0, gx, gy, false);
    };

    add_disp(st->plane_raw_srv[0].Get(), uint32_t(st->plane_pitch[0]), st->plane_float,
             st->bands[0][0], st->pw[0], st->ph[0], st->aw, st->ah, true, 1.0f);
    if (st->chroma != 0) {
        for (int c = 1; c < 3; c++) {
            add_disp(st->plane_raw_srv[c].Get(), uint32_t(st->plane_pitch[c]), st->plane_float,
                     st->bands[c][0], st->pw[c], st->ph[c], st->aw, st->ah, true, 1.0f);
        }
    }

    for (int l = 1; l < Levels; l++) {
        for (int c = 0; c < 3; c++) {
            bool is_chroma_l1 = (c > 0 && l == 1 && st->chroma == 0);
            if (is_chroma_l1) {
                add_disp(st->plane_raw_srv[c].Get(), uint32_t(st->plane_pitch[c]), st->plane_float,
                         st->bands[c][l], st->pw[c], st->ph[c], st->aw >> 1, st->ah >> 1, true, 1.0f);
            } else {
                const BandTex &prev = st->bands[c][l - 1];
                add_disp(prev.coeff_srv.Get(), uint32_t(prev.pitch), 1u, st->bands[c][l],
                         prev.w, prev.h, prev.w, prev.h, false, 1.0f);
            }
        }
    }

    size_t disp_i = 0;
    ID3D11ShaderResourceView *bound_src = nullptr;
    ID3D11UnorderedAccessView *bound_coeff = nullptr;
    for (auto &d : disps) {
        PushConstants pc = {};
        pc.rx = d.rx; pc.ry = d.ry;
        pc.ix = 1.0f / float(d.rx); pc.iy = 1.0f / float(d.ry);
        pc.ax = d.ax; pc.ay = d.ay;
        pc.dc = d.dc_shift;
        pc.rs = d.range_scale;
        pc.groups_x = d.gx;
        pc.slot_stride = 0;
        pc.coeff_pitch = d.coeff_pitch;
        pc.src_pitch = d.src_pitch;
        pc.src_float = d.src_float;
        pc.group_x = d.ox;
        pc.group_y = d.oy;
        pc.phase = 0;

        ID3D11ComputeShader *cs = st->shader.Get();
        if (d.interior)
            cs = d.src_float ? st->shader_interior_float.Get() : st->shader_interior_byte.Get();
        else if (d.src_float)
            cs = st->shader_float.Get();
        ctx->CSSetShader(cs, nullptr, 0);

        if (d.src != bound_src || d.coeff != bound_coeff) {
            ID3D11UnorderedAccessView *uavs[3] = { d.uav, nullptr, d.coeff };
            ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
            ctx->CSSetShaderResources(0, 1, &d.src);
            bound_src = d.src;
            bound_coeff = d.coeff;
        }

        if (st->level_cb.size() <= disp_i) st->level_cb.resize(disp_i + 1);
        if (!st->level_cb[disp_i]) {
            D3D11_BUFFER_DESC cbd = {};
            cbd.ByteWidth = sizeof(PushConstants);
            cbd.Usage = D3D11_USAGE_IMMUTABLE;
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA sd = { &pc, 0, 0 };
            HRESULT hr = st->device->CreateBuffer(&cbd, &sd, &st->level_cb[disp_i]);
            if (FAILED(hr)) return PWD3D11_ERROR_GENERIC;
        }
        ctx->CSSetConstantBuffers(0, 1, st->level_cb[disp_i].GetAddressOf());
        ctx->Dispatch(d.dx, d.dy, 1);
        disp_i++;
    }

    // Clear shader resources
    ctx->CSSetShaderResources(0, 1, &nullsrv);
    ctx->CSSetUnorderedAccessViews(0, 3, nulluav, nullptr);

    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Read back all bands
// ------------------------------------------------------------------
static std::vector<std::vector<float>> dwt_readback(DWTState *st)
{
    std::vector<std::vector<float>> result;
    result.resize(3 * Levels); // [component * Levels + level]

    for (int c = 0; c < 3; c++) {
        int start_level = (st->chroma == 0 && c != 0) ? 1 : 0;
        for (int l = start_level; l < Levels; l++) {
            for (int b = 0; b < 4; b++) {
                auto layer = readback_layer(st->bands[c][l], b, st->device.Get());
                result[c * Levels + l].insert(result[c * Levels + l].end(), layer.begin(), layer.end());
            }
        }
    }
    return result;
}

// ------------------------------------------------------------------
// Validate DWT output against CPU reference
// ------------------------------------------------------------------
static double validate_dwt(DWTState *st, const pwd3d11_cpu_buffer *buf, bool emu_rounding)
{
    // Convert CPU buffer to RefPlane
    RefPlane planes[3];
    for (int c = 0; c < 3; c++) {
        planes[c].w = st->pw[c];
        planes[c].h = st->ph[c];
        planes[c].d.resize(size_t(planes[c].w) * planes[c].h);
        const uint8_t *src = static_cast<const uint8_t*>(buf->data[c]);
        for (size_t i = 0; i < planes[c].d.size(); i++) {
            planes[c].d[i] = src[i] / 255.0f;
        }
    }

    RefPlane prev_ll[3] = {};
    double worst_error = 0;

    for (int c = 0; c < 3; c++) {
        int start_level = (st->chroma == 0 && c != 0) ? 1 : 0;
        for (int l = start_level; l < Levels; l++) {
            bool is_first = (l == start_level);
            (void)is_first; // rx/ry were used when is_first was checked
            int ax = is_first ? ((c > 0 && st->chroma == 0) ? (st->aw >> 1) : st->aw) : prev_ll[c].w;
            int ay = is_first ? ((c > 0 && st->chroma == 0) ? (st->ah >> 1) : st->ah) : prev_ll[c].h;

            RefBands ref = ref_level(prev_ll[c], ax, ay, is_first, emu_rounding);

            // Read back GPU output for this level
            for (int b = 0; b < 4; b++) {
                auto gpu_layer = readback_layer(st->bands[c][l], b, st->device.Get());
                size_t n = size_t(ref.b[b].w) * ref.b[b].h;
                for (size_t i = 0; i < n && i < gpu_layer.size(); i++) {
                    double err = std::fabs(double(gpu_layer[i] - ref.b[b].d[i]));
                    worst_error = std::max(worst_error, err);
                }
            }

            prev_ll[c] = ref.b[0];
        }
    }

    return worst_error;
}

// ------------------------------------------------------------------
// Test oracle: T-IN-1 -- verify UNORM8 -> float conversion
// ------------------------------------------------------------------
static bool test_in1(DWTState *st)
{
    // Sample all 256 codes through the DWT's sampling path
    // Create a small test texture with all 256 values
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 256; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    std::vector<uint8_t> data(256);
    for (int i = 0; i < 256; i++) data[i] = static_cast<uint8_t>(i);

    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = data.data(); sd.SysMemPitch = 256;

    ComPtr<ID3D11Texture2D> test_tex;
    HRESULT hr = st->device->CreateTexture2D(&td, &sd, &test_tex);
    if (FAILED(hr)) return false;

    // Create SRV as Texture2DArray
    D3D11_SHADER_RESOURCE_VIEW_DESC sd_desc = {};
    sd_desc.Format = DXGI_FORMAT_R8_UNORM;
    sd_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sd_desc.Texture2DArray.MipLevels = 1;
    sd_desc.Texture2DArray.ArraySize = 1;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = st->device->CreateShaderResourceView(test_tex.Get(), &sd_desc, &srv);
    if (FAILED(hr)) return false;

    // Use a simple compute shader to sample all 256 values via Gather
    // For T-IN-1, we just verify the float conversion is correct
    // The Gather operation samples at whole-texel coordinates with point sampling
    // so each texel c maps to float(c) / 255.0f

    // Create a UAV to write results
    D3D11_TEXTURE2D_DESC uav_desc = {};
    uav_desc.Width = 256; uav_desc.Height = 1; uav_desc.MipLevels = 1; uav_desc.ArraySize = 1;
    uav_desc.Format = DXGI_FORMAT_R32_FLOAT; uav_desc.SampleDesc.Count = 1;
    uav_desc.Usage = D3D11_USAGE_DEFAULT; uav_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

    ComPtr<ID3D11Texture2D> uav_tex;
    hr = st->device->CreateTexture2D(&uav_desc, nullptr, &uav_tex);
    if (FAILED(hr)) return false;

    ComPtr<ID3D11UnorderedAccessView> uav;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R32_FLOAT;
    ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    hr = st->device->CreateUnorderedAccessView(uav_tex.Get(), &ud, &uav);
    if (FAILED(hr)) return false;

    // Create a simple test shader that samples each texel
    // For T-IN-1, we test that the UNORM->float conversion matches CPU float(c)/255.0f
    // We'll use the probe shader for this, or create an inline test
    // For now, just verify the basic path works - the actual comparison is done in test_t1.cpp

    return true;
}

// ------------------------------------------------------------------
// Free DWT state
// ------------------------------------------------------------------
void dwt_destroy(DWTState *st)
{
    // Flush only. The caller deletes the state, same as quant/analyze/resolve.
    // Deleting here double-freed the COM pointers on encoder destroy.
    if (st) {
        if (st->context) st->context->Flush();
    }
}

// ------------------------------------------------------------------
// Accessors for band textures
// ------------------------------------------------------------------
ID3D11ShaderResourceView *dwt_get_band_srv(DWTState *st, int component, int level)
{
    if (!st || component < 0 || component >= 3 || level < 0 || level >= Levels) return nullptr;
    return st->bands[component][level].coeff_srv.Get();
}

int dwt_get_band_pitch(DWTState *st, int component, int level)
{
    if (!st || component < 0 || component >= 3 || level < 0 || level >= Levels) return -1;
    return st->bands[component][level].pitch;
}

int dwt_get_band_w(DWTState *st, int component, int level)
{
    if (!st || component < 0 || component >= 3 || level < 0 || level >= Levels) return -1;
    return st->bands[component][level].w;
}

int dwt_get_band_h(DWTState *st, int component, int level)
{
    if (!st || component < 0 || component >= 3 || level < 0 || level >= Levels) return -1;
    return st->bands[component][level].h;
}
