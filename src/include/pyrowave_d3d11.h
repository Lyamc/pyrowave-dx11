// pyrowave_d3d11.h -- C ABI for the D3D11 PyroWave encoder
// Mirrors reference/pyrowave/pyrowave.h structs with static_asserts.
// See INTEGRATION.md §4 for the full ABI spec.

#ifndef PYROWAVE_D3D11_H
#define PYROWAVE_D3D11_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Enum types                                                         */
/* ------------------------------------------------------------------ */

/* R/pyrowave.h:42-55 */
typedef enum pwd3d11_result {
    PWD3D11_SUCCESS = 0,
    PWD3D11_TIMEOUT = 1,
    PWD3D11_ERROR_GENERIC = -1,
    PWD3D11_ERROR_INVALID_ARGUMENT = -2,
    PWD3D11_ERROR_OUT_OF_HOST_MEMORY = -3,
    PWD3D11_ERROR_OUT_OF_DEVICE_MEMORY = -4,
    PWD3D11_ERROR_NO_VULKAN = -5,
    PWD3D11_ERROR_NOT_IMPLEMENTED = -6,
    PWD3D11_ERROR_UNSUPPORTED_EXTERNAL_HANDLE = -7,
    PWD3D11_ERROR_FAILED_EXTERNAL_HANDLE = -8,
    PWD3D11_ERROR_INT_MAX = 0x7fffffff
} pwd3d11_result;

/* R/pyrowave.h:57-62 */
typedef enum pwd3d11_chroma_subsampling {
    PWD3D11_CHROMA_SUBSAMPLING_420 = 0,
    PWD3D11_CHROMA_SUBSAMPLING_444 = 1,
    PWD3D11_CHROMA_SUBSAMPLING_INT_MAX = 0x7fffffff
} pwd3d11_chroma_subsampling;

/* R/pyrowave.h:423-433 */
typedef enum pwd3d11_cpu_buffer_format {
    PWD3D11_CPU_BUFFER_FORMAT_NV12 = 0,
    PWD3D11_CPU_BUFFER_FORMAT_YUV420P = 1,
    PWD3D11_CPU_BUFFER_FORMAT_YUV444P = 2,
    PWD3D11_CPU_BUFFER_FORMAT_YUV420P10 = 3,
    PWD3D11_CPU_BUFFER_FORMAT_YUV444P10 = 4,
    PWD3D11_CPU_BUFFER_FORMAT_YUV420P16 = 5,
    PWD3D11_CPU_BUFFER_FORMAT_YUV444P16 = 6,
    PWD3D11_CPU_BUFFER_FORMAT_INT_MAX = 0x7fffffff
} pwd3d11_cpu_buffer_format;

/* ------------------------------------------------------------------ */
/* Struct types -- mirrors of R/pyrowave.h                              */
/* Layout checked by static_assert below.                               */
/* ------------------------------------------------------------------ */

/* R/pyrowave.h:377-381 */
typedef struct pwd3d11_packet {
    size_t offset;
    size_t size;
} pwd3d11_packet;

/* R/pyrowave.h:435-448 */
typedef struct pwd3d11_cpu_buffer {
    void *data[3];
    size_t row_stride_in_bytes[3];
    size_t plane_size_in_bytes[3];
    int width;
    int height;
    pwd3d11_cpu_buffer_format format;
} pwd3d11_cpu_buffer;

/* R/pyrowave.h:450-454 */
typedef struct pwd3d11_rate_control {
    size_t maximum_bitstream_size;
} pwd3d11_rate_control;

/* R/pyrowave.h:368-375 (adapted for D3D11) */
typedef struct pwd3d11_device_t  *pwd3d11_device;
typedef struct pwd3d11_encoder_t *pwd3d11_encoder;

typedef struct pwd3d11_encoder_create_info {
    pwd3d11_device device;
    int width;
    int height;
    pwd3d11_chroma_subsampling chroma;
} pwd3d11_encoder_create_info;

/* INTEGRATION.md §4: pwd3d11_device_create_info */
typedef struct pwd3d11_device_create_info {
    uint32_t struct_size;          /* sizeof, for forward compatibility */
    int32_t  adapter_index;        /* -1: first hardware adapter with FL 11_0 and CS support */
    void    *d3d11_device;         /* optional ID3D11Device* to share; NULL creates one */
    uint32_t flags;                /* bit0: debug layer, bit1: allow WARP (tests only) */
} pwd3d11_device_create_info;

/* INTEGRATION.md §4: pwd3d11_adapter_info */
typedef struct pwd3d11_adapter_info {
    char name[128];
    uint32_t vendor_id;
    uint32_t device_id;
    uint64_t dedicated_video_memory;
} pwd3d11_adapter_info;

/* ------------------------------------------------------------------ */
/* Static asserts -- struct layout verification                          */
/* Cite the original R/ definition each struct mirrors.                  */
/* ------------------------------------------------------------------ */

#ifdef __cplusplus
/* R/pyrowave.h:377-381 -- two size_t fields = 16 */
static_assert(sizeof(pwd3d11_packet) == 16, "pwd3d11_packet size must be 16");

/* R/pyrowave.h:450-454 -- one size_t field = 8 */
static_assert(sizeof(pwd3d11_rate_control) == 8, "pwd3d11_rate_control size must be 8");

