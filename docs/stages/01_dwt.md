# Stage 1: forward DWT

Status: done as a prototype (`prototype/dwt.hlsl`, `prototype/dx11dwt.cpp`); task T1 moves it into `src/`.
Source of truth: `R/shaders/dwt.comp`, `R/shaders/dwt_common.h`, `R/shaders/dwt_swizzle.h`,
`R/pyrowave_encoder.cpp:494-602` (`Encoder::Impl::dwt`), `R/pyrowave_common.cpp:133-189` (band images).

## Variant being ported

`R/CMakeLists.txt:39-47`: the default build is `PYROWAVE_FP32_MATH=ON` -> `PYROWAVE_PRECISION=1`: fp32 lifting math,
fp16 shared-memory storage. The variant with `FP16=0` stores shared memory as `uint` with `packHalf2x16`
(`R/shaders/dwt_common.h:21-29, 45-47`), which is what the prototype mirrors with `f32tof16`. On devices with `shaderFloat16`
the reference picks `FP16=1` instead (`R/pyrowave_common.cpp:282-286`): shared memory is `f16vec2` and the conversion is a
plain `f16vec2(v)` cast. Both round fp32 to fp16 at the same points; whether the two conversions round the same way is
open question OQ-3.

`get_precision()` can be overridden at run time by `PYROWAVE_PRECISION` (`R/pyrowave_common.cpp:15-28`). The port
supports precision 1 only. The Vulkan reference used for validation must run with `PYROWAVE_PRECISION` unset or set to 1.

## Inputs

| Input | Layout |
|---|---|
| Level 0 Y (and level 1 Cb/Cr for 4:2:0) | the stage-0 plane SRVs, `R8_UNORM`, sampled with a point/mirror sampler. `D3D11_TEXTURE_ADDRESS_MIRROR` matches `VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT` (`R/pyrowave_common.cpp:41-50`, `prototype/dx11dwt.cpp:373-377`) |
| Levels 1..4 | the LL layer (layer 0) of the previous level of the same component (`R/pyrowave_encoder.cpp:576-579`; `component_ll_views` = `base_layer 4*c`, 1 layer, `R/pyrowave_common.cpp:176-187`) |
| cbuffer `Push` | `int2 resolution; float2 inv_resolution; int2 aligned_resolution; uint dc_shift; float range_scale;` (`prototype/dwt.hlsl:5-12`). The first three fields are the Vulkan push constants (`R/shaders/dwt.comp:17-22`); `dc_shift` and `range_scale` replace specialization constants 0 and 1 (`R/shaders/dwt.comp:14-15`) |

Per-dispatch values (from `R/pyrowave_encoder.cpp:514-596`, reproduced in `prototype/dx11dwt.cpp:216-232`):

| Dispatch | source | `resolution` | `aligned_resolution` | `dc_shift` / `range_scale` | groups |
|---|---|---|---|---|---|
| L0, Y | Y plane | `W, H` | `AW, AH` | 1 / `range_scale` | `ceil(AW/32) x ceil(AH/32)` |
| L1, Cb and Cr (4:2:0) | chroma plane | `W/2, H/2` | `AW/2, AH/2` | 1 / `range_scale` | `ceil(AW/2/32) x ...` |
| L1, Y; L2..L4, all | LL of level-1 | LL view size | same as `resolution` | 0 / 1.0 | `ceil(w/32) x ceil(h/32)` |

`AW = max(align(W, 32), 128)`, `AH` likewise (`R/pyrowave_common.cpp:253-256`). `inv_resolution = 1.0f / resolution`,
computed on the CPU (`R/pyrowave_encoder.cpp:529-530`). Order: level 0, then for each level 1..4 components 0, 1, 2.
The reference puts one barrier per level (`R/pyrowave_encoder.cpp:591-592`); D3D11 orders dispatches implicitly.

## Outputs (band storage)

The reference stores bands in two image arrays with mip levels: `R16_SFLOAT`, `AW/2 x AH/2`, 2 mips, 12 layers, for
levels 0 and 1, and `R32_SFLOAT` for levels 2..4 (`R/pyrowave_common.cpp:133-157`). Layer = `4*component + band`.

The port uses the prototype's layout, which holds the same values: one `Texture2DArray` per (component, level) with
4 layers (`prototype/dx11dwt.cpp:126-135, 210-214`). That means 13 textures for 4:2:0 (no chroma level 0) and 15 for 4:4:4.

| Level l | size | format |
|---|---|---|
| 0, 1 | `AW >> (l+1)` x `AH >> (l+1)` x 4 layers | `DXGI_FORMAT_R16_FLOAT` |
| 2, 3, 4 | same formula | `DXGI_FORMAT_R32_FLOAT` |

