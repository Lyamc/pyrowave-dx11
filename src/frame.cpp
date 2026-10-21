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
#include "shaders/clamp_dxbc.h"
#include "debug_dump.h"

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------------
// Pack runs as one dispatch. The band table is immutable for this size.
// Sequence is a separate 16-byte constant so the table is not rebuilt.
// ------------------------------------------------------------------
struct PackBandRecord {
    uint32_t resolution_x;
    uint32_t resolution_y;
    uint32_t blocks_32_x;
    uint32_t blocks_32_y;
    uint32_t blocks_8_x;
    uint32_t blocks_8_y;
    uint32_t quant_resolution_code;
    uint32_t group_origin;
    uint32_t block_offset_32;
    uint32_t block_stride_32;
    uint32_t block_offset_8;
    uint32_t block_stride_8;
    uint32_t disp_x;
    uint32_t disp_y;
    uint32_t pad0;
    uint32_t pad1;
};
static_assert(sizeof(PackBandRecord) == 64, "pack band record must be 64 bytes");

struct PackSeqCB {
    uint32_t sequence_code;
    uint32_t band_count;
    uint32_t pad0;
    uint32_t pad1;
};
static_assert(sizeof(PackSeqCB) == 16, "pack sequence constant must be 16 bytes");

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

    D3D11_BOX box = {};
    box.right = UINT(size);
    box.bottom = 1;
    box.back = 1;
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, buf, 0, &box);
    ctx->Flush();

    D3D11_MAPPED_SUBRESOURCE ms;
    hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &ms);
    if (FAILED(hr)) return {};

    std::vector<uint8_t> result(static_cast<uint8_t*>(ms.pData),
                                static_cast<uint8_t*>(ms.pData) + size);
    ctx->Unmap(staging.Get(), 0);
    return result;
}

