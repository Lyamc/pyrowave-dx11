# Integrating the D3D11 encoder into etherquic

`eq:` = github.com/Lyamc/etherquic @ cecf3f4 (checkout `~/Build/etherquic` on 192.168.0.24). Every statement about the
host below cites the code it describes. Proposed changes are marked **Proposed**. They are a plan for task T8; nothing
in etherquic changes as part of this repo's spec work.

## 1. How the host uses PyroWave today

### 1.1 Process model (Windows)
- The service runs as LocalSystem (`eq:src/backend.rs:6-9`). It starts a `--desk` helper in the console session with a
  SYSTEM or user token (`eq:host/src/link.rs:297-337, 419-450`). The helper captures the desktop with DXGI duplication on
  its own D3D11 device (`eq:host/src/desktop.rs:660-690`).
- The helper sends raw RGB frames over a pipe, and **the service encodes them** (`eq:host/src/media.rs:20-23`: "Either
  way the service encodes the helper's raw frames here"). The PyroWave encoder therefore runs in the service process,
  in session 0 (OQ-14).

### 1.2 Capability probe
`eq:media/src/caps.rs:35-76` (`run_probe`), called once at startup from `eq:host/src/extras.rs:97`:
1. `pyrowave::Encoder::new(256, 144, 32 KiB)`, then a real `encode` of a test frame (`:43-47`).
2. On success, push `EncodeCap { video: Pyrowave or PyrowaveCpu (if the Vulkan device is a CPU), encoder: "pyrowave",
   limit: 7680x4320 }` (`:59-63`).
3. Decode the record with `pyrowave::Decoder` to announce decode support (`:64-67`).
4. On failure, add a note `pyrowave encode: <error>` and push no encode row (`:69-75`).

### 1.3 Negotiation
- Rows (`eq:src/negotiate.rs:36-118`): `Video::Pyrowave` (token `pyrowave`, name "PyroWave (GPU)") and
  `Video::PyrowaveCpu` (`pyrowave-cpu`, "PyroWave (CPU)"). Both map to `VideoCodec::Pyrowave`, one bitstream. Default
  order `ALL`: AV1/H.265/H.264 HW, **Pyrowave**, MJPEG, H.264 SW, **PyrowaveCpu** (`:66-74`).
- Host caps text: `host-video <token>=<encoder>:<WxH@fps> ...` (`:240-249`). The encoder name is free text shown on the
  status line (`:190`).
- `choose_video` (`:581-615`) walks the player's offer and takes the first row the host `encodes`. `HostCaps::encodes`
  returns the **first** cap with that row (`:215-217`). An old player that does not send `knows pyrowave-cpu`
  (`:286-338`) gets a CPU encoder under plain `pyrowave` (`:597-604`).

### 1.4 Opening and running the encoder
- `media/src/host.rs:290-345`: pick a row with `choose_video`, build `EncodeSetup { video, encoder, width, height, fps,
  kbps }`, and call `VideoEncoder::open`. If it fails, log `"{name} encoder failed to start"`, `excluded.push(video)` (the
  whole row is excluded for the session) and try the next row. On success, log `video {name} ({encoder}) WxH ...`.
- `media/src/video.rs:28-38`: `Video::Pyrowave | Video::PyrowaveCpu` goes to `pyrowave::Encoder::new(w, h,
  frame_bytes)` with `frame_bytes = kbps*1000/8/fps`, or `default_frame_bytes` (`eq:media/src/pyrowave.rs:586-589`, ≈1.5
  bpp, at least 32 KiB). The encoder raises it to at least 16 KiB (`pyrowave.rs:448`). `setup.encoder` is ignored for PyroWave today.
- `pyrowave::Encoder` (`eq:media/src/pyrowave.rs:438-511`):
  - `new` requires even sides ≥ 16 (`:441`) and creates its own `Device` (Vulkan through `libpyrowave-shared-1.dll`, located
    by `library_candidates`, `:123-165`, overridable with `ETHERQUIC_PYROWAVE`).
  - `encode(frame)` runs `rgb_to_i420` (`eq:media/src/yuv.rs:31-63`), then `encode_cpu` with `FORMAT_YUV420P`,
    `compute_num_packets(64 KiB)` and `packetize` into a buffer of `frame_bytes + 64 KiB`. It emits one **record**:
    `u32le count`, then per packet `u32le size` + bytes (`:471-511`). The record format is the wire format; the player
    does not know which encoder made it.
- FFI layouts used (`eq:media/src/pyrowave.rs:28-63`): `EncInfo{device,width,height,chroma}`,
  `CpuBuffer{data[3],stride[3],size[3],width,height,format}`, `RateControl{maximum_bitstream_size: usize}`,
  `Packet{offset,size: usize}`. These equal `R/pyrowave.h:368-381, 435-454`.

### 1.5 Announcing before the probe
`eq:src/backend.rs:201-217` (`offer_from`) announces the PyroWave row when `pyrowave_ready(gpu, library)` holds
(`:129-131`). `gpu_compute_present` (`:364-373`) checks NVIDIA, `VK_DRIVER_FILES`, `/dev/dri/renderD128` and the NixOS
Vulkan path. None of these are true on a Windows PC with only an HD 6670.

### 1.6 Packaging
`eq:packaging/windows/build-msi.ps1:3-6, 38-41` copies every `*.dll` from `-Codecs` into the install directory. PyroWave for
Windows is a MinGW DLL that `codecs.sh` does not build yet (`eq:packaging/windows/codecs.sh:5-8`); lavapipe ships as
`vulkan-cpu\` (`build-msi.ps1:42-46`). This is how AN515 runs PyroWave today: lavapipe, row `pyrowave-cpu`.

## 2. Codec naming decision

| | A. Reuse row `Video::Pyrowave` (recommended) | B. New row "PyroWave (D3D11)" |
|---|---|---|
| Wire token | `pyrowave` | new, e.g. `pyrowave-d3d11` |
| Encoder name (status/log) | `pyrowave-d3d11`, shown as `video PyroWave (GPU) (pyrowave-d3d11)` | `pyrowave-d3d11` |
| Bitstream | same (`VideoCodec::Pyrowave`) | same |
| Player changes | none; old and new players work | `play/src/codec.rs`, `ui/src/prefs.rs`, a `knows pyrowave-d3d11` line like `knows_pyrowave_cpu` (`negotiate.rs:286-338`), or old players never pick it |
| Host changes | caps probe, `video.rs` dispatch on `setup.encoder`, `backend.rs` readiness | all of A plus `negotiate.rs` (enum, `ALL`, token, name, codec), `ffmpeg.rs`, `video.rs` |
| User control | Auto order unchanged: a GPU-class PyroWave above MJPEG | the user can rank or forbid D3D11 separately |

**Recommendation: A.** It is the same bitstream and, on the HD 6670, a GPU encoder, which is what the row means
(`negotiate.rs:36-41` defines the rows by where the encoder runs). B adds a protocol token for no decoder-visible
difference. Final choice: OQ-13.

With A, if a Windows host has both a usable Vulkan GPU and D3D11, it announces only one `Pyrowave` cap, because
`HostCaps::encodes` takes the first one. Rule: a Vulkan hardware device wins; D3D11 is used only if Vulkan fails or its
device is a CPU.

## 3. Proposed host changes (task T8, in etherquic, after T7 passes)

1. **New module `media/src/pyrowave_d3d11.rs`** (Windows only, `#[cfg(windows)]`). It loads `pyrowave_d3d11.dll` with
   `libloading` like `pyrowave.rs`. The search order mirrors `library_candidates`: env `ETHERQUIC_PYROWAVE_D3D11` (a path),
   then the exe directory, then `C:\Program Files\etherquic`. It exposes `Encoder::new(w, h, frame_bytes)`,
   `encode(&Frame) -> record` (byte-identical record format, reusing `rgb_to_i420` and the same 64 KiB packet boundary),
   and `adapter() -> name/vendor/device`.
2. **Probe** (`caps.rs:run_probe`). After the Vulkan attempt, on Windows, if Vulkan failed or reported a CPU device and
   `ETHERQUIC_PYROWAVE_D3D11=off` is not set:
   - run `pyrowave_d3d11::Encoder::new(256, 144, 32 KiB)` and encode the same `test_frame`;
   - **verify the record with the existing `pyrowave::Decoder`** when one is available (it is, through lavapipe, on AN515).
     Without a decoder, verify by parsing the packet headers (`R/bitstream/bitstream.md:183-213`);
   - push `EncodeCap { video: Video::Pyrowave, encoder: "pyrowave-d3d11", limit: 7680x4320 }` (or lower, OQ-12) before
     any `PyrowaveCpu` cap, and add a note `pyrowave d3d11: <adapter>`.
   The `PyrowaveCpu` (lavapipe) cap stays, so if D3D11 fails at open time, `excluded.push(Video::Pyrowave)` falls back
   down the offer to `pyrowave-cpu`.
3. **Open** (`video.rs:28-38`): for the PyroWave rows, if `setup.encoder == "pyrowave-d3d11"`, construct the D3D11 encoder
   (new variant `VideoEncoder::PyrowaveD3d11 { inner, sink }`), otherwise as today. The `frame_bytes`/`kbps` arithmetic is
   unchanged.
4. **Readiness** (`backend.rs:129-131, 201-217, 364-373`): on Windows, `pyrowave_ready` also returns true when
   `pyrowave_d3d11.dll` is present and DXGI lists a hardware adapter with FL 11_0 (cheap to check, no device needed for
   the vendor check). The real proof is still the probe encode.
5. **Packaging**: build `pyrowave_d3d11.dll` with MSVC, statically linked CRT (`/MT`), shaders embedded as bytecode (no
   `d3dcompiler_47.dll` at runtime), and drop it into the `-Codecs` directory so `build-msi.ps1:38-41` stages it. Ship
   the license and the MIT notice of PyroWave.

## 4. C ABI of `pyrowave_d3d11.dll`

Plain C, `__cdecl`, no C++ types across the boundary, so a MinGW-built Rust host can load an MSVC DLL. Struct layouts are
**identical to `R/pyrowave.h`** (copied into `include/pyrowave_d3d11.h` under new names with static asserts on size and
offsets). The `pyrowave_result` values are reused (`R/pyrowave.h:42-55`) so `result_text` (`eq:media/src/pyrowave.rs:98-108`)
applies unchanged; `-5` reads "no usable device".

```c
#define PWD3D11_API_VERSION 1
typedef struct pwd3d11_device_t  *pwd3d11_device;
typedef struct pwd3d11_encoder_t *pwd3d11_encoder;

typedef struct pwd3d11_device_create_info {
    uint32_t struct_size;          // sizeof, for forward compatibility
    int32_t  adapter_index;        // -1: first hardware adapter with FL 11_0 and CS support
    void    *d3d11_device;         // optional ID3D11Device* to share (AddRef'd); NULL creates one
    uint32_t flags;                // bit0: debug layer, bit1: allow WARP (tests only)
} pwd3d11_device_create_info;

typedef struct pwd3d11_adapter_info { char name[128]; uint32_t vendor_id, device_id; uint64_t dedicated_video_memory; } pwd3d11_adapter_info;

uint32_t       pwd3d11_get_api_version(void);
pyrowave_result pwd3d11_create_device(const pwd3d11_device_create_info *info, pwd3d11_device *out);
pyrowave_result pwd3d11_device_get_adapter_info(pwd3d11_device, pwd3d11_adapter_info *out);
void           pwd3d11_device_destroy(pwd3d11_device);

// Same fields/semantics as pyrowave_encoder_create_info (R/pyrowave.h:368-375) with the device type replaced.
typedef struct pwd3d11_encoder_create_info { pwd3d11_device device; int width, height; pyrowave_chroma_subsampling chroma; } pwd3d11_encoder_create_info;
pyrowave_result pwd3d11_encoder_create(const pwd3d11_encoder_create_info *, pwd3d11_encoder *out);

// Same contracts as R/pyrowave_c.cpp:1389-1547 (encode_cpu), :1549-1684 (num_packets, packetize).
pyrowave_result pwd3d11_encoder_encode_cpu(pwd3d11_encoder, const pyrowave_cpu_buffer *, const pyrowave_rate_control *);
pyrowave_result pwd3d11_encoder_compute_num_packets(pwd3d11_encoder, size_t packet_boundary, size_t *num_packets);
pyrowave_result pwd3d11_encoder_packetize(pwd3d11_encoder, pyrowave_packet *packets, size_t packet_boundary,
                                          size_t *num_packets, void *bitstream, size_t size);
void           pwd3d11_encoder_destroy(pwd3d11_encoder);

// Optional: last frame's GPU stage times in microseconds (upload, dwt, quant, analyze, resolve, packing, readback).
pyrowave_result pwd3d11_encoder_get_timings(pwd3d11_encoder, float *us, size_t count);
```

Contract notes:
- `create_device` rejects software adapters: `DXGI_ADAPTER_FLAG_SOFTWARE`, or the Microsoft Basic Render Driver/WARP
  (VendorId `0x1414`) unless flag bit1 is set. It requires `D3D_FEATURE_LEVEL_11_0` and the R16_FLOAT / R32_FLOAT typed UAV
  store support that `prototype/dx11dwt.cpp:346-377` already checks.
- Entry points are not thread-safe per encoder, the same as `R/pyrowave.h:456`. The immediate context belongs to the
  device. If the host shares a device (`d3d11_device`), it must not use the context concurrently (it does not today:
  capture runs in the helper process).
- `encode_cpu` accepts `PYROWAVE_CPU_BUFFER_FORMAT_YUV420P` (required; the only format etherquic uses,
  `pyrowave.rs:66`). Other formats return `PYROWAVE_ERROR_NOT_IMPLEMENTED` until OQ-20 is decided.
- `maximum_bitstream_size` is rounded down to a multiple of 4 and includes the 8-byte header (`R/pyrowave_c.cpp:1058-1064`).
- Error on device removal (`DXGI_ERROR_DEVICE_REMOVED`/`RESET`): return `PYROWAVE_ERROR_GENERIC`. The host then excludes
  the row and falls back (`host.rs` behaviour), and the next session probes again.
- Logging: none to stdout/stderr (the service has no console); an optional `pwd3d11_set_log_callback` later if needed.
