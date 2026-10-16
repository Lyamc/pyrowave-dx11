// quant.cpp -- Quantization stage: dispatches + resource management
// Port of R/pyrowave_encoder.cpp:436-483 to D3D11 host code (T2b).
// One dispatch per band (42 for 4:2:0), matching Vulkan behavior.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <vector>
#include <cstring>
#include <cmath>
#include <cstdio>

#include "pyrowave_d3d11.h"
#include "debug_dump.h"

#include "shaders/quant_dxbc.h"

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------------
// Push constants (matches cbuffer in quant.hlsl, 48 bytes)
// Layout: int2 @0, int2 @8, float2 @16, float @24, float @28,
//          int @32, int @36, float @40, pad to 48
// ------------------------------------------------------------------
struct QuantPushConstants {
    int   resolution_x;           // @0
    int   resolution_y;           // @4
    int   resolution_8x8_blocks_x;// @8
    int   resolution_8x8_blocks_y;// @12
    float inv_resolution_x;       // @16
    float inv_resolution_y;       // @20
    float input_layer;            // @24
    float quant_resolution;       // @28
    int   block_offset;           // @32
    int   block_stride;           // @36
    float rdo_distortion_scale;   // @40
    float _pad;                   // @44
};
static_assert(sizeof(QuantPushConstants) == 48, "QuantPushConstants must be 48 bytes");

// ------------------------------------------------------------------
// Quant encoder state
// ------------------------------------------------------------------
struct QuantState {
    ~QuantState();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;

    // Input: DWT band textures [component][level]
    // bands[c][l] is a Texture2DArray with 4 layers (LL, LH, HL, HH)
    struct BandRef {
        ComPtr<ID3D11ShaderResourceView> srv;
        int w = 0, h = 0;
        int level = 0, component = 0;
    };
    std::vector<BandRef> bands; // [component * Levels + level]

    // Band dimensions: [component][level]
    int pw[3], ph[3]; // physical (unpadded)
    int aw, ah;       // aligned
    int chroma;       // 0 = 4:2:0, 1 = 4:4:4

    // Output resources
    ComPtr<ID3D11UnorderedAccessView> uav_meta;    // u0: BlockMeta (8 bytes per block)
    ComPtr<ID3D11UnorderedAccessView> uav_stats;   // u1: BlockStats (64 bytes per block)
    ComPtr<ID3D11UnorderedAccessView> uav_payload; // u2: payload counter + bytes

    // Block counts for resource sizing
    int total_blocks = 0;

    // Payload buffer size: AW*AH*2 + padding for 4-byte alignment per group
    size_t payload_size = 0;
};

// ------------------------------------------------------------------
// Helper: get quant resolution for a band (R/pyrowave_encoder.cpp:148-176)
// ------------------------------------------------------------------
static float get_quant_resolution(int level, int component, int band, int chroma)
{
    // Simplified: returns a power of two <= 4096
    // Full implementation would match R/pyrowave_encoder.cpp:148-176
    // For now, use a reasonable default
    (void)component; (void)band; (void)chroma;
    switch (level) {
        case 0: return 4096.0f;
        case 1: return 2048.0f;
        case 2: return 1024.0f;
        case 3: return 512.0f;
        case 4: return 256.0f;
        default: return 4096.0f;
    }
}

// ------------------------------------------------------------------
// Helper: get RDO distortion scale from table (R/pyrowave_encoder.cpp:111-146)
// ------------------------------------------------------------------
static float get_rdo_distortion_scale(int level, int component, int band, int chroma)
{
    // 4:2:0: 42 entries (skip L0 chroma)
    // 4:4:4: 60 entries (5 levels x 3 comps x 4 bands)
    // Table is in tables_rdo.inc, but we can't include it from C++
    // For now, use a placeholder that matches the dumped table
    (void)level; (void)component; (void)band; (void)chroma;
    // This will be filled from the dumped table in the full implementation
    return 1.0f / 256.0f; // placeholder
}