static HRESULT ensure_staging(ID3D11Device *dev, ComPtr<ID3D11Buffer> &buf,
                              size_t *have, size_t need)
{
    if (buf && *have >= need) return S_OK;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = UINT(need);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    buf.Reset();
    HRESULT hr = dev->CreateBuffer(&bd, nullptr, &buf);
    if (SUCCEEDED(hr)) *have = need;
    return hr;
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

    hr_d3d = enc->d3d11_device->CreateComputeShader(
        const_cast<unsigned char*>(g_clamp_dxbc),
        g_clamp_dxbc_len,
        nullptr,
        &enc->clamp_shader
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

    hr = analyze_compile_shaders(enc->analyze);
    if (hr != PWD3D11_SUCCESS) {
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
        return hr;
    }
    hr = resolve_compile_shaders(enc->resolve);
    if (hr != PWD3D11_SUCCESS) {
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
        return hr;
    }

    *out = enc;
    return PWD3D11_SUCCESS;
}

// ------------------------------------------------------------------
// Encode a frame: dispatch pipeline, readback, packetize
// ------------------------------------------------------------------
static int pwd3_stage_on()
{
    static int on = -1;
    if (on < 0) {
        char buf[8] = {};
        DWORD n = GetEnvironmentVariableA("PWD3_STAGE", buf, sizeof buf);
        on = (n > 0 && buf[0] != '0') ? 1 : 0;
    }
    return on;
}

static void pwd3_ts_ensure(pwd3d11_encoder enc)
{
    if (enc->ts_disjoint) return;
    D3D11_QUERY_DESC desc = {};
    desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    enc->d3d11_device->CreateQuery(&desc, &enc->ts_disjoint);
    desc.Query = D3D11_QUERY_TIMESTAMP;
    for (int i = 0; i < 6; i++)
        enc->d3d11_device->CreateQuery(&desc, &enc->ts_mark[i]);
}

static void pwd3_ts_mark(pwd3d11_encoder enc, int index)
{
    if (!pwd3_stage_on() || !enc->ts_mark[index]) return;
    enc->context->End(enc->ts_mark[index].Get());
}

static void pwd3_ts_report(pwd3d11_encoder enc)
{
    if (!pwd3_stage_on() || !enc->ts_disjoint) return;
    auto ctx = enc->context;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
    HRESULT hr = S_FALSE;
    for (int spin = 0; spin < 1000 && hr == S_FALSE; spin++)
        hr = ctx->GetData(enc->ts_disjoint.Get(), &disjoint, sizeof disjoint, 0);
    if (hr != S_OK || disjoint.Disjoint) {
        fprintf(stderr, "stage gpu unavailable\n");
        return;
    }
    UINT64 tick[6] = {};
    for (int i = 0; i < 6; i++) {
        hr = S_FALSE;
        for (int spin = 0; spin < 1000 && hr == S_FALSE; spin++)
            hr = ctx->GetData(enc->ts_mark[i].Get(), &tick[i], sizeof tick[i], 0);
        if (hr != S_OK) {
            fprintf(stderr, "stage gpu mark %d missing\n", i);
            return;
        }
    }
    double scale = 1000.0 / double(disjoint.Frequency);
    static const char *name[5] = { "upload", "dwt", "quant", "clamp", "pack" };
    for (int i = 0; i < 5; i++)
        fprintf(stderr, "gpu %s %.2f\n", name[i], double(tick[i + 1] - tick[i]) * scale);
}

pwd3d11_result pwd3d11_encoder_submit(pwd3d11_encoder enc,
                                        const pwd3d11_cpu_buffer *buf,
                                        const pwd3d11_rate_control *rc)
{
    if (!enc || !buf || !rc) return PWD3D11_ERROR_INVALID_ARGUMENT;
    // The previous frame still owns the payload and the staging copy.
    if (enc->gpu_inflight) return PWD3D11_ERROR_INVALID_ARGUMENT;
    const bool subsampled =
        buf->format == PWD3D11_CPU_BUFFER_FORMAT_YUV420P
        || buf->format == PWD3D11_CPU_BUFFER_FORMAT_YUV420P10
        || buf->format == PWD3D11_CPU_BUFFER_FORMAT_YUV420P16
        || buf->format == PWD3D11_CPU_BUFFER_FORMAT_NV12;
    if (enc->chroma == PWD3D11_CHROMA_SUBSAMPLING_420 && !subsampled)
        return PWD3D11_ERROR_INVALID_ARGUMENT;
    if (enc->chroma == PWD3D11_CHROMA_SUBSAMPLING_444 && subsampled)
        return PWD3D11_ERROR_INVALID_ARGUMENT;

    auto ctx = enc->context;
    if (!ctx) return PWD3D11_ERROR_INVALID_ARGUMENT;
    HRESULT hr_d3d;
    pwd3d11_unbind_cs(ctx.Get());
    if (pwd3_stage_on()) {
        pwd3_ts_ensure(enc);
        if (enc->ts_disjoint) ctx->Begin(enc->ts_disjoint.Get());
        pwd3_ts_mark(enc, 0);
    }

    // Step 1: Upload planes and run DWT
    pwd3d11_result hr = dwt_upload_planes(enc->dwt, buf);
    if (hr != PWD3D11_SUCCESS) return hr;
    pwd3_ts_mark(enc, 1);

    hr = dwt_create_bands(enc->dwt);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = dwt_compile_shader(enc->dwt);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = dwt_run(enc->dwt);
    if (hr != PWD3D11_SUCCESS) return hr;
    pwd3_ts_mark(enc, 2);

    // DWT debug dump: skip band textures (requires texture readback, less critical)

    // Step 2: one entry per wavelet band, coarse level first, matching
    // WaveletBuffers::init_block_meta. 4:2:0 keeps luma at every level and
    // drops only the full-resolution chroma. The previous loop did the opposite.
    std::vector<ID3D11ShaderResourceView*> band_srvs;
    std::vector<int> band_w, band_h, band_pitch, band_level, band_comp, band_id;

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
            for (int band = first_band; band < 4; band++) {
                band_srvs.push_back(srv);
                band_w.push_back(bw);
                band_h.push_back(bh);
                band_pitch.push_back(pitch);
                band_level.push_back(l);
                band_comp.push_back(c);
                band_id.push_back(band);
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

    hr = quant_run(enc->quant);
    if (hr != PWD3D11_SUCCESS) return hr;
    pwd3_ts_mark(enc, 3);

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

    int count8 = enc->quant->total_blocks;
    int count32 = enc->quant->total_blocks_32;
    hr = analyze_set_stats(enc->analyze, enc->quant->stats_buf.Get(), count8, count32);
    if (hr != PWD3D11_SUCCESS) return hr;

    hr = analyze_create_buffers(enc->analyze);
    if (hr != PWD3D11_SUCCESS) return hr;
    enc->resolve->per_sub = enc->analyze->per_sub;

    pwd3d11_unbind_cs(ctx.Get());
    UINT zero_rc[4] = {};
    ctx->ClearUnorderedAccessViewUint(enc->analyze->uav_quant.Get(), zero_rc);

    // Skip analyze/resolve. Occupancy-class DWT+quant+pack already fits a
    // desktop 720p frame at quant 0. A busy frame's payload counter is over
    // the budget; leftover clamp then raises quant so pack does not overflow.
    if (enc->clamp_shader && count32 > 0 && enc->quant->srv_payload) {
        unsigned target_bytes = unsigned(rc->maximum_bitstream_size);
        struct ClampPush {
            unsigned target_bytes;
            unsigned block_count;
            unsigned floor_q;
            unsigned cap_q;
        } pc_clamp = { target_bytes, unsigned(count32), 7u, 6u };
        ID3D11Buffer *cb_clamp = nullptr;
        hr_d3d = pwd3d11_cached_cb(enc->d3d11_device.Get(), enc->clamp_cbs, 0,
                                   &pc_clamp, sizeof(pc_clamp), &cb_clamp);
        if (FAILED(hr_d3d)) return PWD3D11_ERROR_GENERIC;
        pwd3d11_unbind_cs(ctx.Get());
        ID3D11ShaderResourceView *clamp_srv[1] = { enc->quant->srv_payload.Get() };
        ID3D11UnorderedAccessView *clamp_uav[1] = { enc->analyze->uav_quant.Get() };
        ctx->CSSetShader(enc->clamp_shader.Get(), nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &cb_clamp);
        ctx->CSSetShaderResources(0, 1, clamp_srv);
        ctx->CSSetUnorderedAccessViews(0, 1, clamp_uav, nullptr);
        ctx->Dispatch((UINT(count32) + 63u) / 64u, 1, 1);
        pwd3d11_unbind_cs(ctx.Get());
    }
    pwd3_ts_mark(enc, 4);

    // Step 5: Allocate bitstream and meta buffers
    int total_blocks = count32;
    size_t meta_size = size_t(total_blocks) * 8u;
    size_t bitstream_size = rc->maximum_bitstream_size + meta_size;
    bitstream_size = (bitstream_size + 3u) & ~3u;

    if (!enc->bitstream_buf || enc->bitstream_size != bitstream_size) {
        enc->bitstream_buf.Reset();
        enc->uav_bitstream.Reset();
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = UINT(bitstream_size);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        hr_d3d = enc->d3d11_device->CreateBuffer(&bd, nullptr, &enc->bitstream_buf);
        if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = UINT(bitstream_size / 4);
        hr_d3d = enc->d3d11_device->CreateUnorderedAccessView(enc->bitstream_buf.Get(), &ud, &enc->uav_bitstream);
        if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;
        enc->bitstream_size = bitstream_size;
    }

    if (!enc->meta_buf || enc->meta_size != meta_size) {
        enc->meta_buf.Reset();
        enc->uav_meta.Reset();
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = UINT(meta_size);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        hr_d3d = enc->d3d11_device->CreateBuffer(&bd, nullptr, &enc->meta_buf);
        if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = UINT(meta_size / 4);
        hr_d3d = enc->d3d11_device->CreateUnorderedAccessView(enc->meta_buf.Get(), &ud, &enc->uav_meta);
        if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;
        enc->meta_size = meta_size;
    }

    pwd3d11_unbind_cs(ctx.Get());
    UINT zero_bits[4] = {};
    ctx->ClearUnorderedAccessViewUint(enc->uav_meta.Get(), zero_bits);

    if (!enc->srv_pack_meta || enc->pack_srv_count8 != count8) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd_meta = {};
        sd_meta.Format = DXGI_FORMAT_R32_UINT;
        sd_meta.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd_meta.Buffer.NumElements = UINT(count8 * 2);
        hr_d3d = enc->d3d11_device->CreateShaderResourceView(enc->quant->meta_buf.Get(),
                                                            &sd_meta, &enc->srv_pack_meta);
        if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

        D3D11_SHADER_RESOURCE_VIEW_DESC sd_stats = {};
        sd_stats.Format = DXGI_FORMAT_R32_UINT;
        sd_stats.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd_stats.Buffer.NumElements = UINT(count8 * 16);
        hr_d3d = enc->d3d11_device->CreateShaderResourceView(enc->quant->stats_buf.Get(),
                                                            &sd_stats, &enc->srv_pack_stats);
        if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;
        enc->pack_srv_count8 = count8;
    }

    if (!enc->srv_pack_quant || enc->pack_srv_count32 != count32) {
        hr_d3d = pwd3d11_create_raw_srv(enc->d3d11_device.Get(), enc->analyze->quant_buf.Get(),
                                        UINT(count32) * 4u, enc->srv_pack_quant.ReleaseAndGetAddressOf());
        if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;
        enc->pack_srv_count32 = count32;
    }

    std::vector<PackBandRecord> recs;
    recs.reserve(enc->quant->bands.size());
    uint32_t origin = 0;
    for (const auto &b : enc->quant->bands) {
        PackBandRecord r = {};
        uint32_t b32x = uint32_t((b.w + 31) / 32);
        uint32_t b32y = uint32_t((b.h + 31) / 32);
        uint32_t dx = (b32x + 1u) / 2u;
        uint32_t dy = (b32y + 1u) / 2u;
        r.resolution_x = uint32_t(b.w);
        r.resolution_y = uint32_t(b.h);
        r.blocks_32_x = b32x;
        r.blocks_32_y = b32y;
        r.blocks_8_x = uint32_t((b.w + 7) / 8);
        r.blocks_8_y = uint32_t((b.h + 7) / 8);
        r.quant_resolution_code = quant_code_for_band(b.level, b.component, b.band);
        r.group_origin = origin;
        r.block_offset_32 = uint32_t(b.block_offset_32);
        r.block_stride_32 = uint32_t(b.block_stride_32);
        r.block_offset_8 = uint32_t(b.block_offset_8);
        r.block_stride_8 = uint32_t(b.block_stride_8);
        r.disp_x = dx;
        r.disp_y = dy;
        origin += dx * dy;
        recs.push_back(r);
    }
    const uint32_t *words = reinterpret_cast<const uint32_t *>(recs.data());
    size_t nwords = recs.size() * (sizeof(PackBandRecord) / sizeof(uint32_t));
    bool same_table = enc->srv_pack_bands && enc->pack_band_words.size() == nwords
        && memcmp(enc->pack_band_words.data(), words, nwords * sizeof(uint32_t)) == 0;
    if (!recs.empty() && !same_table) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = UINT(nwords * sizeof(uint32_t));
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_SUBRESOURCE_DATA sd = { words, 0, 0 };
        hr_d3d = enc->d3d11_device->CreateBuffer(&bd, &sd, &enc->pack_bands);
        if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;
        D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
        srvd.Format = DXGI_FORMAT_R32_TYPELESS;
        srvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        srvd.BufferEx.NumElements = UINT(nwords);
        srvd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        hr_d3d = enc->d3d11_device->CreateShaderResourceView(enc->pack_bands.Get(), &srvd, &enc->srv_pack_bands);
        if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;
        enc->pack_band_words.assign(words, words + nwords);
        enc->pack_groups = origin;
        for (uint32_t s = 0; s < 7; s++) enc->pack_seq_cb[s].Reset();
    }
    if (!recs.empty() && !enc->pack_seq_cb[0]) {
        for (uint32_t s = 0; s < 7; s++) {
            PackSeqCB seq = {};
            seq.sequence_code = s + 1u;
            seq.band_count = uint32_t(recs.size());
            D3D11_BUFFER_DESC cbd = {};
            cbd.ByteWidth = sizeof(PackSeqCB);
            cbd.Usage = D3D11_USAGE_IMMUTABLE;
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA csd = { &seq, 0, 0 };
            hr_d3d = enc->d3d11_device->CreateBuffer(&cbd, &csd, &enc->pack_seq_cb[s]);
            if (FAILED(hr_d3d)) return PWD3D11_ERROR_GENERIC;
        }
    }

    ID3D11ShaderResourceView *pack_srvs[4] = {
        enc->srv_pack_meta.Get(), enc->srv_pack_stats.Get(), enc->srv_pack_quant.Get(),
        enc->srv_pack_bands.Get()
    };
    ID3D11UnorderedAccessView *pack_uavs[3] = {
        enc->uav_bitstream.Get(), enc->uav_meta.Get(), enc->quant->uav_payload.Get()
    };
    ctx->CSSetShader(enc->pack_shader.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 4, pack_srvs);
    ctx->CSSetUnorderedAccessViews(0, 3, pack_uavs, nullptr);
    unsigned seq_slot = enc->sequence_code - 1u;
    if (seq_slot > 6u || !enc->pack_seq_cb[seq_slot]) return PWD3D11_ERROR_GENERIC;
    ctx->CSSetConstantBuffers(0, 1, enc->pack_seq_cb[seq_slot].GetAddressOf());
    if (enc->pack_groups)
        ctx->Dispatch(enc->pack_groups, 1, 1);
    pwd3_ts_mark(enc, 5);
    if (pwd3_stage_on() && enc->ts_disjoint) ctx->End(enc->ts_disjoint.Get());

    ID3D11ShaderResourceView *nullsrv[4] = {};
    ID3D11UnorderedAccessView *nulluav[3] = {};
    ctx->CSSetShaderResources(0, 4, nullsrv);
    ctx->CSSetUnorderedAccessViews(0, 3, nulluav, nullptr);

    hr_d3d = ensure_staging(enc->d3d11_device.Get(), enc->staging_bitstream,
                            &enc->staging_bitstream_size, enc->bitstream_size);
    if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;
    hr_d3d = ensure_staging(enc->d3d11_device.Get(), enc->staging_meta,
                            &enc->staging_meta_size, enc->meta_size);
    if (FAILED(hr_d3d)) return PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY;

    D3D11_BOX bits_box = {};
    bits_box.right = UINT(enc->bitstream_size);
    bits_box.bottom = 1;
    bits_box.back = 1;
    ctx->CopySubresourceRegion(enc->staging_bitstream.Get(), 0, 0, 0, 0,
                               enc->bitstream_buf.Get(), 0, &bits_box);
    D3D11_BOX meta_box = {};
    meta_box.right = UINT(enc->meta_size);
    meta_box.bottom = 1;
    meta_box.back = 1;
    ctx->CopySubresourceRegion(enc->staging_meta.Get(), 0, 0, 0, 0,
                               enc->meta_buf.Get(), 0, &meta_box);
    ctx->Flush();
    enc->block_count = total_blocks;
    enc->gpu_inflight = 1;
    return PWD3D11_SUCCESS;
}

