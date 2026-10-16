// resolve.cpp -- Host code for resolve rate control stage (T4)
// Dispatches the resolve shader: one workgroup per bucket subdivision.

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
#include "shaders/resolve_dxbc.h"

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------------
// Push constants (matches cbuffer in resolve.hlsl, 16 bytes)
// ------------------------------------------------------------------
struct ResolvePushConstants {
    unsigned int target_payload_size;       // @0
    unsigned int num_blocks_per_subdivision;// @4 = per_sub
    unsigned int _pad[2];                   // @8
};
static_assert(sizeof(ResolvePushConstants) == 16, "ResolvePushConstants must be 16 bytes");

// ------------------------------------------------------------------
// Resolve state
// ------------------------------------------------------------------
struct ResolveState {
    ~ResolveState();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> resolve_shader;

    int per_sub = 0;
};

// ------------------------------------------------------------------
// Create resolve state
// ------------------------------------------------------------------
pwd3d11_result resolve_create(ResolveState *st, ID3D11Device *dev, int per_sub)
{
    if (!dev || per_sub <= 0) return PWD3D11_ERROR_INVALID_ARGUMENT;

    st->device = dev;
    dev->GetImmediateContext(&st->context);
    st->per_sub = per_sub;

    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Compile resolve shader
// ------------------------------------------------------------------
pwd3d11_result resolve_compile_shaders(ResolveState *st)
{
    if (!st->device) return PWD3D11_ERROR_INVALID_ARGUMENT;

    HRESULT hr = st->device->CreateComputeShader(
        const_cast<unsigned char*>(g_resolve_dxbc),
        g_resolve_dxbc_len,
        nullptr,
        &st->resolve_shader
    );
    if (FAILED(hr)) return PWD3D11_ERROR_GENERIC;

    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Run resolve: one dispatch per bucket subdivision
// ------------------------------------------------------------------
pwd3d11_result resolve_run(ResolveState *st,
                            ID3D11ShaderResourceView *srv_bucket,
                            ID3D11UnorderedAccessView *uav_quant,
                            unsigned int target_payload_size)
{
    if (!st || !st->resolve_shader || !srv_bucket || !uav_quant)
        return PWD3D11_ERROR_INVALID_ARGUMENT;

    auto ctx = st->context;
    ID3D11ComputeShader *cs = st->resolve_shader.Get();

    // Number of workgroups = 128 (one per bucket entry)
    const int NUM_BUCKETS = 128;

    // Push constants
    ResolvePushConstants pc = {};
    pc.target_payload_size = target_payload_size;
    pc.num_blocks_per_subdivision = (unsigned int)st->per_sub;

    // Create cbuffer
    ComPtr<ID3D11Buffer> cb;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(ResolvePushConstants);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = { &pc, 0, 0 };
    HRESULT hr = st->device->CreateBuffer(&bd, &sd, &cb);
    if (FAILED(hr)) return PWD3D11_ERROR_GENERIC;

    // Bind resources:
    //   t0 = bucket buffer (SRV, ByteAddressBuffer)
    //   u1 = quant buffer (UAV, RWStructuredBuffer<int>)
    ID3D11ShaderResourceView *srvs[1] = { srv_bucket };
    ID3D11UnorderedAccessView *uavs[1] = { uav_quant };
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
    ctx->CSSetShaderResources(0, 1, srvs);
    ctx->CSSetUnorderedAccessViews(1, 1, uavs, nullptr);
    ctx->Dispatch(NUM_BUCKETS, 1, 1);

    // Clear bindings
    ID3D11ShaderResourceView *nullsrv = nullptr;
    ID3D11UnorderedAccessView *nulluav = nullptr;
    ctx->CSSetShaderResources(0, 1, &nullsrv);
    ctx->CSSetUnorderedAccessViews(1, 1, &nulluav, nullptr);

    ctx->Flush();
    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Free resolve state
// ------------------------------------------------------------------
void resolve_destroy(ResolveState *st)
{
    if (st) {
        if (st->context) st->context->Flush();
    }
}

ResolveState::~ResolveState() = default;
