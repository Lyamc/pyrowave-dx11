// debug_dump.h -- Debug dump infrastructure (T6c)
// Writes manifest.json + *.bin files in VALIDATION §3.4 format.
// Supports pwd3d11_debug_run_stage for M3 mode (feeding Vulkan dumps into D3D11 stages).

#ifndef DEBUG_DUMP_H
#define DEBUG_DUMP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dump a single buffer to <dump_dir>/<name>.bin and record in manifest. */
int debug_dump_buffer(const char *dump_dir, const char *name, int stage, int dtype,
                      int shape_w, int shape_h, const void *data, size_t data_len);

/* Write manifest.json for all dumped buffers in a stage run. */
int debug_write_manifest(const char *dump_dir, const char *vector_name, int w, int h,
                         int budget, const char *device_name,
                         const char **buffer_names, int *stages, int *dtypes,
                         int *shapes_w, int *shapes_h, int count);

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_DUMP_H */
