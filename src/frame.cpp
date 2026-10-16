// frame.cpp -- Frame orchestration: dispatch pipeline, readback, packetize (T5b)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdio>

#include "pyrowave_d3d11.h"
#include "stage_funcs.h"
#include "stage_internal.h"
#include "shaders/pack_dxbc.h"
#include "debug_dump.h"

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
// Helper: read a D3D11 buffer to host memory (for debug dumps)
// ------------------------------------------------------------------
static std::vector<uint8_t> readback_buffer(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                                            ID3D11Buffer *buf, size_t size)
{
    if (!buf || size == 0) return {};

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = UINT(size);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bd.BindFlags = 0;

    ComPtr<ID3D11Buffer> staging;
    HRESULT hr = dev->CreateBuffer(&bd, nullptr, &staging);
    if (FAILED(hr)) return {};

    ctx->CopyResource(staging.Get(), buf);
    ctx->Flush();

    D3D11_MAPPED_SUBRESOURCE ms;
    hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &ms);
    if (FAILED(hr)) return {};

    std::vector<uint8_t> result(static_cast<uint8_t*>(ms.pData),
                                static_cast<uint8_t*>(ms.pData) + size);
    ctx->Unmap(staging.Get(), 0);
    return result;
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
    pwd3d11_result hr = dwt_create(enc->dwt, enc->device,
                                     info->width, info->height, info->chroma);
    if (hr != PWD3D11_SUCCESS) {
        delete enc->dwt;
        delete enc;
        *out = nullptr;
        return hr;
    }

    // Create Quant state
    enc->quant = new QuantState();
    hr = quant_create(enc->quant, enc->device,
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

    // DWT debug dump: skip band textures (requires texture readback, less critical)

    // Step 2: Set up quant with band SRVs
    std::vector<ID3D11ShaderResourceView*> band_srvs;
    std::vector<int> band_w, band_h;
    int num_bands = 0;

    int aw = (enc->width + 31) & ~31;
    int ah = (enc->height + 31) & ~31;
    if (aw < 128) aw = 128;
    if (ah < 128) ah = 128;

    for (int l = 0; l < 5; l++) {
        for (int c = 0; c < 3; c++) {
            if (l == 0 && c > 0) continue;
            if (l >= 1 && c == 0 && enc->chroma == 0) continue;

            int bw = dwt_get_band_w(enc->dwt, c, l);
            int bh = dwt_get_band_h(enc->dwt, c, l);
            if (bw <= 0 || bh <= 0) continue;

            ID3D11ShaderResourceView *srv = dwt_get_band_srv(enc->dwt, c, l);
            if (!srv) continue;

            band_srvs.push_back(srv);
            band_w.push_back(bw);
            band_h.push_back(bh);
            num_bands++;
        }
    }

    hr = quant_set_bands(enc->quant, band_srvs.data(), band_w.data(), band_h.data(), num_bands);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = quant_create_outputs(enc->quant);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = quant_compile_shader(enc->quant);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = quant_run(enc->quant);
    if (hr != PWD3D11_SUCCESS) return hr;

    // Quant debug dump: meta, stats, payload
    if (enc->dump_dir[0]) {
        auto ctx_q = enc->context;
        size_t meta_size = enc->quant->total_blocks * 8;
        size_t stats_size = enc->quant->total_blocks * 64;
        size_t payload_size = enc->quant->payload_size;

        auto meta_data = readback_buffer(enc->d3d11_device.Get(), ctx_q.Get(),
                                         enc->quant->meta_buf.Get(), meta_size);
        if (!meta_data.empty()) {
            debug_dump_buffer(enc->dump_dir, "meta_buffer", 2, 0,
                              enc->quant->total_blocks * 2, 1,
                              meta_data.data(), meta_data.size());
        }

        auto stats_data = readback_buffer(enc->d3d11_device.Get(), ctx_q.Get(),
                                          enc->quant->stats_buf.Get(), stats_size);
        if (!stats_data.empty()) {
            debug_dump_buffer(enc->dump_dir, "block_stat_buffer", 2, 0,
                              enc->quant->total_blocks * 16, 1,
                              stats_data.data(), stats_data.size());
        }

        auto payload_data = readback_buffer(enc->d3d11_device.Get(), ctx_q.Get(),
                                            enc->quant->payload_buf.Get(), payload_size);
        if (!payload_data.empty()) {
            debug_dump_buffer(enc->dump_dir, "payload_data", 2, 0,
                              UINT(payload_size / 4), 1,
                              payload_data.data(), payload_data.size());
        }
    }

    // Step 3: Set up analyze with quant stats output
    int total_blocks, per_sub;
    compute_frame_params(enc->width, enc->height, enc->chroma, &total_blocks, &per_sub);

    hr = analyze_set_stats(enc->analyze, enc->quant->stats_buf.Get(), total_blocks);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = analyze_create_buffers(enc->analyze);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = analyze_compile_shaders(enc->analyze);
    if (hr != PWD3D11_SUCCESS) return hr;

    int bw_32 = (aw + 31) / 32;
    hr = analyze_run(enc->analyze,
                      aw, ah, 0, bw_32, 0, bw_32);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = analyze_finalize_run(enc->analyze);
    if (hr != PWD3D11_SUCCESS) return hr;

    // Analyze debug dump: bucket buffer
    if (enc->dump_dir[0]) {
        auto ctx_a = enc->context;
        auto bucket_data = readback_buffer(enc->d3d11_device.Get(), ctx_a.Get(),
                                           enc->analyze->bucket_buf.Get(),
                                           enc->analyze->bucket_size);
        if (!bucket_data.empty()) {
            debug_dump_buffer(enc->dump_dir, "bucket_buffer", 3, 0,
                              UINT(enc->analyze->bucket_size / 4), 1,
                              bucket_data.data(), bucket_data.size());
        }
    }

    // Step 4: Set up resolve
    D3D11_SHADER_RESOURCE_VIEW_DESC sd_bucket = {};
    sd_bucket.Format = DXGI_FORMAT_R32_UINT;
    sd_bucket.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd_bucket.Buffer.NumElements = UINT(enc->analyze->bucket_size / 4);

    ComPtr<ID3D11ShaderResourceView> srv_bucket;
    hr_d3d = enc->d3d11_device->CreateShaderResourceView(enc->analyze->bucket_buf.Get(), &sd_bucket, &srv_bucket);
    if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    hr = resolve_compile_shaders(enc->resolve);
    if (hr != PWD3D11_SUCCESS) return hr;

    unsigned int target_payload = (unsigned int)(rc->maximum_bitstream_size);
    hr = resolve_run(enc->resolve, srv_bucket.Get(), enc->analyze->uav_quant.Get(), target_payload);
    if (hr != PWD3D11_SUCCESS) return hr;

    // Resolve debug dump: quant buffer
    if (enc->dump_dir[0]) {
        auto ctx_r = enc->context;
        auto quant_data = readback_buffer(enc->d3d11_device.Get(), ctx_r.Get(),
                                          enc->analyze->quant_buf.Get(),
                                          size_t(total_blocks) * 4);
        if (!quant_data.empty()) {
            debug_dump_buffer(enc->dump_dir, "quant_buffer", 4, 1,
                              total_blocks, 1,
                              quant_data.data(), quant_data.size());
        }
    }

    // Step 5: Allocate bitstream and meta buffers
    enc->meta_size = total_blocks * 8;
    enc->bitstream_size = rc->maximum_bitstream_size + enc->meta_size;
    enc->bitstream_size = (enc->bitstream_size + 3u) & ~3u;

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = UINT(enc->bitstream_size);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    hr_d3d = enc->d3d11_device->CreateBuffer(&bd, nullptr, &enc->bitstream_buf);
    if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R32_UINT;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = UINT(enc->bitstream_size / 4);
    hr_d3d = enc->d3d11_device->CreateUnorderedAccessView(enc->bitstream_buf.Get(), &ud, &enc->uav_bitstream);
    if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    bd.ByteWidth = UINT(enc->meta_size);
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    hr_d3d = enc->d3d11_device->CreateBuffer(&bd, nullptr, &enc->meta_buf);
    if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    ud.Buffer.NumElements = UINT(enc->meta_size / 4);
    hr_d3d = enc->d3d11_device->CreateUnorderedAccessView(enc->meta_buf.Get(), &ud, &enc->uav_meta);
    if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    // Step 6: Dispatch pack shader per 32x32 block
    int blocks_32x32_x = (aw + 31) / 32;
    int blocks_32x32_y = (ah + 31) / 32;

    // Create SRVs for pack shader inputs
    D3D11_SHADER_RESOURCE_VIEW_DESC sd_meta = {};
    sd_meta.Format = DXGI_FORMAT_R32_UINT;
    sd_meta.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd_meta.Buffer.NumElements = UINT(total_blocks * 2);

    ComPtr<ID3D11ShaderResourceView> srv_meta;
    hr_d3d = enc->d3d11_device->CreateShaderResourceView(enc->quant->meta_buf.Get(), &sd_meta, &srv_meta);
    if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd_stats = {};
    sd_stats.Format = DXGI_FORMAT_R32_UINT;
    sd_stats.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd_stats.Buffer.NumElements = UINT(total_blocks * 16);

    ComPtr<ID3D11ShaderResourceView> srv_stats;
    hr_d3d = enc->d3d11_device->CreateShaderResourceView(enc->quant->stats_buf.Get(), &sd_stats, &srv_stats);
    if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd_quant = {};
    sd_quant.Format = DXGI_FORMAT_R32_SINT;
    sd_quant.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd_quant.Buffer.NumElements = total_blocks;

    ComPtr<ID3D11ShaderResourceView> srv_quant;
    hr_d3d = enc->d3d11_device->CreateShaderResourceView(enc->analyze->quant_buf.Get(), &sd_quant, &srv_quant);
    if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    // Dispatch pack: one workgroup of 64 threads per 32x32 block
    for (int by = 0; by < blocks_32x32_y; by += 2) {
        for (int bx = 0; bx < blocks_32x32_x; bx += 2) {
            int blocks_x = std::min(2, blocks_32x32_x - bx);
            int blocks_y = std::min(2, blocks_32x32_y - by);

            PackPushConstants pc = {};
            pc.resolution_x = aw;
            pc.resolution_y = ah;
            pc.resolution_32x32_blocks_x = blocks_32x32_x;
            pc.resolution_32x32_blocks_y = blocks_32x32_y;
            pc.resolution_8x8_blocks_x = (aw + 7) / 8;
            pc.resolution_8x8_blocks_y = (ah + 7) / 8;
            pc.quant_resolution_code = 0;
            pc.seq_code = enc->sequence_code;
            pc.block_offset_32x32 = bx;
            pc.block_stride_32x32 = blocks_32x32_x;
            pc.block_offset_8x8 = bx * 4;
            pc.block_stride_8x8 = (aw + 7) / 8;

            ComPtr<ID3D11Buffer> cb;
            D3D11_BUFFER_DESC cbd = {};
            cbd.ByteWidth = sizeof(PackPushConstants);
            cbd.Usage = D3D11_USAGE_IMMUTABLE;
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA csd = { &pc, 0, 0 };
            hr_d3d = enc->d3d11_device->CreateBuffer(&cbd, &csd, &cb);
            if (FAILED(hr_d3d)) return PWD3D11_ERROR_GENERIC;

            ID3D11ComputeShader *pack_cs = enc->pack_shader.Get();
            ID3D11ShaderResourceView *srvs[3] = { srv_meta.Get(), srv_stats.Get(), srv_quant.Get() };
            ID3D11UnorderedAccessView *uavs[3] = { enc->uav_bitstream.Get(), enc->uav_meta.Get(), enc->quant->uav_payload.Get() };

            ctx->CSSetShader(pack_cs, nullptr, 0);
            ctx->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
            ctx->CSSetShaderResources(0, 3, srvs);
            ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
            ctx->Dispatch(blocks_x * blocks_y * 4, 1, 1);

            // Clear bindings
            ID3D11ShaderResourceView *nullsrv = nullptr;
            ID3D11UnorderedAccessView *nulluav = nullptr;
            ctx->CSSetShaderResources(0, 3, &nullsrv);
            ctx->CSSetUnorderedAccessViews(0, 3, &nulluav, nullptr);
        }
    }

    // Step 7: Readback bitstream
    D3D11_BUFFER_DESC bd_staging = {};
    bd_staging.ByteWidth = UINT(enc->bitstream_size);
    bd_staging.Usage = D3D11_USAGE_STAGING;
    bd_staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bd_staging.BindFlags = 0;

    ComPtr<ID3D11Buffer> staging_buf;
    hr_d3d = enc->d3d11_device->CreateBuffer(&bd_staging, nullptr, &staging_buf);
    if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    ctx->CopyResource(staging_buf.Get(), enc->bitstream_buf.Get());
    ctx->Flush();

    D3D11_MAPPED_SUBRESOURCE ms;
    hr_d3d = ctx->Map(staging_buf.Get(), 0, D3D11_MAP_READ, 0, &ms);
    if (FAILED(hr_d3d)) {
        ctx->Unmap(staging_buf.Get(), 0);
        return PWD3D11_ERROR_GENERIC;
    }

    enc->host_bitstream.assign(static_cast<uint8_t*>(ms.pData),
                               static_cast<uint8_t*>(ms.pData) + enc->bitstream_size);

    ctx->Unmap(staging_buf.Get(), 0);

    // Pack/Packetize debug dump: bitstream and meta
    if (enc->dump_dir[0]) {
        debug_dump_buffer(enc->dump_dir, "bitstream", 5, 1,
                          UINT(enc->bitstream_size / 4), 1,
                          enc->host_bitstream.data(), enc->bitstream_size);

        auto ctx_p = enc->context;
        auto meta_data = readback_buffer(enc->d3d11_device.Get(), ctx_p.Get(),
                                         enc->meta_buf.Get(), enc->meta_size);
        if (!meta_data.empty()) {
            debug_dump_buffer(enc->dump_dir, "meta_out", 5, 0,
                              UINT(enc->meta_size / 4), 1,
                              meta_data.data(), meta_data.size());
        }
    }

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
