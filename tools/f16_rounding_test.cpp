// OQ-3 f16 rounding test - simplified
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
using Microsoft::WRL::ComPtr;

static uint16_t cpu_f32tof16(float f)
{
    uint32_t i; memcpy(&i, &f, 4);
    int s = (int)(i >> 31), exp = ((int)(i >> 23) & 0xff) - 127, mant = (int)(i & 0x7fffff);
    if (exp >= 16) return (uint16_t)(s << 15) | 0x7c00u;
    if (exp <= -16) return (uint16_t)(s << 15);
    int hexp = exp + 15, rb = (mant >> 10) & 1, sticky = (mant & 0x3ff) != 0, half = (mant & 0x3ff) == 0x200;
    uint16_t r = (uint16_t)(s << 15) | (uint16_t)(hexp << 10) | (uint16_t)((mant >> 11) & 0x3ff);
    return r + (rb && (sticky || half));
}

int main()
{
    const float tv[] = {1.5f, 3.0f, 0.5f, 0.25f, 65504.0f, 65505.0f, 1e20f, 60000.0f, 100.0f, 1000.0f};
    const int N = 10;
    printf("OQ-3 f16 test: %d values\n", N);

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, 0, 0, 0, 0, D3D11_SDK_VERSION, &dev, 0, &ctx);
    if (!dev) D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, 0, 0, 0, 0, D3D11_SDK_VERSION, &dev, 0, &ctx);
    printf("FL: 0x%08lx\n", (unsigned long)dev->GetFeatureLevel());

    // Simple shader: just write f32tof16 of a constant
    const char* src =
        "cbuffer C : register(b0) { uint N; };\n"
        "RWStructuredBuffer<uint> data : register(u0);\n"
        "[numthreads(64,1,1)]\n"
        "void main(uint t : SV_DispatchThreadID) {\n"
        "  if (t >= N) return;\n"
        "  uint fb = 0x3f800000u + t;\n"
        "  float v = asfloat(fb);\n"
        "  data[t] = f32tof16(v);\n"
        "}\n";
    
    ID3DBlob* sb = nullptr, *eb = nullptr;
    D3DCompile(src, strlen(src), 0, 0, 0, "main", "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS|D3DCOMPILE_ENABLE_STRICTNESS, 0, &sb, &eb);
    if (FAILED(sb ? S_OK : E_FAIL) || !sb) {
        if (eb) { fprintf(stderr, "Compile error:\n%.*s\n", (int)eb->GetBufferSize(), eb->GetBufferPointer()); eb->Release(); }
        return 1;
    }

    ComPtr<ID3D11ComputeShader> cs;
    dev->CreateComputeShader(sb->GetBufferPointer(), sb->GetBufferSize(), 0, &cs);
    sb->Release();

    // Buffer
    D3D11_BUFFER_DESC bd;
    bd.ByteWidth = N * 4; bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS; bd.CPUAccessFlags = 0;
    bd.MiscFlags = 0; bd.StructureByteStride = 0;
    ComPtr<ID3D11Buffer> buf;
    dev->CreateBuffer(&bd, 0, &buf);

    ComPtr<ID3D11UnorderedAccessView> uav;
    dev->CreateUnorderedAccessView(buf.Get(), 0, &uav);

    uint32_t nc = N;
    D3D11_BUFFER_DESC cbd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA cid = { &nc, 4, 0 };
    ComPtr<ID3D11Buffer> cb;
    dev->CreateBuffer(&cbd, &cid, &cb);

    ctx->CSSetShader(cs.Get(), 0, 0);
    ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
    ctx->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), 0);
    ctx->Dispatch((N + 63) / 64, 1, 1);
    ctx->Flush();

    // Read back
    D3D11_BUFFER_DESC sbd = { N * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
    ComPtr<ID3D11Buffer> st;
    dev->CreateBuffer(&sbd, 0, &st);
    ctx->CopyResource(st.Get(), buf.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m);
    uint32_t* gpu = (uint32_t*)m.pData;
    ctx->Unmap(st.Get(), 0);

    printf("\n// GPU f32tof16 results (dispatching t=0..N-1, value = asfloat(t+0x3f800000))\n");
    int fail = 0;
    for (int i = 0; i < N; i++) {
        uint32_t fb = (uint32_t)(0x3f800000u + i);
        float v; memcpy(&v, &fb, 4);
        uint16_t expected = cpu_f32tof16(v);
        uint16_t got = (uint16_t)gpu[i];
        bool ok = got == expected;
        if (!ok) fail++;
        printf("  t=%d v=%.6f cpu=0x%04x gpu=0x%04x %s\n", i, v, expected, got, ok ? "OK" : "FAIL");
    }
    printf("\n// Total: %d, Failures: %d\n%s\n", N, fail, fail == 0 ? "OQ-3: PASS" : "OQ-3: FAIL");
    return fail;
}
