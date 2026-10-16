# T6a review: `tools/vkref`

Reviewed against `docs/VALIDATION.md` §3 and `reference/pyrowave` @ a0d2e71.
`git apply` can succeed. The patched tree does not compile, and nothing in this tree writes a real dump.

`tests/accept_T6a.py` only checks that both patches exist, that `git apply --check` succeeds, and that `testdata/manifests/` has at least 56 JSON files. It never builds the encoder or hashes a buffer. `docs/progress/T6a.md` records those two checks as PASS.

## `0001-dump-hooks.patch` does not compile

Header and definitions do not match.

- `pyrowave_encoder.hpp` declares `void dump_hook(int stage, const char *name)`.
- Call sites pass three arguments, and the names are not strings: `dump_hook(2, meta-buffer, *meta_buffer)`. `meta-buffer` is subtraction of two undeclared identifiers.
- The buffer definition is `Encoder::Impl::dump_hook(int, const char *, const BufferHandle &)`. `Impl` is private (`pyrowave_encoder.hpp:86-87`) and the method is not declared inside `struct Encoder::Impl`, so the definition is ill-formed.
- Calls pass `*meta_buffer`, which is `Buffer&`. The definition takes `BufferHandle&` (`IntrusivePtr`). Stage 5 is worse: `BitstreamBuffers::meta.buffer` is `const Buffer *` (`pyrowave_encoder.hpp:29-33`), so `*buffers.meta.buffer` is `const Buffer`.
- The DWT call is `dump_hook(1, component_layer_views[c][l])` (two arguments, an `ImageViewHandle`). The image overload is a stub that writes nothing.
- `Encoder::encode_bands` calls `impl->encode_bands`, which is only declared on `Impl` and has no body. `0002` then calls that missing function.

Granite API used by the hook is wrong. Buffers are device-local (`BufferDomain::Device`, `pyrowave_encoder.cpp:183-184`), so they cannot be mapped in place.

- `device->barrier(...)` does not exist. Barriers are `CommandBuffer::barrier` (`pyrowave_encoder.cpp:1221`).
- `Device::map_host_buffer` takes `const Buffer &`, not `BufferHandle`. There is no `unmap_host_buffer` after the map.
- Spec requires a full barrier and a copy into a `CachedHost` buffer (`VALIDATION.md` §3.2). The hook maps the GPU buffer and `fwrite`s it.

`dump_enabled` / `dump_dir` are never set. No C API, no env var, no driver call turns the hooks on.

## Dumps would not match §3.2 or §3.4 even if the hook compiled

- Stage 1 must be one f32 file per band, R16F levels widened on readback, named like `band_c0_l0_b1` (`VALIDATION.md:73-74`). `component_layer_views[c][l]` is one 4-layer array view (`pyrowave_common.cpp:178-182`: `base_layer = 4 * component`, `layers = 4`), not a per-band 2D image. The stub does not copy it.
- Stage 2 and stage 5 both open `meta-buffer.bin`, so packing overwrites the quant meta. Stage 5's output meta is a different buffer (`BitstreamPacket[]` in `BitstreamBuffers`, not the internal `BlockMeta` buffer).
- No sha256, no `manifest.json` written next to the bins, no `sequence` field, no device/driver string from the actual ICD.
- One file name per buffer, so a 10-frame sequence (`VALIDATION.md` §7, `SequenceCountMask = 7`) would overwrite frame 0.

## `0002-inject-bands.patch` does not inject bands

`VALIDATION.md` §3.3: upload `bands_f32` into `wavelet_img_high_res` / `wavelet_img_low_res`, round levels 0-1 to R16F the same way the D3D11 side does, then `encode_pre_transformed(cmd, buffers, -1.0f)`, and bump `sequence_count` in the new entry point because `encode_pre_transformed` does not (`pyrowave_encoder.cpp:1196-1198` vs `:1181-1194`).

What the patch does:

- The upload is comments. `bands_f32` and `count` are unused. No staging buffer, no f32→f16 store, no image copy, no layout transition to `GENERAL`.
- It calls `encoder->encoder.encode_bands`, which is not the spec entry and is not implemented.
- Sequence update is `encoder->impl->sequence_count = ...`. `impl` is private.
- No declaration in `pyrowave.h`, and no `PYROWAVE_PUBLIC_API`, so the symbol is not part of the C ABI the driver is supposed to call.
- `CommandBuffer cmd(device, CommandBuffer::Type::Compute)` is not how Granite command buffers are obtained (`device->request_command_buffer` → `CommandBufferHandle`). `Device::submit` takes `CommandBufferHandle &`.
- `cmd.copy_buffer(*gpu, *host, 0, 0, size)` does not match `CommandBuffer::copy_buffer` (dst, src, and a copy count, not five loose integers).
- The same `cmd` is ended, submitted, then `begin`'d again. Results are stored on `contexts[]` without the wait/retire path `pyrowave_encoder_encode_gpu_inner` uses, so `pyrowave_encoder_get_mapped_raw_bitstream` would not return these buffers.
- `quant_scale` is never passed. The normal encode path uses `-1.0f` so each band uses `get_quant_resolution` (`pyrowave_encoder.cpp:1225`).

## `vkref_driver.cpp` cannot drive the reference

It does not link Granite. `tools/vkref/CMakeLists.txt` compiles `pyrowave_encoder.cpp` and `pyrowave_c.cpp` as a standalone static lib (`C++14`, `PYROWAVE_PRECISION=1`) with only the Vulkan loader. Those files include Granite (`device.hpp`, `command_buffer.hpp`, slangmosh). The real build is the pyrowave CMake plus the Granite submodule, with the patches applied on top. This CMake target is not that.

