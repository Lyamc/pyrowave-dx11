// stage_internal.h -- Internal struct definitions for all stages
// Included by frame.cpp and the individual stage .cpp files

#ifndef STAGE_INTERNAL_H
#define STAGE_INTERNAL_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <vector>

using Microsoft::WRL::ComPtr;

// HD 6670 ignores UpdateSubresource and ClearUAV on a DEFAULT buffer that is
// still bound as a CS UAV or SRV. Drop those binds before a later frame
// reuses the same resource.
static inline void pwd3d11_unbind_cs(ID3D11DeviceContext *ctx)
{
    if (!ctx) return;
    ID3D11ShaderResourceView *srv[8] = {};
    ID3D11UnorderedAccessView *uav[8] = {};
    ID3D11Buffer *cb[8] = {};
    ctx->CSSetShaderResources(0, 8, srv);
    ctx->CSSetUnorderedAccessViews(0, 8, uav, nullptr);
    ctx->CSSetConstantBuffers(0, 8, cb);
}

static inline HRESULT pwd3d11_create_raw_uav(ID3D11Device *dev, ID3D11Buffer *buf, UINT bytes,
                                             ID3D11UnorderedAccessView **out)
{
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = bytes / 4u;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    return dev->CreateUnorderedAccessView(buf, &ud, out);
}

static inline HRESULT pwd3d11_create_raw_srv(ID3D11Device *dev, ID3D11Buffer *buf, UINT bytes,
                                             ID3D11ShaderResourceView **out)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.NumElements = bytes / 4u;
    sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    return dev->CreateShaderResourceView(buf, &sd, out);
}

// IMMUTABLE CBs are the path that actually lands constants on the HD 6670.
// Cache them by slot so later frames skip CreateBuffer.
static inline HRESULT pwd3d11_cached_cb(ID3D11Device *dev,
                                        std::vector<ComPtr<ID3D11Buffer>> &cache,
                                        size_t idx, const void *data, UINT bytes,
                                        ID3D11Buffer **out)
{
    if (idx >= cache.size()) cache.resize(idx + 1);
    if (!cache[idx]) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = (bytes + 15u) & ~15u;
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA sd = { data, 0, 0 };
        HRESULT hr = dev->CreateBuffer(&bd, &sd, cache[idx].GetAddressOf());
        if (FAILED(hr)) return hr;
    }
    *out = cache[idx].Get();
    return S_OK;
}

// ------------------------------------------------------------------
// BandTex (dwt.cpp)
// ------------------------------------------------------------------
struct BandTex {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11ShaderResourceView> srv_layer0;
    // Float copy of the four layers. Quant reads this instead of the texture.
    ComPtr<ID3D11Buffer> coeff;
    ComPtr<ID3D11UnorderedAccessView> coeff_uav;
    ComPtr<ID3D11ShaderResourceView> coeff_srv;
    int w = 0, h = 0;
    int pitch = 0;
    DXGI_FORMAT fmt;
};

static const int Levels = 5;

// ------------------------------------------------------------------
// DWTState (dwt.cpp)
// ------------------------------------------------------------------
struct DWTState {
    ~DWTState();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11ComputeShader> shader_float;
    ComPtr<ID3D11ComputeShader> shader_interior_byte;
    ComPtr<ID3D11ComputeShader> shader_interior_float;
    std::vector<ComPtr<ID3D11Buffer>> push_cbs;
    std::vector<ComPtr<ID3D11Buffer>> level_cb;
    ComPtr<ID3D11SamplerState> sampler;

    ComPtr<ID3D11Texture2D> planes[3];
    ComPtr<ID3D11ShaderResourceView> plane_srv[3];
    // Raw R8 copy of each plane. The DWT reads this, not the texture.
    ComPtr<ID3D11Buffer> plane_raw[3];
    ComPtr<ID3D11ShaderResourceView> plane_raw_srv[3];
    int plane_pitch[3] = {};
    int plane_raw_bytes[3] = {};
    uint32_t plane_float = 0;
    std::vector<uint8_t> plane_pack[3];
    bool bands_ready = false;

    BandTex bands[3][Levels];

    int pw[3], ph[3];
    int aw = 0, ah = 0;
    int width = 0, height = 0;
    int chroma = 0;

    // Per-group DWT tile. Two slots, sized for the luma level-0 grid.
    ComPtr<ID3D11Buffer> scratch;
    ComPtr<ID3D11UnorderedAccessView> scratch_uav;
    uint32_t scratch_floats = 0;
};

