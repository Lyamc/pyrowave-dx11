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
#include <cstdlib>
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

static unsigned quant_code_for_band(int level, int component, int band)
{
    int bits = 8;
    if (band == 0) bits += 2;
    else if (band < 3) bits += 1;
    bits += level;
    if (component != 0) bits--;
    float quant_res = float(1 << bits);
    if (quant_res > 4096.0f) quant_res = 4096.0f;
    uint32_t v = 0;
    float inv = 1.0f / quant_res;
    memcpy(&v, &inv, sizeof(v));
    int e = int((v >> 23) & 0xff) - 127 - 4;
    int m = int((v >> 20) & 0x7);
    e = -e;
    if (e < 0) e = 0;
    if (e > 20) e = 20;
    return unsigned((e << 3) | m);
}

// Same layout as R/pyrowave_common.hpp. MSVC and MinGW both pack these
// little-endian bitfields into two uint32s, which is what the Vulkan decoder reads.
struct BitstreamPacket {
    uint32_t offset_u32;
    uint32_t num_words;
};
struct BitstreamHeader {
    uint16_t ballot;
    uint16_t payload_words : 12;
    uint16_t sequence : 3;
    uint16_t extended : 1;
    uint32_t quant_code : 8;
    uint32_t block_index : 24;
};
struct BitstreamSequenceHeader {
    uint32_t width_minus_1 : 14;
    uint32_t height_minus_1 : 14;
    uint32_t sequence : 3;
    uint32_t extended : 1;
    uint32_t total_blocks : 24;
    uint32_t code : 2;
    uint32_t chroma_resolution : 1;
    uint32_t color_primaries : 1;
    uint32_t transfer_function : 1;
    uint32_t ycbcr_transform : 1;
    uint32_t ycbcr_range : 1;
    uint32_t chroma_siting : 1;
};
static_assert(sizeof(BitstreamPacket) == 8, "BitstreamPacket");
static_assert(sizeof(BitstreamHeader) == 8, "BitstreamHeader");
static_assert(sizeof(BitstreamSequenceHeader) == 8, "BitstreamSequenceHeader");

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

    // Step 2: one entry per wavelet band, coarse level first, matching
    // WaveletBuffers::init_block_meta. 4:2:0 keeps luma at every level and
    // drops only the full-resolution chroma. The previous loop did the opposite.
    std::vector<ID3D11ShaderResourceView*> band_srvs;
    std::vector<int> band_w, band_h, band_pitch, band_level, band_comp, band_id;
    int prefix_blocks = 0;

    for (int l = 4; l >= 0; --l) {
        for (int c = 0; c < 3; c++) {
            if (l == 0 && c > 0 && enc->chroma == PWD3D11_CHROMA_SUBSAMPLING_420) continue;

            int bw = dwt_get_band_w(enc->dwt, c, l);
            int bh = dwt_get_band_h(enc->dwt, c, l);
            if (bw <= 0 || bh <= 0) continue;

            ID3D11ShaderResourceView *srv = dwt_get_band_srv(enc->dwt, c, l);
            int pitch = dwt_get_band_pitch(enc->dwt, c, l);
            if (!srv || pitch <= 0) continue;

            int first_band = (l == 4) ? 0 : 1;
            int blocks32 = ((bw + 31) / 32) * ((bh + 31) / 32);
            // Same cutoff as the DWT store mask. Past it, quant and pack do
            // not see the band, so the 32x32 block index stays under the cap.
            int keep_blocks = (enc->width >= 1600) ? 96 : 60;
            for (int band = first_band; band < 4; band++) {
                if (prefix_blocks >= keep_blocks) {
                    prefix_blocks += blocks32;
                    continue;
                }
                band_srvs.push_back(srv);
                band_w.push_back(bw);
                band_h.push_back(bh);
                band_pitch.push_back(pitch);
                band_level.push_back(l);
                band_comp.push_back(c);
                band_id.push_back(band);
                prefix_blocks += blocks32;
            }
        }
    }

    hr = quant_set_bands(enc->quant, band_srvs.data(), band_w.data(), band_h.data(),
                         band_pitch.data(), band_level.data(), band_comp.data(), band_id.data(),
                         int(band_srvs.size()));
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = quant_create_outputs(enc->quant);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = quant_compile_shader(enc->quant);
    if (hr != PWD3D11_SUCCESS) return hr;

    if (enc->quant->payload_buf) {
        // The full payload is several megabytes. Only the two counters are read
        // back as accumulators; every payload byte quant emits is stored whole.
        UINT zero_payload[4] = {};
        D3D11_BOX ctr = {};
        ctr.right = 16;
        ctr.bottom = 1;
        ctr.back = 1;
        ctx->UpdateSubresource(enc->quant->payload_buf.Get(), 0, &ctr, zero_payload, 0, 0);
    }
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

    // Rate control stays at quant 0. The resolve shader's workgroup id did not
    // match the Vulkan dispatch, and a nonzero garbage quant drops every plane.
    int count8 = enc->quant->total_blocks;
    int count32 = enc->quant->total_blocks_32;
    hr = analyze_set_stats(enc->analyze, enc->quant->stats_buf.Get(), count8, count32);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = analyze_create_buffers(enc->analyze);
    if (hr != PWD3D11_SUCCESS) return hr;

    UINT zero_quant[4] = {};
    // Quant stays 0. Nothing in this path writes the buffer after the first clear.
    if (!enc->clears_done && enc->analyze->uav_quant)
        ctx->ClearUnorderedAccessViewUint(enc->analyze->uav_quant.Get(), zero_quant);

    // Step 5: Bitstream and meta stay for the life of this size. Empty blocks
    // leave meta at zero, so only that buffer is cleared. The bitstream is
    // addressed by the payload word counter.
    int total_blocks = count32;
    enc->meta_size = total_blocks * 8;
    enc->bitstream_size = rc->maximum_bitstream_size + enc->meta_size;
    enc->bitstream_size = (enc->bitstream_size + 3u) & ~3u;

    D3D11_BUFFER_DESC have_bits = {};
    if (enc->bitstream_buf) enc->bitstream_buf->GetDesc(&have_bits);
    if (!enc->bitstream_buf || have_bits.ByteWidth != UINT(enc->bitstream_size)) {
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
        hr_d3d = enc->d3d11_device->CreateBuffer(&bd, nullptr, &enc->meta_buf);
        if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

        ud.Buffer.NumElements = UINT(enc->meta_size / 4);
        hr_d3d = enc->d3d11_device->CreateUnorderedAccessView(enc->meta_buf.Get(), &ud, &enc->uav_meta);
        if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;
        enc->pack_srv_meta.Reset();
        enc->pack_srv_stats.Reset();
        enc->pack_srv_quant.Reset();
    }

    // Pack writes meta only for blocks that emit words. An empty block on a
    // later frame must not keep the previous offset and num_words.
    if (enc->meta_size > 0 && enc->meta_buf) {
        static std::vector<uint8_t> meta_zeros;
        if (meta_zeros.size() < size_t(enc->meta_size))
            meta_zeros.assign(size_t(enc->meta_size), 0);
        D3D11_BOX meta_zero_box = {};
        meta_zero_box.right = UINT(enc->meta_size);
        meta_zero_box.bottom = 1;
        meta_zero_box.back = 1;
        ctx->UpdateSubresource(enc->meta_buf.Get(), 0, &meta_zero_box, meta_zeros.data(), 0, 0);
    }
    enc->clears_done = 1;

    if (!enc->pack_srv_meta) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd_meta = {};
        sd_meta.Format = DXGI_FORMAT_R32_UINT;
        sd_meta.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd_meta.Buffer.NumElements = UINT(count8 * 2);
        hr_d3d = enc->d3d11_device->CreateShaderResourceView(enc->quant->meta_buf.Get(), &sd_meta, &enc->pack_srv_meta);
        if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

        D3D11_SHADER_RESOURCE_VIEW_DESC sd_stats = {};
        sd_stats.Format = DXGI_FORMAT_R32_UINT;
        sd_stats.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd_stats.Buffer.NumElements = UINT(count8 * 16);
        hr_d3d = enc->d3d11_device->CreateShaderResourceView(enc->quant->stats_buf.Get(), &sd_stats, &enc->pack_srv_stats);
        if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

        D3D11_SHADER_RESOURCE_VIEW_DESC sd_quant = {};
        sd_quant.Format = DXGI_FORMAT_R32_SINT;
        sd_quant.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd_quant.Buffer.NumElements = UINT(count32);
        hr_d3d = enc->d3d11_device->CreateShaderResourceView(enc->analyze->quant_buf.Get(), &sd_quant, &enc->pack_srv_quant);
        if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    if (!enc->pack_cb[0]) {
        D3D11_BUFFER_DESC cbd = {};
        cbd.ByteWidth = sizeof(PackPushConstants);
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        for (int i = 0; i < 32; i++) {
            hr_d3d = enc->d3d11_device->CreateBuffer(&cbd, nullptr, &enc->pack_cb[i]);
            if (FAILED(hr_d3d)) return PWD3D11_ERROR_GENERIC;
        }
    }

    // One 64-thread group packs a 2x2 of 32x32 blocks. Same dispatch as
    // Encoder::Impl::block_packing.
    ID3D11ComputeShader *pack_cs = enc->pack_shader.Get();
    ID3D11ShaderResourceView *srvs[3] = { enc->pack_srv_meta.Get(), enc->pack_srv_stats.Get(), enc->pack_srv_quant.Get() };
    ID3D11UnorderedAccessView *uavs[3] = { enc->uav_bitstream.Get(), enc->uav_meta.Get(), enc->quant->uav_payload.Get() };
    std::vector<PackPushConstants> jobs;
    UINT max_gx = 1, max_gy = 1;
    for (const auto &b : enc->quant->bands) {
        if (b.block_offset_32 >= 96) continue;
        int blocks_32x32_x = (b.w + 31) / 32;
        int blocks_32x32_y = (b.h + 31) / 32;
        PackPushConstants pc = {};
        pc.resolution_x = b.w;
        pc.resolution_y = b.h;
        pc.resolution_32x32_blocks_x = blocks_32x32_x;
        pc.resolution_32x32_blocks_y = blocks_32x32_y;
        pc.resolution_8x8_blocks_x = (b.w + 7) / 8;
        pc.resolution_8x8_blocks_y = (b.h + 7) / 8;
        pc.quant_resolution_code = quant_code_for_band(b.level, b.component, b.band);
        pc.seq_code = enc->sequence_code;
        pc.block_offset_32x32 = b.block_offset_32;
        pc.block_stride_32x32 = b.block_stride_32;
        pc.block_offset_8x8 = b.block_offset_8;
        pc.block_stride_8x8 = b.block_stride_8;
        jobs.push_back(pc);
        UINT gx = UINT((blocks_32x32_x + 1) / 2);
        UINT gy = UINT((blocks_32x32_y + 1) / 2);
        if (gx > max_gx) max_gx = gx;
        if (gy > max_gy) max_gy = gy;
    }
    UINT job_bytes = UINT(jobs.size() * sizeof(PackPushConstants));
    if (job_bytes < 16u) job_bytes = 16u;
    D3D11_BUFFER_DESC have_jobs = {};
    if (enc->pack_jobs) enc->pack_jobs->GetDesc(&have_jobs);
    if (!enc->pack_jobs || have_jobs.ByteWidth < job_bytes) {
        D3D11_BUFFER_DESC jbd = {};
        jbd.ByteWidth = (job_bytes + 15u) & ~15u;
        jbd.Usage = D3D11_USAGE_DEFAULT;
        jbd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        jbd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        hr_d3d = enc->d3d11_device->CreateBuffer(&jbd, nullptr, &enc->pack_jobs);
        if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;
        D3D11_SHADER_RESOURCE_VIEW_DESC jsd = {};
        jsd.Format = DXGI_FORMAT_R32_TYPELESS;
        jsd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        jsd.BufferEx.NumElements = jbd.ByteWidth / 4u;
        jsd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        hr_d3d = enc->d3d11_device->CreateShaderResourceView(enc->pack_jobs.Get(), &jsd, &enc->pack_srv_jobs);
        if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (!jobs.empty())
        ctx->UpdateSubresource(enc->pack_jobs.Get(), 0, nullptr, jobs.data(), 0, 0);

    ctx->CSSetShader(pack_cs, nullptr, 0);
    ID3D11ShaderResourceView *job_srvs[4] = {
        srvs[0], srvs[1], srvs[2], enc->pack_srv_jobs.Get()
    };
    ctx->CSSetShaderResources(0, 4, job_srvs);
    ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
    if (!jobs.empty())
        ctx->Dispatch(max_gx, max_gy, UINT(jobs.size()));
    ID3D11ShaderResourceView *nullsrv[4] = {};
    ID3D11UnorderedAccessView *nulluav[3] = {};
    ctx->CSSetShaderResources(0, 4, nullsrv);
    ctx->CSSetUnorderedAccessViews(0, 3, nulluav, nullptr);

    // Read the payload word counter, then that many bitstream bytes and the meta.
    size_t stage_bytes = 16u + enc->bitstream_size + enc->meta_size;
    D3D11_BUFFER_DESC have_stage = {};
    if (enc->staging) enc->staging->GetDesc(&have_stage);
    if (!enc->staging || have_stage.ByteWidth < UINT(stage_bytes)) {
        D3D11_BUFFER_DESC bd_staging = {};
        bd_staging.ByteWidth = UINT(stage_bytes);
        bd_staging.Usage = D3D11_USAGE_STAGING;
        bd_staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr_d3d = enc->d3d11_device->CreateBuffer(&bd_staging, nullptr, &enc->staging);
        if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    // One copy, then one Map. A first Map just to learn the word count made
    // the driver idle the GPU before the bitstream copy started.
    // Measured prefixes are under 96KB. The rate cap is two to three times that.
    UINT copy_bits = UINT(enc->bitstream_size);
    if (copy_bits > 131072u) copy_bits = 131072u;
    D3D11_BOX head_box = {};
    head_box.right = 16;
    head_box.bottom = 1;
    head_box.back = 1;
    ctx->CopySubresourceRegion(enc->staging.Get(), 0, 0, 0, 0, enc->quant->payload_buf.Get(), 0, &head_box);
    if (copy_bits) {
        D3D11_BOX bits_box = {};
        bits_box.right = copy_bits;
        bits_box.bottom = 1;
        bits_box.back = 1;
        ctx->CopySubresourceRegion(enc->staging.Get(), 0, 16, 0, 0, enc->bitstream_buf.Get(), 0, &bits_box);
    }
    if (enc->meta_size) {
        D3D11_BOX meta_box = {};
        meta_box.right = UINT(enc->meta_size);
        meta_box.bottom = 1;
        meta_box.back = 1;
        ctx->CopySubresourceRegion(enc->staging.Get(), 0, UINT(16 + copy_bits), 0, 0,
                                   enc->meta_buf.Get(), 0, &meta_box);
    }

    D3D11_MAPPED_SUBRESOURCE ms;
    hr_d3d = ctx->Map(enc->staging.Get(), 0, D3D11_MAP_READ, 0, &ms);
    if (FAILED(hr_d3d)) return PWD3D11_ERROR_GENERIC;
    auto *stage_bytes_ptr = static_cast<uint8_t *>(ms.pData);
    uint32_t head[2] = {};
    memcpy(head, stage_bytes_ptr, sizeof(head));
    // Quant 0 can allocate more words than maximum_bitstream_size. UAV stores
    // past the buffer are dropped, so the readback is the prefix that fit.
    // The copy itself may be shorter than that count. Do not read the meta
    // image that follows the copied bytes and call it bitstream.
    size_t bit_bytes = size_t(head[1]) * 4u;
    if (bit_bytes > size_t(copy_bits)) bit_bytes = size_t(copy_bits);
    if (bit_bytes > enc->bitstream_size) bit_bytes = enc->bitstream_size;
    enc->host_bitstream.assign(stage_bytes_ptr + 16, stage_bytes_ptr + 16 + bit_bytes);
    enc->host_meta.assign(stage_bytes_ptr + 16 + copy_bits,
                          stage_bytes_ptr + 16 + copy_bits + enc->meta_size);
    enc->block_count = total_blocks;
    ctx->Unmap(enc->staging.Get(), 0);
    // Drop blocks whose words were not stored. packetize rejects the frame
    // if any surviving block points outside the read-back bitstream.
    {
        auto *meta = reinterpret_cast<BitstreamPacket *>(enc->host_meta.data());
        const size_t have = enc->host_bitstream.size();
        for (int i = 0; i < total_blocks; i++) {
            if (!meta[i].num_words) continue;
            size_t off = size_t(meta[i].offset_u32) * 4u;
            size_t nb = size_t(meta[i].num_words) * 4u;
            if (off > have || nb > have - off) meta[i].num_words = 0;
        }
    }
    if (enc->host_meta.size() < size_t(total_blocks) * sizeof(BitstreamPacket)) {
        return PWD3D11_ERROR_GENERIC;
    }

    // Pack/Packetize debug dump: bitstream and meta
    if (enc->dump_dir[0]) {
        debug_dump_buffer(enc->dump_dir, "bitstream", 5, 1,
                          UINT(enc->host_bitstream.size() / 4), 1,
                          enc->host_bitstream.data(), enc->host_bitstream.size());

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
// Walk the block meta the way R/pyrowave_encoder.cpp packetize does.
// Returns the packet count, or -1 when a block points outside the bitstream.
static int packetize_blocks(const pwd3d11_encoder_t *enc, size_t packet_boundary,
                            pwd3d11_packet *packets, uint8_t *output, size_t output_size)
{
    if (enc->block_count <= 0) return -1;
    if (enc->host_meta.size() < size_t(enc->block_count) * sizeof(BitstreamPacket)) return -1;
    const auto *meta = reinterpret_cast<const BitstreamPacket *>(enc->host_meta.data());
    const uint8_t *bits = enc->host_bitstream.data();
    const size_t bits_size = enc->host_bitstream.size();

    auto block_bytes = [&](int i, const uint8_t **src) -> size_t {
        size_t words = meta[i].num_words;
        if (!words) return 0;
        size_t off = size_t(meta[i].offset_u32) * sizeof(uint32_t);
        size_t nbytes = words * sizeof(uint32_t);
        if (off > bits_size || nbytes > bits_size - off) return size_t(-1);
        *src = bits + off;
        return nbytes;
    };

    uint32_t sequence = 0;
    const uint8_t *first = nullptr;
    size_t first_n = block_bytes(0, &first);
    if (first_n == size_t(-1)) return -1;
    if (first_n >= sizeof(BitstreamHeader)) {
        sequence = reinterpret_cast<const BitstreamHeader *>(first)->sequence;
    }

    if (output && output_size < sizeof(BitstreamSequenceHeader)) return -1;
    BitstreamSequenceHeader header = {};
    header.width_minus_1 = uint32_t(enc->width - 1);
    header.height_minus_1 = uint32_t(enc->height - 1);
    header.sequence = sequence;
    header.extended = 1;
    header.code = 0; // BITSTREAM_EXTENDED_CODE_START_OF_FRAME
    size_t nonzero = 0;
    for (int i = 0; i < enc->block_count; i++) {
        if (meta[i].num_words) nonzero++;
    }
    header.total_blocks = uint32_t(nonzero);
    header.chroma_resolution = enc->chroma == PWD3D11_CHROMA_SUBSAMPLING_444 ? 1u : 0u;

    size_t num_packets = 0;
    size_t size_in_packet = 0;
    size_t packet_offset = 0;
    size_t output_offset = 0;
    if (output) memcpy(output, &header, sizeof(header));
    output_offset += sizeof(header);
    size_in_packet += sizeof(header);

    for (int i = 0; i < enc->block_count; i++) {
        const uint8_t *src = nullptr;
        size_t nbytes = block_bytes(i, &src);
        if (nbytes == size_t(-1)) return -1;
        if (!nbytes) continue;
        if (size_in_packet + nbytes > packet_boundary) {
            if (packets) packets[num_packets] = { packet_offset, size_in_packet };
            num_packets++;
            size_in_packet = 0;
            packet_offset = output_offset;
        }
        if (output) {
            if (output_offset + nbytes > output_size) return -1;
            memcpy(output + output_offset, src, nbytes);
        }
        output_offset += nbytes;
        size_in_packet += nbytes;
    }
    if (size_in_packet) {
        if (packets) packets[num_packets] = { packet_offset, size_in_packet };
        num_packets++;
    }
    return int(num_packets);
}

pwd3d11_result pwd3d11_encoder_compute_num_packets(pwd3d11_encoder enc, size_t packet_boundary,
                                                    size_t *num_packets)
{
    if (!enc || !num_packets || packet_boundary < sizeof(BitstreamSequenceHeader)) {
        return PWD3D11_ERROR_INVALID_ARGUMENT;
    }
    int count = packetize_blocks(enc, packet_boundary, nullptr, nullptr, 0);
    if (count < 0) return PWD3D11_ERROR_GENERIC;
    *num_packets = size_t(count);
    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Packetize: meta offsets into the read-back bitstream, one record the
// Vulkan decoder accepts (R/pyrowave_encoder.cpp Encoder::Impl::packetize).
// ------------------------------------------------------------------
pwd3d11_result pwd3d11_encoder_packetize(pwd3d11_encoder enc, pwd3d11_packet *packets,
                                          size_t packet_boundary, size_t *num_packets,
                                          void *bitstream, size_t size)
{
    if (!enc || !num_packets || !bitstream) return PWD3D11_ERROR_INVALID_ARGUMENT;
    if (packet_boundary < sizeof(BitstreamSequenceHeader)) return PWD3D11_ERROR_INVALID_ARGUMENT;
    int count = packetize_blocks(enc, packet_boundary, packets, static_cast<uint8_t *>(bitstream), size);
    if (count < 0) return PWD3D11_ERROR_GENERIC;
    *num_packets = size_t(count);
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
