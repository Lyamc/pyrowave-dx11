# proto/dx11-dwt: PyroWave forward DWT on D3D11 (feasibility)

This prototype ports PyroWave's forward DWT stage (`shaders/dwt.comp` @ a0d2e71, the revision `nix/pyrowave.nix` pins) to
D3D11 cs_5_0, to see whether a full D3D11 port is worth doing for AN515's Radeon HD 6670 (TeraScale 2, FL 11_0, no Vulkan).

Files:
- `dwt.hlsl`: the HLSL port, kept line-for-line close to the GLSL.
- `dx11dwt.cpp`: the harness. It holds the CPU reference, checks correctness, times the DWT with timestamp queries over 200 runs, and times the upload.
- `pwbench.cpp`: runs PyroWave's own encoder through its C API, loaded at runtime, and prints PyroWave's per-stage GPU timestamps.
- `pyrowave.h`: PyroWave's public header (MIT), needed by pwbench. pwbench also needs Vulkan-Headers on the include path.

Build on Windows: `cl /O2 /EHsc /arch:AVX2 dx11dwt.cpp` and `cl /O2 /EHsc /std:c++17 /I<vk headers> pwbench.cpp`.
Build on Linux: `clang++ -O2 -I<vk headers> pwbench.cpp -ldl`, then run `pwbench <libpyrowave-shared.so.1> <64-bit libvulkan.so.1> W H N`.

## Transform (what PyroWave does, and what the port reproduces)
- Wavelet: CDF 9/7 lifting (alpha -1.586134, beta -0.052980, gamma 0.882911, delta 0.443507, K 1.230174). Low-pass is scaled by 1/K and high-pass by K.
- Levels: 5 (DecompositionLevels). Luma is padded to a multiple of 32 (720 becomes 736, 1080 becomes 1088).
- 4:2:0: luma level 0 comes from the Y plane. Cb and Cr enter at level 1 from their half-resolution planes; there is no chroma level 0. Levels 1 to 4 then run for all 3 components. The input is DC-shifted by -0.5 on first entry.
- Tiling: each 64-thread workgroup handles a 32x32 block plus a 4-texel apron. The block is loaded with textureGather and a mirrored-repeat sampler, using a custom mirror coordinate fixup (whole-sample symmetric extension). Two rows are packed per vec2.
- Passes: horizontal 8x2 lifting, then 4x2 lifting on the apron rows, then a transposed write. The vertical pass reuses the same code, and the result is written to 4 bands (LL, H-high, V-high, HH) of an image array.
- Precision (default build, PRECISION=1): fp32 math, with groupshared data stored as fp16 through packHalf2x16. Band storage is R16F for levels 0 and 1 and R32F for levels 2 to 4. HLSL f32tof16/f16tof32 maps 1:1, so no fp16 ALU or 16-bit storage is needed.
- Subgroups: the DWT uses them only to build `local_index` (equal to SV_GroupIndex). All data exchange already goes through shared memory and barriers. 8/16-bit storage is not used in the DWT. The other stages use these features heavily (see below).

## Correctness (HD 6670, synthetic test image: gradients, checkerboard and noise)
Max abs error of the GPU output against a CPU reference that runs the same addressing and lifting with fp16 rounding at the same points, across all levels, bands and components:
- 720p: 2.4e-3. 1080p: 1.7e-3. Coefficients have magnitude of about 0.5, so this is a few fp16 ULPs from rounding differences.
- Against a pure fp32 CPU reference, the error is 2.0e-3 (720p) and 1.6e-3 (1080p). That is the same order as PyroWave's own fp16 shared-memory path.

## Timings (median / p90, ms)
| | 720p | 1080p |
|---|---|---|
| HD 6670 D3D11 DWT (5 levels, 200 runs, timestamps) | 0.870 / 0.878 | 1.778 / 1.788 |
| HD 6670 upload, CopyResource on GPU (~3 GB/s) | 0.471 / 0.472 | 1.011 / 1.012 |
| HD 6670 upload, wall time Map+memcpy+copy to done | 0.652 / 0.676 | 1.286 / 1.336 |
| lavapipe on AN515 (5950X, LP_NATIVE_VECTOR_WIDTH=512), PyroWave DWT | 7.63 | 18.80 |
| lavapipe, PyroWave whole encode (wall per frame) | 21.9 / 23.5 | 37.9 / 39.6 |
| RTX 2060 SUPER (Linux), PyroWave DWT | 0.064 | 0.094 |
| RTX 2060 SUPER, sum of all GPU stages / wall per frame | 0.27 / 0.64 | 0.41 / 1.01 |

Lavapipe per-stage breakdown at 720p: DWT 7.6, Quant 5.9, Analyze 3.5, Packing 3.4, Resolve 0.4 ms. At 1080p: DWT 18.8, Quant 9.4, Analyze 3.9, Packing 3.8, Resolve 0.5 ms.
The DWT's share of the encode is 36% (720p) and 51% (1080p) on lavapipe, but only about 24% on the RTX.

## Full-encode estimate on the HD 6670
The HD 6670 runs the DWT 13.6x (720p) to 19x (1080p) slower than the RTX 2060 SUPER. Scaling the RTX's other stages (0.20 / 0.31 ms) by that ratio, then adding a 1.5-2x penalty for emulating subgroup ops and for TeraScale's slow atomics, gives:
- 720p: DWT 0.87 + other stages 3-5.5 + upload 0.65 + bitstream readback ~0.2 = about 5-7 ms, or roughly **140-200 fps** (GPU busy time; frames can be pipelined).
- 1080p: DWT 1.78 + other stages 6-11 + upload 1.3 + readback ~0.3 = about 9.5-14 ms, or roughly **70-105 fps**.
- A lower bound from lavapipe's DWT share (DWT / share) gives 2.4 ms (720p) and 3.5 ms (1080p), so the band above is conservative.
For comparison, lavapipe on the 5950X manages 46 fps at 720p and 26 fps at 1080p while saturating the CPU.

## Porting the remaining stages
The other stages lean on Vulkan features that D3D11 FL 11_0 lacks, so they need real rework rather than translation:
- `wavelet_quant` (128 threads): subgroupClusteredAdd/Max, Shuffle, ShuffleUp/Xor, fp16 and u8/u16 storage, and atomicAdd.
- `block_packing` (the bit-plane entropy packing, 64 threads): subgroupBallot, ClusteredAdd, Shuffle/ShuffleUp, u8/u16 SSBO writes, and atomicOr/And/Add.
- `analyze_rate_control`: subgroupAdd, ShuffleXor, atomicAdd. `resolve_rate_control`: InclusiveAdd, Shuffle, atomicMax.
In D3D11 these become groupshared scans and reductions with barriers, which TeraScale's 64-wide wavefronts handle reasonably. Sub-32-bit stores become packed uint read-modify-write or InterlockedOr on RWByteAddressBuffer. Readback goes through staging buffers, and the host side needs a D3D11 replacement for Granite's command and resource management.
Effort: about 2-3 weeks for one engineer to reach a bit-exact bitstream. That breaks down as roughly 3-5 days for quant and rate control, 5-7 days for block_packing (the hardest part, which must match the bitstream bit for bit), 3-4 days for host plumbing and integration into etherquic-host's Windows path, and 2-3 days for validating against the Vulkan encoder on the RTX.
