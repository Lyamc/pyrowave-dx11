// frame.cpp -- Frame orchestration: dispatch pipeline, readback, packetize (T5b)

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
#include "stage_funcs.h"
#include "shaders/pack_dxbc.h"

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------------
// Push constants for pack shader (BlockPackingPushData, 48 bytes)
// ------------------------------------------------------------------
struct PackPushConstants {
    int   resolution_x;
    int   resolution_y;
    int   resolution_32x32_blocks_x;
    int   resolution_32x32_blocks_y;
    int   resolution_8x8_blocks_x;
    int   resolution_8x8_blocks_y;
    unsigned int quant_resolution_code;
    unsigned int seq_code;
    int   block_offset_32x32;
    int   block_stride_32x32;
    int   block_offset_8x8;
    int   block_stride_8x8;
};
static_assert(sizeof(PackPushConstants) == 48, "PackPushConstants must be 48 bytes");

// ------------------------------------------------------------------
// Encoder state (full pipeline)
// ------------------------------------------------------------------
struct pwd3d11_encoder_t {
    pwd3d11_device device;
    ComPtr<ID3D11Device> d3d11_device;
    ComPtr<ID3D11DeviceContext> context;
    int width;
    int height;
    pwd3d11_chroma_subsampling chroma;
    char dump_dir[512];

    // Stage states
    DWTState *dwt = nullptr;
    QuantState *quant = nullptr;
    AnalyzeState *analyze = nullptr;
    ResolveState *resolve = nullptr;
    ComPtr<ID3D11ComputeShader> pack_shader;

    // Frame-level resources
    ComPtr<ID3D11Buffer> bitstream_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_bitstream;
    ComPtr<ID3D11Buffer> payload_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_payload;
    ComPtr<ID3D11Buffer> meta_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_meta;

    // Bitstream size estimate
    size_t bitstream_size = 0;
    size_t meta_size = 0;

    // Sequence code (incremented per frame, masked to 3 bits)
    unsigned int sequence_code;

    // Timings
    float timings[10];
    size_t timing_count = 0;
};

// ------------------------------------------------------------------
// Helper: compute num blocks and per_sub for a frame
// ------------------------------------------------------------------
static void compute_frame_params(int W, int H, int /*chroma*/,
                                  int *out_total_blocks, int *out_per_sub)
{
    int aw = (W + 31) & ~31;
    int ah = (H + 31) & ~31;
    if (aw < 128) aw = 128;
    if (ah < 128) ah = 128;

    // 32x32 blocks
    int blocks_32x32_x = (aw + 31) / 32;
    int blocks_32x32_y = (ah + 31) / 32;
    int total_blocks = blocks_32x32_x * blocks_32x32_y;

    // per_sub = next_pow2(align(total_blocks, 16) / 16)
    int aligned_blocks = (total_blocks + 15) & ~15;
    int per_sub = aligned_blocks / 16;
    while (per_sub & (per_sub - 1)) per_sub &= per_sub - 1;
    if (per_sub < 1) per_sub = 1;

    *out_total_blocks = total_blocks;
    *out_per_sub = per_sub;
}

// ------------------------------------------------------------------
// Create encoder
// ------------------------------------------------------------------
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
    enc->dump_dir[0] = '\0';
    enc->sequence_code = 1u;
    memset(enc->timings, 0, sizeof(enc->timings));

    // Get D3D11 device and context
    enc->d3d11_device = static_cast<ID3D11Device*>(pwd3d11_device_get_d3d11_device(info->device));
    enc->context = static_cast<ID3D11DeviceContext*>(pwd3d11_device_get_d3d11_context(info->device));
    if (!enc->d3d11_device) {
        delete enc;
        *out = nullptr;
        return PWD3D11_ERROR_INVALID_ARGUMENT;
    }

    // Create DWT state
    enc->dwt = new DWTState();
    pwd3d11_result hr = dwt_create(enc->dwt, enc->d3d11_device.Get(),
                                     info->width, info->height, info->chroma);
    if (hr != PWD3D11_SUCCESS) {
        delete enc->dwt;
        delete enc;
        *out = nullptr;
        return hr;
    }

    // Create Quant state
    enc->quant = new QuantState();
    hr = quant_create(enc->quant, enc->d3d11_device.Get(),
                       info->width, info->height, info->chroma);
    if (hr != PWD3D11_SUCCESS) {
        dwt_destroy(enc->dwt);
        delete enc->quant;
        delete enc->dwt;
        delete enc;
        *out = nullptr;
        return hr;
    }

    // Compute frame params
    int total_blocks, per_sub;
    compute_frame_params(info->width, info->height, info->chroma, &total_blocks, &per_sub);

    // Create Analyze state
    enc->analyze = new AnalyzeState();
    hr = analyze_create(enc->analyze, enc->d3d11_device.Get(),
                         info->width, info->height, info->chroma);
    if (hr != PWD3D11_SUCCESS) {
        quant_destroy(enc->quant);
        dwt_destroy(enc->dwt);
        delete enc->analyze;
        delete enc->quant;
        delete enc->dwt;
        delete enc;
        *out = nullptr;
        return hr;
    }

    // Create Resolve state
    enc->resolve = new ResolveState();
    hr = resolve_create(enc->resolve, enc->d3d11_device.Get(), per_sub);
    if (hr != PWD3D11_SUCCESS) {
        analyze_destroy(enc->analyze);
        quant_destroy(enc->quant);
        dwt_destroy(enc->dwt);
        delete enc->resolve;
        delete enc->analyze;
        delete enc->quant;
        delete enc->dwt;
        delete enc;
        *out = nullptr;
        return hr;
    }

    // Compile pack shader
    HRESULT hr_d3d = enc->d3d11_device->CreateComputeShader(
        const_cast<unsigned char*>(g_pack_dxbc),
        g_pack_dxbc_len,
        nullptr,
        &enc->pack_shader
    );
    if (!SUCCEEDED(hr_d3d)) {
        resolve_destroy(enc->resolve);
        analyze_destroy(enc->analyze);
        quant_destroy(enc->quant);
        dwt_destroy(enc->dwt);
        delete enc->resolve;
        delete enc->analyze;
        delete enc->quant;
        delete enc->dwt;
        delete enc;
        *out = nullptr;
        return PWD3D11_ERROR_GENERIC;
    }

    *out = enc;
    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Encode a frame: dispatch pipeline, readback, packetize
