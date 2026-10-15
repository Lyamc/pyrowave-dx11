// test_t0.cpp -- T0 acceptance tests
// Tests: CREATE_DEVICE, REJECT_WARP, REJECT_WARP_FLAG, T-IN-0 (exact plane upload)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>

#include "pyrowave_d3d11.h"

using Microsoft::WRL::ComPtr;

static bool test_create_device()
{
    pwd3d11_device_create_info ci = {};
    ci.struct_size = sizeof(ci);
    ci.adapter_index = -1;
    ci.flags = 0;
    pwd3d11_device dev = nullptr;
    pwd3d11_result r = pwd3d11_create_device(&ci, &dev);
    if (r != PWD3D11_SUCCESS || !dev) {
        printf("  CREATE_DEVICE failed: result=%d\n", (int)r);
        return false;
    }
    pwd3d11_device_destroy(dev);
    return true;
}

static bool test_reject_warp()
{
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        return true;
    }

    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter> adapter;
        if (factory->EnumAdapters(i, adapter.ReleaseAndGetAddressOf()) != S_OK) break;

        DXGI_ADAPTER_DESC desc;
        if (FAILED(adapter->GetDesc(&desc))) continue;

        bool is_warp = false;
        if (desc.VendorId == 0x1414 && desc.DeviceId == 0x008C) is_warp = true;
        if (wcsstr(desc.Description, L"warp") || wcsstr(desc.Description, L"basic")) is_warp = true;

        if (!is_warp) continue;

        pwd3d11_device_create_info ci = {};
        ci.struct_size = sizeof(ci);
        ci.adapter_index = (int32_t)i;
        ci.flags = 0;
        pwd3d11_device dev = nullptr;
        pwd3d11_result r = pwd3d11_create_device(&ci, &dev);

        if (r == PWD3D11_SUCCESS && dev) {
            pwd3d11_device_destroy(dev);
            printf("  REJECT_WARP FAILED: WARP adapter %u was accepted without flag\n", i);
            return false;
        }
    }
    return true;
}

static bool test_reject_warp_with_flag()
{
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        return true;
    }

    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter> adapter;
        if (factory->EnumAdapters(i, adapter.ReleaseAndGetAddressOf()) != S_OK) break;

        DXGI_ADAPTER_DESC desc;
        if (FAILED(adapter->GetDesc(&desc))) continue;

        bool is_warp = false;
        if (desc.VendorId == 0x1414 && desc.DeviceId == 0x008C) is_warp = true;
        if (wcsstr(desc.Description, L"warp") || wcsstr(desc.Description, L"basic")) is_warp = true;

        if (!is_warp) continue;

        pwd3d11_device_create_info ci = {};
        ci.struct_size = sizeof(ci);
        ci.adapter_index = (int32_t)i;
        ci.flags = 2;
        pwd3d11_device dev = nullptr;
        pwd3d11_result r = pwd3d11_create_device(&ci, &dev);

        if (r != PWD3D11_SUCCESS || !dev) {
            printf("  REJECT_WARP_FLAG FAILED: WARP should be accepted when flag is set\n");
            return false;
        }
        pwd3d11_device_destroy(dev);
    }
    return true;
}

static bool test_t_in_0()
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.ReleaseAndGetAddressOf())))) {
        printf("  T-IN-0: factory creation failed\n");
        return false;
    }

    ComPtr<IDXGIAdapter> hw_adapter;
    ComPtr<IDXGIAdapter1> adapter1;
    for (UINT i = 0; ; ++i) {
        if (factory->EnumAdapters1(i, adapter1.ReleaseAndGetAddressOf()) != S_OK) break;
        DXGI_ADAPTER_DESC1 desc;
        adapter1->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            adapter1->QueryInterface(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(hw_adapter.ReleaseAndGetAddressOf()));
            break;
        }
        adapter1.Reset();
    }
    if (!hw_adapter) {
        printf("  T-IN-0: no hardware adapter found\n");
        return false;
    }

    D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got;
    ComPtr<ID3D11Device> test_dev;
    ComPtr<ID3D11DeviceContext> test_ctx;
    HRESULT hr = D3D11CreateDevice(hw_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                    fls, 1, D3D11_SDK_VERSION, &test_dev, &got, &test_ctx);
    if (FAILED(hr) || got < D3D_FEATURE_LEVEL_11_0) {
        printf("  T-IN-0: device creation failed hr=0x%x\n", (unsigned)hr);
        return false;
    }

    const int W = 32, H = 32;
    std::vector<uint8_t> host_data(W * H);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            host_data[y * W + x] = ((x + y) & 1) ? 255 : 0;
        }
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;

    ComPtr<ID3D11Texture2D> tex;
    hr = test_dev->CreateTexture2D(&td, nullptr, &tex);
    if (FAILED(hr)) {
        printf("  T-IN-0: texture creation failed hr=0x%x\n", (unsigned)hr);
        return false;
    }

    D3D11_BOX box = { 0, 0, 0, (UINT)W, (UINT)H, 1 };
    test_ctx->UpdateSubresource(tex.Get(), 0, &box, host_data.data(), W, W * H);
    test_ctx->Flush();

    D3D11_TEXTURE2D_DESC td_readback = td;
    td_readback.Usage = D3D11_USAGE_STAGING; td_readback.BindFlags = 0; td_readback.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    hr = test_dev->CreateTexture2D(&td_readback, nullptr, &staging);
    if (FAILED(hr)) {
        printf("  T-IN-0: staging texture creation failed hr=0x%x\n", (unsigned)hr);
        return false;
    }

    test_ctx->CopyResource(staging.Get(), tex.Get());
    test_ctx->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = test_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        printf("  T-IN-0: Map failed hr=0x%x\n", (unsigned)hr);
        return false;
    }

    bool match = true;
    const uint8_t *readback = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < H; ++y) {
        const uint8_t *row = readback + y * mapped.RowPitch;
        for (int x = 0; x < W; ++x) {
            int idx = y * W + x;
            if (row[x] != host_data[idx]) {
                printf("  T-IN-0: mismatch at byte %d: host=%d read=%d\n", idx, host_data[idx], row[x]);
                match = false;
                goto done;
            }
        }
    }
    done:;

    test_ctx->Unmap(staging.Get(), 0);

    return match;
}

int main()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int passed = 0;
    int total = 0;

    {
        ++total;
        printf("TEST: CREATE_DEVICE ... ");
        if (test_create_device()) {
            printf("PASS\n");
            ++passed;
        } else {
            printf("FAIL\n");
        }
    }

    {
        ++total;
        printf("TEST: REJECT_WARP ... ");
        if (test_reject_warp()) {
            printf("PASS\n");
            ++passed;
        } else {
            printf("FAIL\n");
        }
    }

    {
        ++total;
        printf("TEST: REJECT_WARP_FLAG ... ");
        if (test_reject_warp_with_flag()) {
            printf("PASS\n");
            ++passed;
        } else {
            printf("FAIL\n");
        }
    }

    {
        ++total;
        printf("TEST: T-IN-0 ... ");
        if (test_t_in_0()) {
            printf("PASS\n");
            ++passed;
        } else {
            printf("FAIL\n");
        }
    }

    printf("\nResults: %d/%d tests passed\n", passed, total);
    CoUninitialize();
    return (passed == total) ? 0 : 1;
}
