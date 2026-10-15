# Stage 0: input planes and colour conversion

Status: spec. No code yet. Read `docs/SPEC.md` first; this file assumes its conventions (`R/` = `reference/pyrowave/`,
`eq:` = etherquic @ cecf3f4, the checkout on 192.168.0.24).

## What the reference does

PyroWave itself does no colour conversion on the CPU path. etherquic converts RGB to I420 on the CPU and hands
PyroWave three 8-bit planes:

- `eq:media/src/yuv.rs:31-63` `rgb_to_i420`: integer full-range BT.601-style matrix. Y = `(77R + 150G + 29B + 128) >> 8`,
  chroma from the 2x2 box average, `Cb = ((-43R - 85G + 128B + 128) >> 8) + 128`, `Cr = ((128R - 107G - 21B + 128) >> 8) + 128`,
  clamped to 0..255.
- `eq:media/src/pyrowave.rs:407-422` `planes_buffer`: `PYROWAVE_CPU_BUFFER_FORMAT_YUV420P` (= 1), strides `w, w/2, w/2`,
  sizes `w*h, (w/2)*(h/2)` x2. Chroma is `CHROMA_420` (`pyrowave.rs:66-67`).
- `eq:media/src/pyrowave.rs:441-443`: the host only builds an encoder for even sides of 16 or more.
- `R/pyrowave_c.cpp:1389-1547` `pyrowave_encoder_encode_cpu`:
  - rejects a size other than the encoder's, and 4:4:4 data for a 4:2:0 encoder (1399-1408);
  - sets `ycbcr_scaling_factor` to 1.0 for 8-bit and 16-bit formats and to `0xffff/0x3ff` for the P10 formats (1410-1418);
  - creates one sampled image per plane: `R8_UNORM` (8-bit), `R16_UNORM` (P10/P16), or `R8G8_UNORM` for the NV12 chroma
    plane (1445-1476). Chroma images are `width/2 x height/2` for 4:2:0 (1467-1471);
  - copies rows with the row length `row_stride / bpp` (1522-1525), then calls `pyrowave_encoder_encode_gpu` (1545).
- `R/pyrowave_c.cpp:1159-1170`: `views.range_scale = ycbcr_scaling_factor`. The DWT multiplies by it on first entry.

## D3D11 contract (v1)

Required for etherquic: `YUV420P`, 8-bit, 4:2:0. Everything else is optional (see the table below).

| Item | Value |
|---|---|
| Plane textures | 3 x `ID3D11Texture2D`, `DXGI_FORMAT_R8_UNORM`, Y `W x H`, Cb/Cr `W/2 x H/2`, `MipLevels 1`, `ArraySize 1` |
| Usage | `D3D11_USAGE_DEFAULT`, `BIND_SHADER_RESOURCE`. Upload with `UpdateSubresource` or a `D3D11_USAGE_STAGING` texture + `CopyResource` (the prototype measures both, `prototype/README.md` timings table) |
| Views | One SRV per plane. The prototype DWT declares `Texture2DArray<float>` (`prototype/dwt.hlsl:14`), so create the SRV as `D3D11_SRV_DIMENSION_TEXTURE2DARRAY`, `ArraySize 1` (`prototype/dx11dwt.cpp:118-124`) |
| `range_scale` | 1.0 for 8-bit. Passed in the DWT cbuffer (`prototype/dwt.hlsl:11`) instead of the Vulkan specialization constant (`R/shaders/dwt.comp:15`) |
| Row pitch | Honour `row_stride_in_bytes[i]`; the reference requires `stride >= width*bpp` and `stride*height <= plane_size` (`R/pyrowave_c.cpp:1437-1440`). Return the same errors |

Optional formats, for parity with the C API later (not needed by etherquic):

| Format | Reference | D3D11 mapping |
|---|---|---|
| NV12 | `R8G8_UNORM` chroma, Cr read through a G swizzle (`R/pyrowave_c.cpp:1508`) | D3D11 views have no swizzle. Create one `R8G8_UNORM` SRV and give the DWT a cbuffer channel selector (0 = .r, 1 = .g). That is a shader change and must keep Gather semantics (use `GatherGreen` for the G channel) |
| YUV420P10 / 444P10 | `R16_UNORM`, `range_scale = 65535/1023` | `R16_UNORM` SRV, same `range_scale` |
| YUV420P16 / 444P16 | `R16_UNORM`, `range_scale = 1` | same |
| 4:4:4 | all planes full size, the DWT runs level 0 for all 3 components (`R/pyrowave_encoder.cpp:538-550`) | same dispatch shape |

## Features used and cs_5_0 replacements

None. The stage is a texture upload. The only conversion is UNORM8 to float on sampling, which the DWT does.

## Barrier rules

Host side only: the upload (`UpdateSubresource`/`CopyResource`) must come before the first DWT dispatch on the same
immediate context. D3D11 orders those for you. With two frame contexts (see stage 6), never overwrite a plane texture
that an unfinished frame still samples. Keep one plane set per context, as `R/pyrowave_c.cpp:886` does.

## Rounding

UNORM8 to float is `c / 255` in both APIs. Whether both produce the same float32 for all 256 codes is checked, not
assumed: test T-IN-1.

## Test oracle

- T-IN-0: plane bytes uploaded equal the bytes the host produced (read back the texture, compare). Exact.
- T-IN-1: sample all 256 codes through the DWT's sampling path (Gather on an `R8_UNORM` SRV) on the HD 6670 and compare the
  float bits with `float(c)/255.0f` computed on the CPU, and with the same probe on the Vulkan reference. Exact match is
  expected; any mismatch is recorded in `docs/OPEN_QUESTIONS.md` and makes DWT input differ before any arithmetic.
- The `rgb_to_i420` conversion is outside the encoder and identical in both paths, because it is the same Rust code.
