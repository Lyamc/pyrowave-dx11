// analyze.cpp -- Host code for analyze + finalize rate control stages (T3)
// Dispatches analyze (per band) and finalize (one per frame) shaders.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <vector>
#include <cstring>
#include <cstdio>

#include "pyrowave_d3d11.h"
#include "debug_dump.h"
#include "stage_internal.h"
#include "shaders/analyze_dxbc.h"
#include "shaders/analyze_finalize_dxbc.h"

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------------
// Push constants (matches cbuffer in analyze.hlsl, 48 bytes)
// ------------------------------------------------------------------
struct AnalyzePushConstants {
    int   resolution_x;              // @0
    int   resolution_y;              // @4
    int   resolution_8x8_blocks_x;   // @8
    int   resolution_8x8_blocks_y;   // @12
    int   block_offset_8x8;          // @16
    int   block_stride_8x8;          // @20
    int   block_offset_32x32;        // @24
    int   block_stride_32x32;        // @28
    unsigned int total_wg_count;            // @32
    unsigned int num_blocks_aligned;        // @36
    unsigned int block_index_shamt;         // @40
    unsigned int _pad;                      // @44
};
static_assert(sizeof(AnalyzePushConstants) == 48, "AnalyzePushConstants must be 48 bytes");

// ------------------------------------------------------------------
// Create analyze state
// ------------------------------------------------------------------
pwd3d11_result analyze_create(AnalyzeState *st, ID3D11Device *dev, int W, int H, int chroma)
{
    if (!dev) return PWD3D11_ERROR_INVALID_ARGUMENT;

    st->device = dev;
    dev->GetImmediateContext(&st->context);
    st->aw = (W + 31) & ~31;
    st->ah = (H + 31) & ~31;
    st->chroma = chroma;

    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Set block stats buffer (from quant stage output)
// ------------------------------------------------------------------
pwd3d11_result analyze_set_stats(AnalyzeState *st, ID3D11Buffer *stats_buffer, int total_blocks)
{
    if (!stats_buffer || !st) return PWD3D11_ERROR_INVALID_ARGUMENT;

    st->stats_buf = stats_buffer;
    st->total_blocks_32x32 = total_blocks;

    // Create SRV
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32_UINT;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.NumElements = UINT(total_blocks * 16); // 64 bytes = 16 uints per block

    HRESULT hr = st->device->CreateShaderResourceView(stats_buffer, &sd, &st->srv_stats);
    return SUCCEEDED(hr) ? PWD3D11_SUCCESS : PWD3D11_ERROR_GENERIC;
}

// ------------------------------------------------------------------
// Create bucket buffer (spec §2 layout)
// ------------------------------------------------------------------
pwd3d11_result analyze_create_buffers(AnalyzeState *st)
{
    if (!st->device) return PWD3D11_ERROR_INVALID_ARGUMENT;

    // per_sub = next_pow2(align(block_count_32x32, 16) / 16)
    int aligned_blocks = (st->total_blocks_32x32 + 15) & ~15;
    int per_sub = aligned_blocks / 16;
    // Round per_sub to next power of 2
    while (per_sub & (per_sub - 1)) per_sub &= per_sub - 1;
    if (per_sub < 1) per_sub = 1;

    // Bucket buffer size: 64 + 128*16*4 + 128 * per_sub * 16 * 8
    size_t bucket_size = 64ull + 128ull * 16ull * 4ull + 128ull * size_t(per_sub) * 16ull * 8ull;
    st->bucket_size = bucket_size;

    // Create buffer
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = UINT(bucket_size);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

    HRESULT hr = st->device->CreateBuffer(&bd, nullptr, &st->bucket_buf);
    if (FAILED(hr)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    // Create UAV
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R32_UINT;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = UINT(bucket_size / 4);

    hr = st->device->CreateUnorderedAccessView(st->bucket_buf.Get(), &ud, &st->uav_bucket);
    if (FAILED(hr)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    // Create quant data buffer (one int per 32x32 block)
    D3D11_BUFFER_DESC bd_quant = {};
    bd_quant.ByteWidth = st->total_blocks_32x32 * 4;
    bd_quant.Usage = D3D11_USAGE_DEFAULT;
    bd_quant.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

    hr = st->device->CreateBuffer(&bd_quant, nullptr, &st->quant_buf);
    if (FAILED(hr)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    D3D11_UNORDERED_ACCESS_VIEW_DESC ud_quant = {};
    ud_quant.Format = DXGI_FORMAT_R32_SINT;
    ud_quant.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud_quant.Buffer.NumElements = st->total_blocks_32x32;

    hr = st->device->CreateUnorderedAccessView(st->quant_buf.Get(), &ud_quant, &st->uav_quant);
    if (FAILED(hr)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Compile shaders
// ------------------------------------------------------------------
pwd3d11_result analyze_compile_shaders(AnalyzeState *st)
{
    if (!st->device) return PWD3D11_ERROR_INVALID_ARGUMENT;

    // Analyze shader
    HRESULT hr = st->device->CreateComputeShader(
        const_cast<unsigned char*>(g_analyze_dxbc),
        g_analyze_dxbc_len,
        nullptr,
        &st->analyze_shader
    );
    if (FAILED(hr)) return PWD3D11_ERROR_GENERIC;

    // Finalize shader
    hr = st->device->CreateComputeShader(
        const_cast<unsigned char*>(g_analyze_finalize_dxbc),
        g_analyze_finalize_dxbc_len,
        nullptr,
        &st->finalize_shader
    );
    if (FAILED(hr)) return PWD3D11_ERROR_GENERIC;

    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Run analyze: one dispatch per band
// ------------------------------------------------------------------
pwd3d11_result analyze_run(AnalyzeState *st,
                           int bw, int bh, int block_offset_8x8, int block_stride_8x8,
                           int block_offset_32x32, int block_stride_32x32)
{
    if (!st || !st->analyze_shader || !st->srv_stats) return PWD3D11_ERROR_INVALID_ARGUMENT;

    auto ctx = st->context;
    ID3D11ComputeShader *cs = st->analyze_shader.Get();

    int bw_32 = (bw + 31) / 32;
    int bh_32 = (bh + 31) / 32;

    // Compute per_sub and num_blocks_aligned
    int aligned_blocks = (st->total_blocks_32x32 + 15) & ~15;
    int per_sub = aligned_blocks / 16;
    while (per_sub & (per_sub - 1)) per_sub &= per_sub - 1;
    if (per_sub < 1) per_sub = 1;
    unsigned int num_blocks_aligned = unsigned int(per_sub) * 16u;
    unsigned int block_index_shamt = 0;
    while ((1u << block_index_shamt) < unsigned int(per_sub)) block_index_shamt++;

    AnalyzePushConstants pc = {};
    pc.resolution_x = bw;
    pc.resolution_y = bh;
    pc.resolution_8x8_blocks_x = (bw + 7) / 8;
    pc.resolution_8x8_blocks_y = (bh + 7) / 8;
    pc.block_offset_8x8 = block_offset_8x8;
    pc.block_stride_8x8 = block_stride_8x8;
    pc.block_offset_32x32 = block_offset_32x32;
    pc.block_stride_32x32 = block_stride_32x32;
    pc.total_wg_count = unsigned int(bw_32 * bh_32);
    pc.num_blocks_aligned = num_blocks_aligned;
    pc.block_index_shamt = block_index_shamt;

    // Create cbuffer
    ComPtr<ID3D11Buffer> cb;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(AnalyzePushConstants);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = { &pc, 0, 0 };
    HRESULT hr = st->device->CreateBuffer(&bd, &sd, &cb);
    if (FAILED(hr)) return PWD3D11_ERROR_GENERIC;

    ID3D11UnorderedAccessView *uavs[1] = { st->uav_bucket.Get() };
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
    ctx->CSSetShaderResources(0, 1, &st->srv_stats);
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ctx->Dispatch(bw_32, bh_32, 1);

    // Clear bindings
    ID3D11ShaderResourceView *nullsrv = nullptr;
    ID3D11UnorderedAccessView *nulluav = nullptr;
    ctx->CSSetShaderResources(0, 1, &nullsrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nulluav, nullptr);

    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Run finalize: one dispatch of 512 threads
// ------------------------------------------------------------------
pwd3d11_result analyze_finalize_run(AnalyzeState *st)
{
    if (!st || !st->finalize_shader) return PWD3D11_ERROR_INVALID_ARGUMENT;

    auto ctx = st->context;
    ID3D11ComputeShader *cs = st->finalize_shader.Get();

    ID3D11UnorderedAccessView *uavs[1] = { st->uav_bucket.Get() };
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ctx->Dispatch(512, 1, 1);

    ID3D11UnorderedAccessView *nulluav = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &nulluav, nullptr);

    ctx->Flush();
    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Free analyze state
// ------------------------------------------------------------------
void analyze_destroy(AnalyzeState *st)
{
    if (st) {
        if (st->context) st->context->Flush();
    }
}

AnalyzeState::~AnalyzeState() = default;