// ------------------------------------------------------------------
// QuantState (quant.cpp)
// ------------------------------------------------------------------
struct QuantState {
    ~QuantState();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    std::vector<ComPtr<ID3D11Buffer>> push_cbs;
    std::vector<ComPtr<ID3D11Buffer>> band_cb;
    std::vector<ComPtr<ID3D11Buffer>> sub_cb;

    struct BandRef {
        ComPtr<ID3D11ShaderResourceView> srv;
        int w = 0, h = 0;
        int pitch = 0;
        int level = 0, component = 0, band = 0;
        int block_offset_8 = 0, block_stride_8 = 0;
        int block_offset_32 = 0, block_stride_32 = 0;
    };
    std::vector<BandRef> bands;
    int pw[3], ph[3];
    int aw = 0, ah = 0;
    int chroma = 0;

    int total_blocks = 0;
    int total_blocks_32 = 0;
    size_t payload_size = 0;
    ComPtr<ID3D11Buffer> meta_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_meta;
    ComPtr<ID3D11Buffer> stats_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_stats;
    ComPtr<ID3D11Buffer> payload_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_payload;
    ComPtr<ID3D11ShaderResourceView> srv_payload;
};

// ------------------------------------------------------------------
// AnalyzeState (analyze.cpp)
// ------------------------------------------------------------------
struct AnalyzeState {
    ~AnalyzeState();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> analyze_shader;
    ComPtr<ID3D11ComputeShader> finalize_shader;
    std::vector<ComPtr<ID3D11Buffer>> push_cbs;
    size_t cb_cursor = 0;

    size_t bucket_size = 0;
    ComPtr<ID3D11Buffer> bucket_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_bucket;
    ComPtr<ID3D11ShaderResourceView> srv_bucket;
    ComPtr<ID3D11Buffer> stats_buf;
    ComPtr<ID3D11ShaderResourceView> srv_stats;
    ComPtr<ID3D11Buffer> quant_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_quant;
    int total_blocks_32x32 = 0;
    int stats_8x8 = 0;
    int quant_blocks = 0;
    int per_sub = 0;
    bool quant_zero = false;
    int aw = 0, ah = 0;
    int chroma = 0;
};

// ------------------------------------------------------------------
// ResolveState (resolve.cpp)
// ------------------------------------------------------------------
struct ResolveState {
    ~ResolveState();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> resolve_shader;
    std::vector<ComPtr<ID3D11Buffer>> push_cbs;
    int per_sub = 0;
};

// ------------------------------------------------------------------
// Encoder state (frame.cpp)
// ------------------------------------------------------------------
struct pwd3d11_encoder_t {
    pwd3d11_device device;
    ComPtr<ID3D11Device> d3d11_device;
    ComPtr<ID3D11DeviceContext> context;
    int width;
    int height;
    pwd3d11_chroma_subsampling chroma;
    char dump_dir[512];

    DWTState *dwt = nullptr;
    QuantState *quant = nullptr;
    AnalyzeState *analyze = nullptr;
    ResolveState *resolve = nullptr;
    ComPtr<ID3D11ComputeShader> pack_shader;
    ComPtr<ID3D11ComputeShader> clamp_shader;
    std::vector<ComPtr<ID3D11Buffer>> pack_cbs;
    std::vector<ComPtr<ID3D11Buffer>> clamp_cbs;
    ComPtr<ID3D11Buffer> pack_bands;
    ComPtr<ID3D11ShaderResourceView> srv_pack_bands;
    ComPtr<ID3D11ShaderResourceView> srv_pack_meta;
    ComPtr<ID3D11ShaderResourceView> srv_pack_stats;
    ComPtr<ID3D11ShaderResourceView> srv_pack_quant;
    ComPtr<ID3D11Buffer> pack_seq_cb[7];
    std::vector<uint32_t> pack_band_words;
    uint32_t pack_groups = 0;
    int pack_srv_count8 = 0;
    int pack_srv_count32 = 0;

    ComPtr<ID3D11Buffer> bitstream_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_bitstream;
    ComPtr<ID3D11Buffer> payload_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_payload;
    ComPtr<ID3D11Buffer> meta_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_meta;
    ComPtr<ID3D11Buffer> staging_bitstream;
    ComPtr<ID3D11Buffer> staging_meta;
    size_t staging_bitstream_size = 0;
    size_t staging_meta_size = 0;

    size_t bitstream_size = 0;
    size_t meta_size = 0;
    int block_count = 0;
    std::vector<uint8_t> host_bitstream;
    std::vector<uint8_t> host_meta;
    unsigned int sequence_code;
    float timings[10];
    size_t timing_count = 0;
    ComPtr<ID3D11Query> ts_disjoint;
    ComPtr<ID3D11Query> ts_mark[6];
    int gpu_inflight;
};

#endif /* STAGE_INTERNAL_H */