// ------------------------------------------------------------------
// Create quant state
// ------------------------------------------------------------------
pwd3d11_result quant_create(QuantState *st, pwd3d11_device dev, int W, int H, int chroma_subsample)
{
    if (!st || !dev) return PWD3D11_ERROR_INVALID_ARGUMENT;

    // Get device and context from pwd3d11_device
    // Cast internal pwd3d11_device_t to get ComPtr<ID3D11Device>
    // We need to access through the public API
    // For now, assume st was initialized with device/context from DWTState
    (void)dev;

    st->pw[0] = W;
    st->ph[0] = H;
    st->pw[1] = W / 2;
    st->ph[1] = H / 2;
    st->pw[2] = W / 2;
    st->ph[2] = H / 2;

    st->aw = (W + 31) & ~31;
    st->ah = (H + 31) & ~31;
    if (st->aw < 128) st->aw = 128;
    if (st->ah < 128) st->ah = 128;
    st->chroma = chroma_subsample;

    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Set band textures (called after DWT completes)
// ------------------------------------------------------------------
pwd3d11_result quant_set_bands(QuantState *st, ID3D11ShaderResourceView * const *band_srvs,
                                const int *band_w, const int *band_h, int num_bands)
{
    if (!st || !band_srvs) return PWD3D11_ERROR_INVALID_ARGUMENT;

    st->bands.resize(num_bands);
    for (int i = 0; i < num_bands; i++) {
        st->bands[i].srv = band_srvs[i];
        st->bands[i].w = band_w[i];
        st->bands[i].h = band_h[i];
    }
    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Create output UAVs
// ------------------------------------------------------------------
pwd3d11_result quant_create_outputs(QuantState *st)
{
    if (!st->device) return PWD3D11_ERROR_INVALID_ARGUMENT;

    // Count total blocks
    int total_blocks = 0;
    for (auto &b : st->bands) {
        int bw = (b.w + 7) / 8;
        int bh = (b.h + 7) / 8;
        total_blocks += bw * bh;
    }
    st->total_blocks = total_blocks;

    // u0: BlockMeta (8 bytes per block)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = total_blocks * 8;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

        ComPtr<ID3D11Buffer> buf;
        HRESULT hr = st->device->CreateBuffer(&bd, nullptr, &buf);
        if (FAILED(hr)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = total_blocks * 2; // 2 uints per block

        hr = st->device->CreateUnorderedAccessView(buf.Get(), &ud, &st->uav_meta);
        if (FAILED(hr)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    // u1: BlockStats (64 bytes per block = 16 uints)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = total_blocks * 64;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

        ComPtr<ID3D11Buffer> buf;
        HRESULT hr = st->device->CreateBuffer(&bd, nullptr, &buf);
        if (FAILED(hr)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = total_blocks * 16;

        hr = st->device->CreateUnorderedAccessView(buf.Get(), &ud, &st->uav_stats);
        if (FAILED(hr)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    // u2: Payload buffer (AW*AH*2 + padding)
    st->payload_size = size_t(st->aw) * size_t(st->ah) * 2 + 4 * total_blocks + 8;

    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = UINT(st->payload_size);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

        ComPtr<ID3D11Buffer> buf;
        HRESULT hr = st->device->CreateBuffer(&bd, nullptr, &buf);
        if (FAILED(hr)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = UINT(st->payload_size / 4);

        hr = st->device->CreateUnorderedAccessView(buf.Get(), &ud, &st->uav_payload);
        if (FAILED(hr)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Compile quant shader
// ------------------------------------------------------------------
pwd3d11_result quant_compile_shader(QuantState *st)
{
    if (!st->device) return PWD3D11_ERROR_INVALID_ARGUMENT;

    // Try embedded bytecode first
    HRESULT hr = st->device->CreateComputeShader(
        const_cast<unsigned char*>(g_quant_dxbc),
        g_quant_dxbc_len,
        nullptr,
        &st->shader
    );

    if (SUCCEEDED(hr)) return PWD3D11_SUCCESS;

    // Fallback: compile from source (for testing)
    ComPtr<ID3DBlob> cs_blob, err_blob;
    hr = D3DCompileFromFile(
        L"C:/Build/codecs/pyrowave-dx11/src/shaders/quant.hlsl",
        nullptr, nullptr, "main", "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS |
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS,
        0, &cs_blob, &err_blob
    );

    if (FAILED(hr)) {
        if (err_blob) {
            fprintf(stderr, "quant_compile_shader: compile failed: %s\n",
                    (char*)err_blob->GetBufferPointer());
        }
        return PWD3D11_ERROR_GENERIC;
    }

    hr = st->device->CreateComputeShader(
        cs_blob->GetBufferPointer(),
        cs_blob->GetBufferSize(),
        nullptr,
        &st->shader
    );

    return SUCCEEDED(hr) ? PWD3D11_SUCCESS : PWD3D11_ERROR_GENERIC;
}

// ------------------------------------------------------------------
// Run quant: one dispatch per band (42 for 4:2:0)
// ------------------------------------------------------------------
pwd3d11_result quant_run(QuantState *st)
{
    if (!st || !st->shader) return PWD3D11_ERROR_INVALID_ARGUMENT;

    auto ctx = st->context;
    ID3D11ComputeShader *cs = st->shader.Get();
    ID3D11ShaderResourceView *nullsrv = nullptr;
    ID3D11UnorderedAccessView *nulluav[3] = {nullptr, nullptr, nullptr};

    ctx->CSSetShader(cs, nullptr, 0);

    int band_idx = 0;
    for (auto &b : st->bands) {
        int bw = b.w, bh = b.h;
        int bw_8x8 = (bw + 7) / 8;
        int bh_8x8 = (bh + 7) / 8;

        // Compute push constants
        QuantPushConstants pc = {};
        pc.resolution_x = bw;
        pc.resolution_y = bh;
        pc.resolution_8x8_blocks_x = bw_8x8;
        pc.resolution_8x8_blocks_y = bh_8x8;
        pc.inv_resolution_x = 1.0f / bw;
        pc.inv_resolution_y = 1.0f / bh;
        pc.input_layer = float(band_idx);

        // quant_resolution = 1.0f / decode_quant(encode_quant(1.0f / quant_res))
        float quant_res = get_quant_resolution(b.level, b.component, 0, st->chroma);
        // Simplified: quant_resolution = quant_res (identity for now)
        pc.quant_resolution = quant_res;

        // block_offset and block_stride are computed from band index
        // For now, use a simple mapping
        pc.block_offset = 0;
        pc.block_stride = 0;
        pc.rdo_distortion_scale = get_rdo_distortion_scale(b.level, b.component, 0, st->chroma);

        // Create cbuffer
        ComPtr<ID3D11Buffer> cb;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(QuantPushConstants);
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA sd = { &pc, 0, 0 };
        HRESULT hr = st->device->CreateBuffer(&bd, &sd, &cb);
        if (FAILED(hr)) return PWD3D11_ERROR_GENERIC;

        ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->CSSetShaderResources(0, 1, b.srv.GetAddressOf());
        ID3D11UnorderedAccessView *uavs[3] = { st->uav_meta.Get(), st->uav_stats.Get(), st->uav_payload.Get() };
        ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);

        ctx->Dispatch(bw_8x8, bh_8x8, 1);

        band_idx++;
    }

    ctx->CSSetShaderResources(0, 1, &nullsrv);
    ctx->CSSetUnorderedAccessViews(0, 3, nulluav, nullptr);
    ctx->Flush();

    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Read back meta/stats/payload (for validation) — stub for now
// ------------------------------------------------------------------
pwd3d11_result quant_readback(QuantState *st,
                               std::vector<uint8_t> *out_meta,
                               std::vector<uint8_t> *out_stats,
                               std::vector<uint8_t> *out_payload)
{
    (void)st; (void)out_meta; (void)out_stats; (void)out_payload;
    // TODO: implement staging buffer readback
    return PWD3D11_ERROR_NOT_IMPLEMENTED;
}

// ------------------------------------------------------------------
// Free quant state
// ------------------------------------------------------------------
void quant_destroy(QuantState *st)
{
    if (st) {
        if (st->context) st->context->Flush();
        // ComPtr handles release
    }
}

QuantState::~QuantState() = default;