Band layer meaning: 0 = LL, 1 = HL, 2 = LH, 3 = HH (`R/bitstream/bitstream.md:497-502`). The write pattern is
`R/shaders/dwt.comp:222-239` (`prototype/dwt.hlsl:182-195`). Every texel of every band is written. Each 32x32 input
tile produces 16x16 per band, and the dispatch covers the aligned size, so no texel is left uninitialized.

Each texture needs a UAV (`RWTexture2DArray<float>`, 4 layers) for this stage, an SRV with 4 layers for quantization,
and an SRV of layer 0 only (as a 1-layer `Texture2DArray`) for the next DWT level. Typed UAV *stores* of
`R16_FLOAT` are supported at FL11_0; the prototype checks this at startup (`prototype/dx11dwt.cpp:361-363`). No stage ever
does a typed UAV *load* of a band. Bands are read only through SRVs, so the FL11_0 "typed UAV loads only for R32"
limit does not apply.

## GLSL features and cs_5_0 replacements

| GLSL | where | cs_5_0 |
|---|---|---|
| `gl_SubgroupID * gl_SubgroupSize + gl_SubgroupInvocationID` | `dwt.comp:203` | `SV_GroupIndex` (1-D group of 64). Not a subgroup algorithm; the reference sets subgroup size 4..128 only so this index is valid (`R/pyrowave_encoder.cpp:506-507`) |
| `textureGather(sampler2D, uv)` | `dwt.comp:45-48, 68, 78, 89` | `Texture2DArray.Gather(SamplerState, float3(uv, 0))`. Component order is the same in both APIs: x=(i0,j1) y=(i1,j1) z=(i1,j0) w=(i0,j0). The `.wzxy` swizzle is kept |
| `packHalf2x16` / `unpackHalf2x16` (FP16=0) or `f16vec2` (FP16=1) | `dwt_common.h:44-51` | `f32tof16` / `f16tof32` on a `groupshared uint` (`prototype/dwt.hlsl:30-42`) |
| `imageStore(image2DArray, ivec3, vec4)` | `dwt.comp:234-237` | `RWTexture2DArray<float>[int3] = v` |
| `barrier()` | `dwt.comp:124, 176, 207, 215, 220` | `GroupMemoryBarrierWithGroupSync()` |
| specialization constants `DCShift`, `RangeScale` | `dwt.comp:14-15, 50-51` | cbuffer fields `dc_shift`, `range_scale`. The GLSL multiplies only when `RangeScale != 1.0`; multiplying by exactly 1.0 is an identity in IEEE fp32, so the prototype's unconditional multiply gives the same values |

No 8/16-bit storage, no atomics, no other subgroup operations.

## Barrier correctness

All five barriers are in uniform control flow in the GLSL. Note `forward_transform4x2` (`dwt.comp:146-199`), which keeps
its barrier *outside* the `active_lane` branches (176). The prototype keeps the same structure
(`prototype/dwt.hlsl:175-180` plus the in-function barriers). Rules for T1:

- Do not move any barrier into `if (active_lane)` or into the output loops.
- The barrier inside `forward_transform8x2` (`dwt.comp:124`) protects the transposed write against other threads'
  pending reads (write-after-read). It stays even though no thread reads after it in the same phase.
- Group size is 64, one TeraScale wavefront. Lockstep is still not assumed: the barriers stay.

## Rounding semantics

- Lifting is fp32 with fp16 rounding at every shared-memory store (`store_shared`), which is 4 stores per value per pass.
  `f32tof16` rounding mode: OQ-3.
- `a += c * (b + d)` may be contracted to a fused or unfused `mad` by fxc and by the driver. The GLSL is not `precise`
  either, so this is not bit-exact across GPUs in the reference either. Accepted: PLAN's fallback requirement.
- The band store converts fp32 to fp16 for levels 0-1 (UAV format conversion). Its rounding mode is OQ-3.
- Mirror coordinate fixup: `generate_mirror_uv` (`dwt.comp:28-37`) is integer arithmetic followed by
  `vec2(coord) * inv_resolution`. With point sampling and whole-texel coordinates, the footprint chosen by Gather is
  stable under the small error in `coord * inv_resolution`, because the sample point sits half a texel from any
  footprint boundary.

## Test oracle

- T-DWT-1 (exists): the prototype's CPU fp16-emulating reference. Max abs error ≤ 4e-3 over all levels, bands and components
  (measured 2.4e-3 at 720p and 1.7e-3 at 1080p, `prototype/README.md`).
- T-DWT-2 (new): compare against bands dumped from the Vulkan reference for the same planes (`docs/VALIDATION.md` §3).
  Report max abs error and the fp16-ULP histogram per (component, level, band). Pass: max abs ≤ 4e-3 and ≥ 99% of texels
  within 1 fp16 ULP for R16F levels (the ULP target is a proposal; record the measured value in OPEN_QUESTIONS OQ-11).
- DWT differences propagate into quantization. That is why every later stage is validated with *injected* Vulkan
  bands, not with D3D11 bands (VALIDATION §3.3, mode M2 in §5).