// ------------------------------------------------------------------
pwd3d11_result pwd3d11_encoder_encode_cpu(pwd3d11_encoder enc,
                                           const pwd3d11_cpu_buffer *buf,
                                           const pwd3d11_rate_control *rc)
{
    if (!enc || !buf || !rc) return PWD3D11_ERROR_INVALID_ARGUMENT;
    (void)enc->chroma;  // Suppress unreferenced warning

    auto ctx = enc->context;
    if (!ctx) return PWD3D11_ERROR_INVALID_ARGUMENT;
    HRESULT hr_d3d;

    // Step 1: Upload planes and run DWT
    pwd3d11_result hr = dwt_upload_planes(enc->dwt, buf);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = dwt_create_bands(enc->dwt);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = dwt_compile_shader(enc->dwt);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = dwt_run(enc->dwt);
    if (hr != PWD3D11_SUCCESS) return hr;

    // Step 2: Set up quant with band SRVs
    std::vector<ID3D11ShaderResourceView*> band_srvs;
    int num_bands = 0;

    // Collect band SRVs in quant loop order
    // L0: Y
    // L1: Cb, Cr, Y
    // L2..4: Y, Cb, Cr (if applicable)
    int aw = (enc->width + 31) & ~31;
    int ah = (enc->height + 31) & ~31;
    if (aw < 128) aw = 128;
    if (ah < 128) ah = 128;

    for (int l = 0; l < 5; l++) {
        for (int c = 0; c < 3; c++) {
            if (l == 0 && c > 0) continue;  // L0 only has Y
            if (l >= 1 && c == 0 && enc->chroma == 0) continue;  // 4:2:0 chroma starts at L1

            int bw = dwt_get_band_w(enc->dwt, c, l);
            int bh = dwt_get_band_h(enc->dwt, c, l);
            if (bw <= 0 || bh <= 0) continue;

            ID3D11ShaderResourceView *srv = dwt_get_band_srv(enc->dwt, c, l);
            if (!srv) continue;

            band_srvs.push_back(srv);
            num_bands++;
        }
    }

    hr = quant_set_bands(enc->quant, band_srvs.data(), num_bands);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = quant_create_outputs(enc->quant);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = quant_compile_shader(enc->quant);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = quant_run(enc->quant);
    if (hr != PWD3D11_SUCCESS) return hr;

    // Step 3: Set up analyze with quant output
    int total_blocks, per_sub;
    compute_frame_params(enc->width, enc->height, enc->chroma, &total_blocks, &per_sub);

    // Get quant output buffers
    // quant.cpp stores uav_meta and uav_stats in QuantState
    // We need to create an SRV for the stats buffer
    // For now, use the meta buffer as stats (they have the same layout for our purposes)
    hr = analyze_set_stats(enc->analyze, nullptr, total_blocks);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = analyze_create_buffers(enc->analyze);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = analyze_compile_shaders(enc->analyze);
    if (hr != PWD3D11_SUCCESS) return hr;

    // Run analyze per band
    // For simplicity, run on the full frame dimensions
    int bw_32 = (aw + 31) / 32;
    (void)bw_32;
    hr = analyze_run(enc->analyze, nullptr,
                      aw, ah, 0, bw_32, 0, bw_32);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = analyze_finalize_run(enc->analyze);
    if (hr != PWD3D11_SUCCESS) return hr;

    // Step 4: Set up resolve
    // Create SRV for bucket buffer
    // The bucket buffer is created in analyze_create_buffers as a UAV
    // We need to create an SRV for it to use in resolve
    // For now, skip resolve (stub)

    // Step 5: Allocate bitstream buffer
    size_t target_payload = rc->maximum_bitstream_size;
    enc->meta_size = total_blocks * 8;  // 8 bytes per BitstreamPacket
    enc->bitstream_size = target_payload + enc->meta_size;
    enc->bitstream_size = (enc->bitstream_size + 3u) & ~3u;  // Word-aligned

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = UINT(enc->bitstream_size);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    hr_d3d = enc->d3d11_device->CreateBuffer(&bd, nullptr, &enc->bitstream_buf);
    if (!SUCCEEDED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R32_UINT;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = UINT(enc->bitstream_size / 4);
    hr_d3d = enc->d3d11_device->CreateUnorderedAccessView(enc->bitstream_buf.Get(), &ud, &enc->uav_bitstream);
    if (!SUCCEEDED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    // Create meta buffer
    bd.ByteWidth = UINT(enc->meta_size);
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    hr_d3d = enc->d3d11_device->CreateBuffer(&bd, nullptr, &enc->meta_buf);
    if (!SUCCEEDED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    ud.Buffer.NumElements = UINT(enc->meta_size / 4);
    hr_d3d = enc->d3d11_device->CreateUnorderedAccessView(enc->meta_buf.Get(), &ud, &enc->uav_meta);
    if (!SUCCEEDED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    // Step 6: Dispatch pack shader per band
    // For now, skip actual packing (needs resolve output)

    // Step 7: Readback bitstream
    // Create staging buffer for readback
    D3D11_BUFFER_DESC bd_staging = {};
    bd_staging.ByteWidth = UINT(enc->bitstream_size);
    bd_staging.Usage = D3D11_USAGE_STAGING;
    bd_staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bd_staging.BindFlags = 0;

    ComPtr<ID3D11Buffer> staging_buf;
    hr_d3d = enc->d3d11_device->CreateBuffer(&bd_staging, nullptr, &staging_buf);
    if (!SUCCEEDED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    ctx->CopyResource(staging_buf.Get(), enc->bitstream_buf.Get());
    ctx->Flush();

    // Map and read back
    D3D11_MAPPED_SUBRESOURCE ms;
    hr_d3d = ctx->Map(staging_buf.Get(), 0, D3D11_MAP_READ, 0, &ms);
    if (!SUCCEEDED(hr_d3d)) {
        ctx->Unmap(staging_buf.Get(), 0);
        return PWD3D11_ERROR_GENERIC;
    }

    // Copy to host memory (caller will provide buffer via packetize)
    // For now, just unmap
    ctx->Unmap(staging_buf.Get(), 0);

    // Increment sequence code
    enc->sequence_code = (enc->sequence_code + 1u) & 7u;
    if (enc->sequence_code == 0u) enc->sequence_code = 1u;

    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Compute number of packets
// ------------------------------------------------------------------
pwd3d11_result pwd3d11_encoder_compute_num_packets(pwd3d11_encoder enc, size_t /*packet_boundary*/,
                                                    size_t *num_packets)
{
    if (!enc || !num_packets) return PWD3D11_ERROR_INVALID_ARGUMENT;

    // Parse meta buffer to count packets
    // For now, return a placeholder
    *num_packets = 1;
    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Packetize: parse meta and copy bitstream to output
// ------------------------------------------------------------------
pwd3d11_result pwd3d11_encoder_packetize(pwd3d11_encoder enc, pwd3d11_packet *packets,
                                          size_t /*packet_boundary*/, size_t *num_packets,
                                          void * /*bitstream*/, size_t size)
{
    if (!enc || !num_packets) return PWD3D11_ERROR_INVALID_ARGUMENT;

    // Parse meta buffer to create packet structures
    // For now, return a placeholder
    *num_packets = 1;
    if (packets) {
        packets[0].offset = 0;
        packets[0].size = size;
    }

    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Destroy encoder
// ------------------------------------------------------------------
void pwd3d11_encoder_destroy(pwd3d11_encoder enc)
{
    if (enc) {
        if (enc->context) enc->context->Flush();
        resolve_destroy(enc->resolve);
        analyze_destroy(enc->analyze);
        quant_destroy(enc->quant);
        dwt_destroy(enc->dwt);
        delete enc->resolve;
        delete enc->analyze;
        delete enc->quant;
        delete enc->dwt;
        delete enc;
    }
}

// ------------------------------------------------------------------
// Get timings
// ------------------------------------------------------------------
pwd3d11_result pwd3d11_encoder_get_timings(pwd3d11_encoder enc, float *us, size_t count)
{
    if (!enc || !us || count == 0) return PWD3D11_ERROR_INVALID_ARGUMENT;
    size_t n = enc->timing_count < count ? enc->timing_count : count;
    memcpy(us, enc->timings, n * sizeof(float));
    return PWD3D11_SUCCESS;
}
