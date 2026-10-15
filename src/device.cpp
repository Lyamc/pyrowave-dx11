// device.cpp -- D3D11 device creation, adapter selection, WARP rejection
// Mirrors prototype/dx11dwt.cpp:346-377 device setup pattern.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdio>

#include "pyrowave_d3d11.h"

using Microsoft::WRL::ComPtr;

// Internal device state
struct pwd3d11_device_t {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc;
    bool warp_allowed;
};

// Check if an adapter is software/WARP
static bool is_software_adapter(IDXGIAdapter *adapter)
{
    DXGI_ADAPTER_DESC desc;
    if (FAILED(adapter->GetDesc(&desc))) return false;
    // Microsoft Basic Render Driver (WARP) has VendorId 0x1414 and DeviceId 0x008C
    if (desc.VendorId == 0x1414 && desc.DeviceId == 0x008C) return true;
    // Check description for "warp" or "basic"
    wchar_t lower[128];
    for (unsigned i = 0; desc.Description[i]; ++i) lower[i] = (wchar_t)(desc.Description[i] | 0x20);
    lower[127] = 0;
    if (wcsstr(lower, L"warp")) return true;
    if (wcsstr(lower, L"basic")) return true;
    return false;
}

// Find hardware adapter at given index (or first if index == -1)
// If warp_allowed is true, WARP adapters are not skipped
static HRESULT find_adapter(int32_t adapter_index, bool warp_allowed, ComPtr<IDXGIAdapter> *out)
{
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return hr;

    int found = 0;
    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter> adapter;
        hr = factory->EnumAdapters(i, adapter.ReleaseAndGetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) return hr;

        bool is_warp = is_software_adapter(adapter.Get());
        if (is_warp && !warp_allowed) continue;

        if (adapter_index >= 0 && found < adapter_index) {
            ++found;
            continue;
        }

        *out = adapter;
        return S_OK;
    }
    return DXGI_ERROR_NOT_FOUND;
}

uint32_t pwd3d11_get_api_version(void)
{
    return PWD3D11_API_VERSION;
}

pwd3d11_result pwd3d11_create_device(const pwd3d11_device_create_info *info, pwd3d11_device *out)
{
    if (!info || !out) return PWD3D11_ERROR_INVALID_ARGUMENT;
    if (info->struct_size != sizeof(pwd3d11_device_create_info)) return PWD3D11_ERROR_INVALID_ARGUMENT;

    pwd3d11_device_t *dev = new pwd3d11_device_t();
    dev->warp_allowed = (info->flags & 2) != 0;  // bit1: allow WARP

    // If host provides a device, share it
    if (info->d3d11_device) {
        ComPtr<ID3D11Device> host_device;
        HRESULT hr = reinterpret_cast<ID3D11Device*>(info->d3d11_device)->
            QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(host_device.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            delete dev;
            return PWD3D11_ERROR_INVALID_ARGUMENT;
        }
        dev->device = host_device;
        host_device->GetImmediateContext(&dev->context);

        // Get adapter info
        ComPtr<IDXGIDevice> dxgi_device;
        hr = host_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgi_device.ReleaseAndGetAddressOf()));
        if (SUCCEEDED(hr)) {
            dxgi_device->GetAdapter(&dev->adapter);
            dev->adapter->GetDesc(&dev->desc);
        }
        *out = dev;
        return PWD3D11_SUCCESS;
    }

    // Find a hardware adapter
    ComPtr<IDXGIAdapter> adapter;
    HRESULT hr = find_adapter(info->adapter_index, dev->warp_allowed, &adapter);
    if (FAILED(hr)) {
        delete dev;
        *out = nullptr;
        return PWD3D11_ERROR_NO_VULKAN;  // -5: no usable device
    }

    dev->adapter = adapter;
    adapter->GetDesc(&dev->desc);

    // Create D3D11 device with FL 11_0
    D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got;
    hr = D3D11CreateDevice(
        adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
#if defined(_DEBUG)
        D3D11_CREATE_DEVICE_DEBUG  // bit0: debug layer
#else
        0
#endif
        ,
        fls,
        _countof(fls),
        D3D11_SDK_VERSION,
        &dev->device,
        &got,
        &dev->context
    );

    if (FAILED(hr) || got < D3D_FEATURE_LEVEL_11_0) {
        delete dev;
        *out = nullptr;
        return PWD3D11_ERROR_NO_VULKAN;
    }

    // Verify R16_FLOAT typed UAV store support
    UINT fs = 0;
    hr = dev->device->CheckFormatSupport(DXGI_FORMAT_R16_FLOAT, &fs);
    if (FAILED(hr) || !(fs & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) {
        delete dev;
        *out = nullptr;
        return PWD3D11_ERROR_NO_VULKAN;
    }

    *out = dev;
    return PWD3D11_SUCCESS;
}

pwd3d11_result pwd3d11_device_get_adapter_info(pwd3d11_device dev, pwd3d11_adapter_info *out)
{
    if (!dev || !out) return PWD3D11_ERROR_INVALID_ARGUMENT;
    if (!dev->adapter) return PWD3D11_ERROR_GENERIC;

    // Re-read desc in case it wasn't populated
    if (dev->desc.DeviceId == 0) {
        dev->adapter->GetDesc(&dev->desc);
    }

    // Copy name (wide to narrow, truncating if needed)
    for (int i = 0; i < 127 && dev->desc.Description[i]; ++i) {
        out->name[i] = static_cast<char>(dev->desc.Description[i]);
    }
    out->name[127] = '\0';
    out->vendor_id = dev->desc.VendorId;
    out->device_id = dev->desc.DeviceId;
    out->dedicated_video_memory = static_cast<uint64_t>(dev->desc.DedicatedVideoMemory);

    return PWD3D11_SUCCESS;
}

void pwd3d11_device_destroy(pwd3d11_device dev)
{
    if (dev) {
        if (dev->context) dev->context->Flush();
        dev->context.Reset();
        dev->device.Reset();
        dev->adapter.Reset();
        delete dev;
    }
}
