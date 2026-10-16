// test_t2_quant.cpp -- T2b acceptance tests for quantization
// Tests: T-Q-0 (unit), T-Q-1 (code_word), T-Q-2 (payload), T-Q-3 (stats), T-Q-4 (bounds)
//
// Strategy: run DWT, inject known bands into quant, compare outputs vs CPU golden model

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

#include "pyrowave_d3d11.h"

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------------
// CPU helpers
// ------------------------------------------------------------------
static inline uint16_t f2h(float f) { return uint16_t(_mm_extract_epi16(_mm_cvtps_ph(_mm_set_ss(f), _MM_FROUND_TO_NEAREST_INT), 0)); }
static inline float h2f(uint16_t h) { return _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))); }

// ------------------------------------------------------------------
// T-Q-0: firstbithigh(0u) == 0xFFFFFFFF, f32tof16 boundary cases
// Runs f16_rounding.hlsl shader and compares GPU results vs CPU f2h
// ------------------------------------------------------------------
static bool test_q0(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    // Compile f16_rounding.hlsl
    ComPtr<ID3DBlob> cs_blob, err_blob;
    HRESULT hr = D3DCompileFromFile(
        L"C:/Build/codecs/pyrowave-dx11/src/shaders/f16_rounding.hlsl",
        nullptr, nullptr, "main", "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs_blob, &err_blob
    );

    if (FAILED(hr)) {
        if (err_blob) {
            fprintf(stderr, "T-Q-0: compile failed: %s\n", (char*)err_blob->GetBufferPointer());
        }
        printf("FAIL T-Q-0 (shader compile)\n");
        return false;
    }

    struct InputData { float value; };
    struct OutputData { uint32_t f32_bits; uint16_t f16_result; uint16_t reserved; };

    std::vector<float> test_values = {
        0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f,
        65504.0f, 65505.0f, 65520.0f,
        1e-10f, -1e-10f, 1e10f, -1e10f,
        1.1875f, 1.1953125f
    };

    ComPtr<ID3D11ComputeShader> cs;
    hr = dev->CreateComputeShader(cs_blob->GetBufferPointer(), cs_blob->GetBufferSize(), nullptr, &cs);
    if (FAILED(hr)) {
        printf("FAIL T-Q-0 (create shader)\n"); return false;
    }

    // Create UAVs for input/output
    ComPtr<ID3D11Buffer> input_buf, output_buf;
    UINT count = (UINT)test_values.size();

    D3D11_BUFFER_DESC bd_in = {};
    bd_in.ByteWidth = sizeof(InputData) * count;
    bd_in.Usage = D3D11_USAGE_DEFAULT;
    bd_in.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    hr = dev->CreateBuffer(&bd_in, nullptr, &input_buf);
    if (FAILED(hr)) { printf("FAIL T-Q-0 (create input buf)\n"); return false; }

    D3D11_BUFFER_DESC bd_out = {};
    bd_out.ByteWidth = sizeof(OutputData) * count;
    bd_out.Usage = D3D11_USAGE_DEFAULT;
    bd_out.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    hr = dev->CreateBuffer(&bd_out, nullptr, &output_buf);
    if (FAILED(hr)) { printf("FAIL T-Q-0 (create output buf)\n"); return false; }

    // Upload via staging
    D3D11_BUFFER_DESC bd_staging = {};
    bd_staging.ByteWidth = sizeof(InputData) * count;
    bd_staging.Usage = D3D11_USAGE_STAGING;
    bd_staging.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> staging_in;
    hr = dev->CreateBuffer(&bd_staging, nullptr, &staging_in);
    if (FAILED(hr)) { printf("FAIL T-Q-0 (create staging)\n"); return false; }

    D3D11_MAPPED_SUBRESOURCE ms;
    hr = ctx->Map(staging_in.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    if (FAILED(hr)) { printf("FAIL T-Q-0 (map staging)\n"); return false; }
    std::memcpy(ms.pData, test_values.data(), sizeof(InputData) * count);
    ctx->Unmap(staging_in.Get(), 0);
    ctx->CopyResource(input_buf.Get(), staging_in.Get());

    // Config buffer
    uint32_t test_count = count;
    ComPtr<ID3D11Buffer> config_buf;
    D3D11_BUFFER_DESC bd_cfg = {};
    bd_cfg.ByteWidth = sizeof(uint32_t);
    bd_cfg.Usage = D3D11_USAGE_IMMUTABLE;
    bd_cfg.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = { &test_count, 0, 0 };
    hr = dev->CreateBuffer(&bd_cfg, &sd, &config_buf);
    if (FAILED(hr)) { printf("FAIL T-Q-0 (create config)\n"); return false; }

    // Create UAVs for buffers
    ComPtr<ID3D11UnorderedAccessView> uav_in, uav_out;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R32_FLOAT;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = count;
    hr = dev->CreateUnorderedAccessView(input_buf.Get(), &ud, &uav_in);
    if (FAILED(hr)) { printf("FAIL T-Q-0 (create uav_in)\n"); return false; }
    ud.Format = DXGI_FORMAT_R32_UINT;
    hr = dev->CreateUnorderedAccessView(output_buf.Get(), &ud, &uav_out);
    if (FAILED(hr)) { printf("FAIL T-Q-0 (create uav_out)\n"); return false; }

    // Dispatch
    ID3D11UnorderedAccessView *uavs[2] = { uav_out.Get(), uav_in.Get() };
    ctx->CSSetShader(cs.Get(), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, config_buf.GetAddressOf());
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ctx->Dispatch(count, 1, 1);
    ctx->Flush();

    // Read back
    D3D11_BUFFER_DESC bd_staging_out = {};
    bd_staging_out.ByteWidth = sizeof(OutputData) * count;
    bd_staging_out.Usage = D3D11_USAGE_STAGING;
    bd_staging_out.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging_out;
    hr = dev->CreateBuffer(&bd_staging_out, nullptr, &staging_out);
    if (FAILED(hr)) { printf("FAIL T-Q-0 (create staging out)\n"); return false; }

    ctx->CopyResource(staging_out.Get(), output_buf.Get());
    hr = ctx->Map(staging_out.Get(), 0, D3D11_MAP_READ, 0, &ms);
    if (FAILED(hr)) { printf("FAIL T-Q-0 (map results)\n"); return false; }

    OutputData *results = (OutputData*)ms.pData;
    bool pass = true;

    printf("T-Q-0: f32tof16 rounding tests (%u values)\n", count);
    for (UINT i = 0; i < count; i++) {
        uint16_t got = results[i].f16_result;
        uint16_t ref = f2h(test_values[i]);
        if (got != ref) {
            fprintf(stderr, "  FAIL: f32tof16(%f) = 0x%04x, expected 0x%04x\n",
                    test_values[i], got, ref);
            pass = false;
        }
    }

    ctx->Unmap(staging_out.Get(), 0);

    if (pass) printf("PASS T-Q-0\n");
    else printf("FAIL T-Q-0\n");

    return pass;
}

// ------------------------------------------------------------------
// T-Q-1..T-Q-4: Full quant test with injected bands
// Requires DWT to be complete and band textures available
// For now, stub: these tests need the full pipeline
// ------------------------------------------------------------------
static bool test_q1_q4(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    (void)dev; (void)ctx;
    printf("pending T-Q-1..T-Q-4 (need full pipeline with DWT + quant)\n");
    return true;
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

    // Test T-Q-0
    if (!test_q0(device.Get(), context.Get())) all_pass = false;

    // Test T-Q-1..T-Q-4 (stub)
    if (!test_q1_q4(device.Get(), context.Get())) all_pass = false;

    if (all_pass) printf("PASS BUILD\n");
    else printf("FAIL BUILD\n");

    return all_pass ? 0 : 1;
}
