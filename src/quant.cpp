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
#include "stage_internal.h"

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
    uint32_t coeff_pitch;         // @44
};
static_assert(sizeof(QuantPushConstants) == 48, "QuantPushConstants must be 48 bytes");

// ------------------------------------------------------------------
// Helper: get quant resolution for a band (R/pyrowave_encoder.cpp:148-176)
// ------------------------------------------------------------------
static uint8_t encode_quant_code(float decoder_q_scale)
{
    uint32_t v = 0;
    memcpy(&v, &decoder_q_scale, sizeof(v));
    int e = int((v >> 23) & 0xff) - 127 - 4;
    int m = int((v >> 20) & 0x7);
    e = -e;
    if (e < 0) e = 0;
    if (e > 20) e = 20;
    return uint8_t((e << 3) | m);
}

static float decode_quant_code(uint8_t quant_code)
{
    int e = 4 - int(quant_code >> 3);
    int m = int(quant_code & 0x7);
    int shift = 20 + e;
    float mag = float(8 + m);
    if (shift >= 0) mag *= float(1 << shift);
    else mag /= float(1 << -shift);
    return mag / (8.0f * 1024.0f * 1024.0f);
}

// R/pyrowave_encoder.cpp get_noise_power_normalized_quant_resolution, capped at 4096.
static float get_quant_resolution(int level, int component, int band, int /*chroma*/)
{
    int bits = 8;
    if (band == 0) bits += 2;
    else if (band < 3) bits += 1;
    bits += level;
    if (component != 0) bits--;
    float quant_res = float(1 << bits);
    if (quant_res > 4096.0f) quant_res = 4096.0f;
    uint8_t code = encode_quant_code(1.0f / quant_res);
    float decoded = decode_quant_code(code);
    if (decoded == 0.0f) return quant_res;
    return 1.0f / decoded;
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

    // Same borrowed-then-AddRef path as dwt_create. Leaving this unset made
    // quant_create_outputs reject every encode as an invalid argument.
    st->device = static_cast<ID3D11Device *>(pwd3d11_device_get_d3d11_device(dev));
    if (!st->device) return PWD3D11_ERROR_INVALID_ARGUMENT;
    st->device->GetImmediateContext(&st->context);
    if (!st->context) return PWD3D11_ERROR_INVALID_ARGUMENT;

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
                                const int *band_w, const int *band_h, const int *band_pitch,
                                const int *level, const int *component, const int *band,
                                int num_bands)
{
    if (!st || !band_srvs || !band_pitch || !level || !component || !band) return PWD3D11_ERROR_INVALID_ARGUMENT;

    st->bands.resize(num_bands);
    int count8 = 0;
    int count32 = 0;
    for (int i = 0; i < num_bands; i++) {
        auto &b = st->bands[i];
        b.srv = band_srvs[i];
        b.w = band_w[i];
        b.h = band_h[i];
        b.pitch = band_pitch[i];
        b.level = level[i];
        b.component = component[i];
        b.band = band[i];
        int b8x = (b.w + 7) / 8;
        int b32x = (b.w + 31) / 32;
        int b32y = (b.h + 31) / 32;
        b.block_offset_8 = count8;
        b.block_stride_8 = b8x;
        b.block_offset_32 = count32;
        b.block_stride_32 = b32x;
        count8 += b8x * ((b.h + 7) / 8);
        count32 += b32x * b32y;
    }
    st->total_blocks_32 = count32;
    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Create output UAVs
// ------------------------------------------------------------------
pwd3d11_result quant_create_outputs(QuantState *st)
{
    if (!st->device) return PWD3D11_ERROR_INVALID_ARGUMENT;

    int total_blocks = 0;
    for (auto &b : st->bands) {
        int bw = (b.w + 7) / 8;
        int bh = (b.h + 7) / 8;
        total_blocks += bw * bh;
    }
    size_t payload_size = 136u + size_t(total_blocks) * 256u;
    payload_size = (payload_size + 15u) & ~size_t(15);
    if (st->meta_buf && st->stats_buf && st->payload_buf
        && st->total_blocks == total_blocks && st->payload_size == payload_size) {
        return PWD3D11_SUCCESS;
    }
    st->total_blocks = total_blocks;
    if (total_blocks <= 0 || st->total_blocks_32 <= 0) return PWD3D11_ERROR_INVALID_ARGUMENT;

    // u0: BlockMeta (8 bytes per block)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = total_blocks * 8;
        bd.Usage = D3D11_USAGE_DEFAULT;
        // Analyze and pack create shader-resource views of this buffer.
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

        ComPtr<ID3D11Buffer> buf;
        HRESULT hr = st->device->CreateBuffer(&bd, nullptr, &buf);
        if (FAILED(hr)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

        st->meta_buf = buf;

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
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

        ComPtr<ID3D11Buffer> buf;
        HRESULT hr = st->device->CreateBuffer(&bd, nullptr, &buf);
        if (FAILED(hr)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

        st->stats_buf = buf;

        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = total_blocks * 16;

        hr = st->device->CreateUnorderedAccessView(buf.Get(), &ud, &st->uav_stats);
        if (FAILED(hr)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    // u2: 8-byte header, 32 stripe bins, then one 256-byte slot per 8x8.
    st->payload_size = 136u + size_t(total_blocks) * 256u;
    st->payload_size = (st->payload_size + 15u) & ~size_t(15);

    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = UINT(st->payload_size);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

        ComPtr<ID3D11Buffer> buf;
        HRESULT hr = st->device->CreateBuffer(&bd, nullptr, &buf);
        if (FAILED(hr)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

        st->payload_buf = buf;

        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = UINT(st->payload_size / 4);

        hr = st->device->CreateUnorderedAccessView(buf.Get(), &ud, &st->uav_payload);
        if (FAILED(hr)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;
        hr = pwd3d11_create_raw_srv(st->device.Get(), buf.Get(), UINT(st->payload_size),
                                    st->srv_payload.ReleaseAndGetAddressOf());
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
    if (st->shader) return PWD3D11_SUCCESS;

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

    pwd3d11_unbind_cs(ctx.Get());
    ctx->CSSetShader(cs, nullptr, 0);
    // Pack and quant only read bytes this dispatch writes. Skip a
    // multi-megabyte ClearUAV. Bytes 0-7 hold the pack word counter.
    // Bytes 8-135 are the 32 quant stripe bins (quant.hlsl PAYLOAD_SLOT0).
    UINT zeros_hdr[34] = {};
    D3D11_BOX box = {};
    box.right = sizeof(zeros_hdr);
    box.bottom = 1;
    box.back = 1;
    ctx->UpdateSubresource(st->payload_buf.Get(), 0, &box, zeros_hdr, 0, 0);
    UINT zero[4] = {};
    ctx->ClearUnorderedAccessViewUint(st->uav_meta.Get(), zero);
    ctx->ClearUnorderedAccessViewUint(st->uav_stats.Get(), zero);

    ID3D11UnorderedAccessView *uavs[3] = { st->uav_meta.Get(), st->uav_stats.Get(), st->uav_payload.Get() };
    ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);

    struct SubCB {
        struct { float layer, qres, z, w; } info[4];
        int off[4];
    };
    static_assert(sizeof(SubCB) == 80, "quant subband constant is 80 bytes");

    size_t set_i = 0;
    for (size_t i = 0; i < st->bands.size(); ) {
        const auto &b0 = st->bands[i];
        if (b0.w <= 0 || b0.h <= 0) { i++; continue; }
        size_t j = i + 1;
        while (j < st->bands.size() && st->bands[j].srv.Get() == b0.srv.Get()
               && st->bands[j].w == b0.w && st->bands[j].h == b0.h
               && st->bands[j].pitch == b0.pitch)
            j++;
        UINT nsub = UINT(j - i);
        if (nsub > 4) return PWD3D11_ERROR_GENERIC;

        QuantPushConstants pc = {};
        pc.resolution_x = b0.w;
        pc.resolution_y = b0.h;
        pc.resolution_8x8_blocks_x = (b0.w + 7) / 8;
        pc.resolution_8x8_blocks_y = (b0.h + 7) / 8;
        pc.inv_resolution_x = 1.0f / float(b0.w);
        pc.inv_resolution_y = 1.0f / float(b0.h);
        pc.block_stride = b0.block_stride_8;
        pc.rdo_distortion_scale = get_rdo_distortion_scale(b0.level, b0.component, b0.band, st->chroma);
        pc.coeff_pitch = uint32_t(b0.pitch);

        SubCB sub = {};
        for (UINT s = 0; s < nsub; s++) {
            const auto &b = st->bands[i + s];
            sub.info[s].layer = float(b.band);
            sub.info[s].qres = get_quant_resolution(b.level, b.component, b.band, st->chroma);
            sub.off[s] = b.block_offset_8;
        }

        if (st->band_cb.size() <= set_i) st->band_cb.resize(set_i + 1);
        if (st->sub_cb.size() <= set_i) st->sub_cb.resize(set_i + 1);
        if (!st->band_cb[set_i]) {
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = sizeof(QuantPushConstants);
            bd.Usage = D3D11_USAGE_IMMUTABLE;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA sd = { &pc, 0, 0 };
            HRESULT hr = st->device->CreateBuffer(&bd, &sd, &st->band_cb[set_i]);
            if (FAILED(hr)) return PWD3D11_ERROR_GENERIC;
        }
        if (!st->sub_cb[set_i]) {
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = sizeof(SubCB);
            bd.Usage = D3D11_USAGE_IMMUTABLE;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA sd = { &sub, 0, 0 };
            HRESULT hr = st->device->CreateBuffer(&bd, &sd, &st->sub_cb[set_i]);
            if (FAILED(hr)) return PWD3D11_ERROR_GENERIC;
        }
        ID3D11Buffer *cbs[2] = { st->band_cb[set_i].Get(), st->sub_cb[set_i].Get() };
        ctx->CSSetConstantBuffers(0, 2, cbs);
        ctx->CSSetShaderResources(0, 1, b0.srv.GetAddressOf());
        int bw_8 = (b0.w + 7) / 8;
        int bh_8 = (b0.h + 7) / 8;
        ctx->Dispatch(UINT((bw_8 + 7) / 8), UINT(bh_8), nsub);
        set_i++;
        i = j;
    }

    ctx->CSSetShaderResources(0, 1, &nullsrv);
    ctx->CSSetUnorderedAccessViews(0, 3, nulluav, nullptr);

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