pwd3d11_result pwd3d11_encoder_finish(pwd3d11_encoder enc)
{
    if (!enc || !enc->gpu_inflight) return PWD3D11_ERROR_INVALID_ARGUMENT;
    auto ctx = enc->context;
    if (!ctx) return PWD3D11_ERROR_INVALID_ARGUMENT;
    int total_blocks = enc->block_count;

    D3D11_MAPPED_SUBRESOURCE ms;
    HRESULT hr_d3d = ctx->Map(enc->staging_meta.Get(), 0, D3D11_MAP_READ, 0, &ms);
    if (FAILED(hr_d3d)) return PWD3D11_ERROR_GENERIC;
    enc->host_meta.assign(static_cast<uint8_t*>(ms.pData),
                          static_cast<uint8_t*>(ms.pData) + enc->meta_size);
    ctx->Unmap(enc->staging_meta.Get(), 0);
    if (enc->host_meta.size() < size_t(total_blocks) * sizeof(BitstreamPacket)) {
        return PWD3D11_ERROR_GENERIC;
    }
    const auto *block_meta = reinterpret_cast<const BitstreamPacket *>(enc->host_meta.data());
    size_t extent = 0;
    for (int bi = 0; bi < total_blocks; bi++) {
        if (!block_meta[bi].num_words) continue;
        size_t end = (size_t(block_meta[bi].offset_u32) + block_meta[bi].num_words) * sizeof(uint32_t);
        if (end > extent) extent = end;
    }
    if (extent > enc->bitstream_size) extent = enc->bitstream_size;

    hr_d3d = ctx->Map(enc->staging_bitstream.Get(), 0, D3D11_MAP_READ, 0, &ms);
    if (FAILED(hr_d3d)) return PWD3D11_ERROR_GENERIC;
    enc->host_bitstream.assign(static_cast<uint8_t*>(ms.pData),
                               static_cast<uint8_t*>(ms.pData) + extent);
    ctx->Unmap(enc->staging_bitstream.Get(), 0);
    pwd3_ts_report(enc);

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

    // Same sequence step encode_cpu used to take after the map.
    enc->sequence_code = (enc->sequence_code + 1u) & 7u;
    if (enc->sequence_code == 0u) enc->sequence_code = 1u;
    enc->gpu_inflight = 0;
    return PWD3D11_SUCCESS;
}

pwd3d11_result pwd3d11_encoder_encode_cpu(pwd3d11_encoder enc,
                                           const pwd3d11_cpu_buffer *buf,
                                           const pwd3d11_rate_control *rc)
{
    if (enc && enc->gpu_inflight) {
        pwd3d11_result pending = pwd3d11_encoder_finish(enc);
        if (pending != PWD3D11_SUCCESS) return pending;
    }
    pwd3d11_result hr = pwd3d11_encoder_submit(enc, buf, rc);
    if (hr != PWD3D11_SUCCESS) return hr;
    return pwd3d11_encoder_finish(enc);
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