/* R/pyrowave.h:435-448 -- data[3](24)+row_stride[3](24)+plane_size[3](24)+width(4)+height(4)+format(4) = 84+pad(4) = 88 */
static_assert(sizeof(pwd3d11_cpu_buffer) == 88, "pwd3d11_cpu_buffer size mismatch");
static_assert(offsetof(pwd3d11_cpu_buffer, data) == 0, "pwd3d11_cpu_buffer.data offset must be 0");
static_assert(offsetof(pwd3d11_cpu_buffer, width) == 72, "pwd3d11_cpu_buffer.width offset must be 72");
static_assert(offsetof(pwd3d11_cpu_buffer, height) == 76, "pwd3d11_cpu_buffer.height offset must be 76");
static_assert(offsetof(pwd3d11_cpu_buffer, format) == 80, "pwd3d11_cpu_buffer.format offset must be 80");

/* INTEGRATION.md §4: name(128)+vendor_id(4)+device_id(4)+dedicated_video_memory(8) = 144 */
static_assert(sizeof(pwd3d11_adapter_info) == 144, "pwd3d11_adapter_info size must be 144");
static_assert(offsetof(pwd3d11_adapter_info, name) == 0, "pwd3d11_adapter_info.name offset must be 0");
static_assert(offsetof(pwd3d11_adapter_info, vendor_id) == 128, "pwd3d11_adapter_info.vendor_id offset must be 128");
static_assert(offsetof(pwd3d11_adapter_info, device_id) == 132, "pwd3d11_adapter_info.device_id offset must be 132");
static_assert(offsetof(pwd3d11_adapter_info, dedicated_video_memory) == 136, "pwd3d11_adapter_info.dedicated_video_memory offset must be 136");
#endif

/* ------------------------------------------------------------------ */
/* C ABI entry points (INTEGRATION.md §4)                                */
/* ------------------------------------------------------------------ */

#define PWD3D11_API_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

uint32_t       pwd3d11_get_api_version(void);
pwd3d11_result pwd3d11_create_device(const pwd3d11_device_create_info *info, pwd3d11_device *out);
pwd3d11_result pwd3d11_device_get_adapter_info(pwd3d11_device, pwd3d11_adapter_info *out);
void          *pwd3d11_device_get_d3d11_device(pwd3d11_device);  // returns ID3D11Device* (T6c debug)
void          *pwd3d11_device_get_d3d11_context(pwd3d11_device); // returns ID3D11DeviceContext* (T6c debug)
void           pwd3d11_device_destroy(pwd3d11_device);

pwd3d11_result pwd3d11_encoder_create(const pwd3d11_encoder_create_info *, pwd3d11_encoder *out);
pwd3d11_result pwd3d11_encoder_encode_cpu(pwd3d11_encoder, const pwd3d11_cpu_buffer *, const pwd3d11_rate_control *);
pwd3d11_result pwd3d11_encoder_compute_num_packets(pwd3d11_encoder, size_t packet_boundary, size_t *num_packets);
pwd3d11_result pwd3d11_encoder_packetize(pwd3d11_encoder, pwd3d11_packet *packets, size_t packet_boundary,
                                         size_t *num_packets, void *bitstream, size_t size);
void           pwd3d11_encoder_destroy(pwd3d11_encoder);

pwd3d11_result pwd3d11_encoder_get_timings(pwd3d11_encoder, float *us, size_t count);

/* ------------------------------------------------------------------ */
/* Debug API -- T6c: dump hooks + pwd3d11_debug_run_stage (VALIDATION §3.4, §5) */
/* ------------------------------------------------------------------ */

/* Buffer descriptor for debug dumps (matches VALIDATION §3.4 format) */
typedef struct pwd3d11_debug_buffer {
    const char *name;       /* e.g. "band_c0_l0_b0" */
    int stage;              /* stage number (1=DWT, 2=quant, ...) */
    int dtype;              /* 0=f32, 1=f16 */
    int shape_w;
    int shape_h;
    const void *data;       /* pointer to raw buffer data */
    size_t data_len;        /* number of elements (floats or uint16_t) */
} pwd3d11_debug_buffer;

/* Stage enum for debug_run_stage */
typedef enum pwd3d11_stage {
    PWD3D11_STAGE_DWT = 1,
    PWD3D11_STAGE_QUANT = 2,
    PWD3D11_STAGE_ANALYZE = 3,
    PWD3D11_STAGE_RESOLVE = 4,
    PWD3D11_STAGE_PACK = 5,
    PWD3D11_STAGE_PACKETIZE = 6,
    PWD3D11_STAGE_MAX = 0x7fffffff
} pwd3d11_stage;

/* Run a specific stage with optional input buffers from Vulkan dumps (M3 mode) */
/* input_buffers: array of pwd3d11_debug_buffer, one per band input (for DWT stage) */
/* output_buffers: array of pwd3d11_debug_buffer to receive output */
/* num_input_buffers / num_output_buffers: counts */
pwd3d11_result pwd3d11_debug_run_stage(pwd3d11_encoder enc, pwd3d11_stage stage,
                                        const pwd3d11_debug_buffer *input_buffers,
                                        size_t num_input_buffers,
                                        pwd3d11_debug_buffer *output_buffers,
                                        size_t num_output_buffers);

/* Set the debug dump output directory. When PWD3D11_DEBUG_DUMP is defined,
   running stages will write manifest.json + *.bin files in VALIDATION §3.4 format. */
void pwd3d11_debug_set_dump_dir(pwd3d11_encoder enc, const char *dump_dir);

#ifdef __cplusplus
}
#endif

#endif /* PYROWAVE_D3D11_H */
