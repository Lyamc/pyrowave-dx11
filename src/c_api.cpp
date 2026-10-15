// c_api.cpp -- C ABI entry points for pyrowave_d3d11.dll
// Stubs for encoder functions (T5b fills these in).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "pyrowave_d3d11.h"

using Microsoft::WRL::ComPtr;

// Internal encoder state (stub)
struct pwd3d11_encoder_t {
    pwd3d11_device device;
    int width;
    int height;
    pwd3d11_chroma_subsampling chroma;
};

pwd3d11_result pwd3d11_encoder_create(const pwd3d11_encoder_create_info *info, pwd3d11_encoder *out)
{
    if (!info || !out || !info->device) return PWD3D11_ERROR_INVALID_ARGUMENT;
    if (info->width < 16 || info->height < 16) return PWD3D11_ERROR_INVALID_ARGUMENT;
    if ((info->width & 1) || (info->height & 1)) return PWD3D11_ERROR_INVALID_ARGUMENT;
    if (info->chroma != PWD3D11_CHROMA_SUBSAMPLING_420 && info->chroma != PWD3D11_CHROMA_SUBSAMPLING_444)
        return PWD3D11_ERROR_INVALID_ARGUMENT;

    pwd3d11_encoder_t *enc = new pwd3d11_encoder_t();
    enc->device = info->device;
    enc->width = info->width;
    enc->height = info->height;
    enc->chroma = info->chroma;
    *out = enc;
    return PWD3D11_SUCCESS;
}

pwd3d11_result pwd3d11_encoder_encode_cpu(pwd3d11_encoder, const pwd3d11_cpu_buffer *, const pwd3d11_rate_control *)
{
    return PWD3D11_ERROR_NOT_IMPLEMENTED;
}

pwd3d11_result pwd3d11_encoder_compute_num_packets(pwd3d11_encoder, size_t, size_t *)
{
    return PWD3D11_ERROR_NOT_IMPLEMENTED;
}

pwd3d11_result pwd3d11_encoder_packetize(pwd3d11_encoder, pwd3d11_packet *, size_t, size_t *, void *, size_t)
{
    return PWD3D11_ERROR_NOT_IMPLEMENTED;
}

void pwd3d11_encoder_destroy(pwd3d11_encoder enc)
{
    if (enc) {
        delete enc;
    }
}

pwd3d11_result pwd3d11_encoder_get_timings(pwd3d11_encoder, float *, size_t)
{
    return PWD3D11_ERROR_NOT_IMPLEMENTED;
}
