# Port plan

## Hard requirement: bit-exact bitstream
The D3D11 encoder must produce a bitstream that the existing PyroWave decoder (Vulkan, and etherquic's players) decodes
unchanged. Target: for the same input frame and the same rate-control budget, the D3D11 packet bytes are identical to
the Vulkan encoder's (`reference/pyrowave/pyrowave_encoder.cpp` + `shaders/`). Where bit-identity is impossible
(DWT float rounding differs across GPUs - the prototype is within ~2e-3 of an fp16 CPU reference, a few fp16 ULPs), the
fallback requirement is: (a) every packet is a valid PyroWave bitstream per `reference/pyrowave/bitstream`, (b) the
quantized coefficients match the Vulkan encoder's for identical DWT input (feed the same DWT bands into both), and
(c) decoded PSNR is within 0.05 dB of Vulkan at equal size. Quant, rate control and packing are integer logic after the
DWT, so given identical bands they must match exactly.

## Stages
1. **DWT** - done as prototype (`prototype/dwt.hlsl`). Move into `src/`, keep the band layout (R16F L0-1, R32F L2-4).
2. **Quantization** (`wavelet_quant.comp`, `dwt_quant_scale.h`, 3-5 days). Replace subgroupClusteredAdd/Max,
   Shuffle/ShuffleUp/Xor with groupshared reductions; u8/u16 storage becomes packed uint RMW; atomicAdd -> InterlockedAdd.
3. **Rate control** (`analyze_rate_control*.comp`, `resolve_rate_control.comp`, part of the 3-5 days). subgroupAdd /
   InclusiveAdd -> groupshared scans; atomicMax -> InterlockedMax.
4. **Block packing** (`block_packing.comp`, 5-7 days, hardest). subgroupBallot -> groupshared bitmask build;
   u8/u16 SSBO writes -> RWByteAddressBuffer InterlockedOr on aligned words. Must match bit for bit.
5. **Host** (3-4 days). D3D11 device/queue/resources replacing Granite; staging-buffer readback; C API matching
   `pyrowave.h` so etherquic-host's Windows path can pick it when Vulkan is missing.
6. **Validation** (2-3 days). Dump per-stage buffers from the Vulkan encoder (RTX 2060 SUPER on Linux) and the D3D11
   one, diff stage by stage, then diff packets, then decode and compare PSNR.

## Build and test
- Toolchain: MSVC + Windows SDK (d3d11, d3dcompiler, fxc for cs_5_0). CMake is available on HYTE.
- Run on AN515 (HD 6670, FL11_0, no Vulkan): copy binaries over the 192.168.0.24 hop, run there.
- Timing: D3D11 timestamp queries, 200 runs, median/p90 (as `prototype/dx11dwt.cpp`). Budget at 720p: ~5-7 ms total.
- Reference timings for the Vulkan encoder: `prototype/pwbench.cpp` on the RTX / lavapipe.