Vulkan setup does not meet `pyrowave.h:89-144`:

- `#define VK_NO_PROTOTYPES` and then direct `vkCreateInstance` / `vkEnumeratePhysicalDevices` / … with no `vkGetInstanceProcAddr` load. Those calls are null.
- `pyrowave_device_create_info` leaves `instance_create_info` and `device_create_info` null. Both must stay valid for the life of the device (`pyrowave.h:100-107`).
- Comments require a graphics-capable queue and Vulkan 1.3 subgroup size control. The device is created with one compute queue, no extensions, and `shaderInt16` sitting in an unused `VkPhysicalDeviceFeatures2`. Only `VkPhysicalDeviceVulkan12Features` is chained, so 8-bit storage is requested and 16-bit arithmetic is not.
- `features2` is dead. `pEnabledFeatures` is null.

Dump path is a no-op:

- `PYROWAVE_DUMP_DIR` is read and ignored (`vkref_driver.cpp:343-347`). There is no API on the committed patch to set `dump_dir`.
- `bands_file` is parsed and ignored. M2 injection is not called.
- After `encode_cpu` it writes `bitstream.bin` and `meta.bin`. Those names are not the §3.2 table (`meta-buffer`, `block-stat-buffer`, `payload-data`, `bucket-buffer`, `quant-buffer`, per-band images).
- `write_manifest` emits `// TODO: populate from dump files` inside the JSON array. That file is not valid JSON and has no `sha256`, `sequence`, or per-buffer entries.
- `test_vectors[]` is unused. The CLI encodes one frame of one size.

Vector content will not match `tools/genvectors.py` / `VALIDATION.md` §7:

- Missing `text` and `edge`.
- `grad` is luma-only `x * 255 / w` with chroma 128. Checker/noise/flat are local formulas, not the shared generator.
- Noise is `(i * 7 + 13) & 0xFF`, not a seeded RNG.
- One budget: `max(W*H*3/16, 32 KiB)`. Missing 16 KiB, `W*H`, and a size not divisible by 4.
- One frame. Spec is 10 frames so the 3-bit sequence counter wraps.
- Sizes in the unused table are incomplete (most patterns only at 256×144).

## `generate_manifests.py` / `testdata/manifests/`

56 files exist. They are placeholders, not manifests of a dump directory (`VALIDATION.md` §3.4: one `manifest.json` beside the `.bin` files).

- Every `sha256` is the string `"placeholder"`.
- Device is hardcoded `RTX 2060 SUPER/595.104.02`. Nothing queried a driver.
- `ref` is `a0d2e71+vkref` with no patch commit.
- Band shape is `[W/32, H/32]`. A level-0 band is about `aligned/2` on each axis (`Alignment = 32`, `pyrowave_common.hpp:113`). For 256×144 that is on the order of 128×80, not 8×4. Only `band_c0_l0_b0` is listed; chroma and levels 1-4 and bands 1-3 are absent. L0-1 are R16F in the image and must be stored as widened f32.
- `meta-buffer` shape is `(W*H)/1024`, i.e. a 32×32-block count, and it ignores alignment. `meta_buffer` is `block_count_8x8 * sizeof(BlockMeta)` with `BlockMeta` 8 bytes (`pyrowave_encoder.cpp:190-191`, `pyrowave_common.hpp:106-110`). 256×144 aligns to 256×160 → 640 blocks of 8×8, not 36.
- `block-stat-buffer` uses that same 32×32 count times 16. `BlockStats` is 64 bytes per 8×8 block (`pyrowave_common.hpp:99-104`).
- `payload-data` shape `W*H*2` and `bucket-buffer` shape `[1024]` do not match the allocations in `init_block_meta` (`pyrowave_encoder.cpp:194-207`: payload is `aligned_width * aligned_height * 2`; the bucket buffer is `RDOBucketOffset` plus per-bucket op lists).
- `quant-buffer` is one `uint32` per 32×32 block (`pyrowave_encoder.cpp:199-201`), which happens to match `(W*H)/1024` only when both dimensions are already multiples of 32. 1366×768 is not (`VALIDATION.md` §7: AW 1376).
- Stage 5 lists `bitstream` only. The internal quant meta and the packed meta are different buffers and both have to be dumped (§3.2 stage 2 vs stage 5).
- No `sequence`. One budget only. No `text` / `edge`.

## Uncommitted files in `tools/vkref/` (do not treat as a fix)

The working tree is not the committed patch:

- `0001-dump-hooks.patch` is modified and still has the public-vs-`Impl` mismatch and the Granite call errors.
- `0001-dump-hooks-v2.patch` duplicates `analyze_rdo` / `block_packing` calls and pastes a second copy of `compute_num_packets` / `packetize`.
- `apply_dump_hooks.py` looks for source text that is not in a0d2e71 (the script was written against an already-edited file). It is not a patch `git apply` can check.

## Acceptance still open

AGENTS.md T6a: patches apply to a0d2e71, and manifests for all vectors.

- Applying is necessary and not sufficient. The applied result must build with the real Granite tree and must run on the RTX.
- Manifests have to describe real `.bin` files with sha256, for every synthetic vector, size, budget, and the 10-frame sequences in `VALIDATION.md` §7, plus the four real clips kept on .24.
- Level 0 (public `get_mapped_raw_bitstream` + `packetize`, no patch) is unimplemented. Level 1 hooks are stubs. Level 2 injection does not upload bands.
