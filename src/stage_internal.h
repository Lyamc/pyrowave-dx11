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

// ------------------------------------------------------------------
// BandTex (dwt.cpp)
// ------------------------------------------------------------------
struct BandTex {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11ShaderResourceView> srv_layer0;
    int w = 0, h = 0;
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
    ComPtr<ID3D11SamplerState> sampler;

    ComPtr<ID3D11Texture2D> planes[3];
    ComPtr<ID3D11ShaderResourceView> plane_srv[3];

    BandTex bands[3][Levels];

    int pw[3], ph[3];
    int aw = 0, ah = 0;
    int width = 0, height = 0;
    int chroma = 0;
};

// ------------------------------------------------------------------
// QuantState (quant.cpp)
// ------------------------------------------------------------------
struct QuantState {
    ~QuantState();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;

    struct BandRef {
        ComPtr<ID3D11ShaderResourceView> srv;
        int w = 0, h = 0;
        int level = 0, component = 0;
    };
    std::vector<BandRef> bands;
    int pw[3], ph[3];
    int aw = 0, ah = 0;
    int chroma = 0;

    int total_blocks = 0;
    size_t payload_size = 0;
    ComPtr<ID3D11Buffer> meta_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_meta;
    ComPtr<ID3D11Buffer> stats_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_stats;
    ComPtr<ID3D11Buffer> payload_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_payload;
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

    size_t bucket_size = 0;
    ComPtr<ID3D11Buffer> bucket_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_bucket;
    ComPtr<ID3D11Buffer> stats_buf;
    ComPtr<ID3D11ShaderResourceView> srv_stats;
    ComPtr<ID3D11Buffer> quant_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_quant;
    int total_blocks_32x32 = 0;
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

    ComPtr<ID3D11Buffer> bitstream_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_bitstream;
    ComPtr<ID3D11Buffer> payload_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_payload;
    ComPtr<ID3D11Buffer> meta_buf;
    ComPtr<ID3D11UnorderedAccessView> uav_meta;

    size_t bitstream_size = 0;
    size_t meta_size = 0;
    std::vector<uint8_t> host_bitstream;
    unsigned int sequence_code;
    float timings[10];
    size_t timing_count = 0;
};

#endif /* STAGE_INTERNAL_H */
