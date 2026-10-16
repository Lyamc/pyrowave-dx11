// c_api.cpp -- C ABI entry points for pyrowave_d3d11.dll
// Stubs for encoder functions (T5b fills these in).
// Debug infrastructure for T6c (dump hooks + pwd3d11_debug_run_stage).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "pyrowave_d3d11.h"
#include "debug_dump.h"

using Microsoft::WRL::ComPtr;

// Forward declarations (defined in dwt.cpp and quant.cpp)
struct DWTState;
struct QuantState;

// Internal encoder state (stub)
struct pwd3d11_encoder_t {
    pwd3d11_device device;
    int width;
    int height;
    pwd3d11_chroma_subsampling chroma;
    char dump_dir[512];  // debug dump directory (T6c)

    // Stage states (T1+T2b)
    DWTState *dwt = nullptr;
    QuantState *quant = nullptr;
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

// ------------------------------------------------------------------
// Debug API -- T6c: pwd3d11_debug_run_stage + dump dir (VALIDATION §3.4, §5)
// ------------------------------------------------------------------

void pwd3d11_debug_set_dump_dir(pwd3d11_encoder enc, const char *dump_dir)
{
    if (!enc) return;
    if (!dump_dir) {
        enc->dump_dir[0] = '\0';
        return;
    }
    strncpy_s(enc->dump_dir, sizeof(enc->dump_dir), dump_dir, _TRUNCATE);
}

pwd3d11_result pwd3d11_debug_run_stage(pwd3d11_encoder enc, pwd3d11_stage stage,
                                        const pwd3d11_debug_buffer *input_buffers,
                                        size_t num_input_buffers,
                                        pwd3d11_debug_buffer *output_buffers,
                                        size_t num_output_buffers)
{
    if (!enc) return PWD3D11_ERROR_INVALID_ARGUMENT;
    (void)input_buffers; (void)num_input_buffers;
    (void)output_buffers; (void)num_output_buffers;

    switch (stage) {
    case PWD3D11_STAGE_DWT:
        // DWT is stage 1: takes raw YUV input, produces band textures.
        // For M3 mode, input_buffers would be the raw YUV plane data.
        // output_buffers receive the band outputs (read back from GPU).
        // TODO (T5b+): full implementation with DWTState integration.
        // For now, stub: if dump_dir is set, record the call.
        if (enc->dump_dir[0]) {
            // Write a minimal manifest indicating DWT stage was "run"
            const char *names[] = {"stub_dwt_output"};
            int stages[] = {1};
            int dtypes[] = {0};
            int sw[] = {0}, sh[] = {0};
            debug_write_manifest(enc->dump_dir, "stub", enc->width, enc->height, 0,
                                 "D3D11-debug", names, stages, dtypes, sw, sh, 1);
        }
        return PWD3D11_ERROR_NOT_IMPLEMENTED;

    case PWD3D11_STAGE_QUANT:
    case PWD3D11_STAGE_ANALYZE:
    case PWD3D11_STAGE_RESOLVE:
    case PWD3D11_STAGE_PACK:
    case PWD3D11_STAGE_PACKETIZE:
        return PWD3D11_ERROR_NOT_IMPLEMENTED;

    default:
        return PWD3D11_ERROR_INVALID_ARGUMENT;
    }
}
