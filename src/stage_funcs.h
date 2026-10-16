// stage_funcs.h -- Internal declarations for stage functions
// Included by c_api.cpp and frame.cpp

#ifndef STAGE_FUNCS_H
#define STAGE_FUNCS_H

#include "stage_internal.h"

// DWT (dwt.cpp)
pwd3d11_result dwt_create(DWTState *st, pwd3d11_device dev, int W, int H, int chroma_subsample);
pwd3d11_result dwt_upload_planes(DWTState *st, const pwd3d11_cpu_buffer *buf);
pwd3d11_result dwt_create_bands(DWTState *st);
pwd3d11_result dwt_compile_shader(DWTState *st);
pwd3d11_result dwt_run(DWTState *st);
ID3D11ShaderResourceView *dwt_get_band_srv(DWTState *st, int component, int level);
int dwt_get_band_w(DWTState *st, int component, int level);
int dwt_get_band_h(DWTState *st, int component, int level);
void dwt_destroy(DWTState *st);

// Quant (quant.cpp)
pwd3d11_result quant_create(QuantState *st, pwd3d11_device dev, int W, int H, int chroma_subsample);
pwd3d11_result quant_set_bands(QuantState *st, ID3D11ShaderResourceView * const *band_srvs,
                                 const int *band_w, const int *band_h, int num_bands);
pwd3d11_result quant_create_outputs(QuantState *st);
pwd3d11_result quant_compile_shader(QuantState *st);
pwd3d11_result quant_run(QuantState *st);
void quant_destroy(QuantState *st);

// Analyze (analyze.cpp)
pwd3d11_result analyze_create(AnalyzeState *st, ID3D11Device *dev, int W, int H, int chroma);
pwd3d11_result analyze_set_stats(AnalyzeState *st, ID3D11Buffer *stats_buffer, int total_blocks);
pwd3d11_result analyze_create_buffers(AnalyzeState *st);
pwd3d11_result analyze_compile_shaders(AnalyzeState *st);
pwd3d11_result analyze_run(AnalyzeState *st,
                           int bw, int bh, int block_offset_8x8, int block_stride_8x8,
                           int block_offset_32x32, int block_stride_32x32);
pwd3d11_result analyze_finalize_run(AnalyzeState *st);
void analyze_destroy(AnalyzeState *st);

// Resolve (resolve.cpp)
pwd3d11_result resolve_create(ResolveState *st, ID3D11Device *dev, int per_sub);
pwd3d11_result resolve_compile_shaders(ResolveState *st);
pwd3d11_result resolve_run(ResolveState *st,
                            ID3D11ShaderResourceView *srv_bucket,
                            ID3D11UnorderedAccessView *uav_quant,
                            unsigned int target_payload_size);
void resolve_destroy(ResolveState *st);

#endif /* STAGE_FUNCS_H */
