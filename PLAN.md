# Port plan

## Hard requirement: bit-exact bitstream
The D3D11 encoder must produce a bitstream that the existing PyroWave decoder (Vulkan, and etherquic's players) decodes
unchanged. Target: for the same input frame and the same rate-control budget, the D3D11 packet bytes are identical to
the Vulkan encoder's (`reference/pyrowave/pyrowave_encoder.cpp` + `shaders/`). Bit-identity is impossible for the DWT
(float rounding differs across GPUs; the prototype is within ~2e-3 of an fp16 CPU reference, a few fp16 ULPs). There the
fallback requirement is:
(a) every packet is a valid PyroWave bitstream per `reference/pyrowave/bitstream`;
(b) the quantized coefficients match the Vulkan encoder's for identical DWT input (feed the same DWT bands into both);
(c) decoded PSNR is within 0.05 dB of Vulkan at equal size.
Given identical bands, quant, rate control and packing must match exactly, apart from the float-sensitive points listed in
`docs/SPEC.md` §6, which Vulkan itself does not pin down across GPUs. The precise definition is in `docs/SPEC.md` §1 and §5.

## Stages
Every stage has a normative spec in `docs/stages/`; tasks and acceptance criteria are in `AGENTS.md` §5.
0. **Input** (`docs/stages/00_input.md`). I420 planes uploaded as R8_UNORM textures.
1. **DWT** (`01_dwt.md`). Done as a prototype (`prototype/dwt.hlsl`). Move it into `src/` and keep the band layout (R16F L0-1,
   R32F L2-4). Task T1.
2. **Quantization** (`02_quant.md`, `wavelet_quant.comp`, `dwt_quant_scale.h`, 3-5 days). Replace subgroupClusteredAdd/Max
   and Shuffle/ShuffleUp/Xor with groupshared reductions that sit outside divergent code. u8 payload stores become
   groupshared byte staging plus aligned word stores into per-group allocations. atomicAdd becomes InterlockedAdd. T2a-T2c.
3. **Rate control** (`03_analyze_rate_control.md`, `04_resolve_rate_control.md`, part of the 3-5 days). subgroupAdd and
   InclusiveAdd become groupshared scans in a fixed order; atomicMax becomes InterlockedMax. The windowed finalize scan
   is reproduced bug-compatibly (OQ-5). T3, T4.
4. **Block packing** (`05_block_packing.md`, `block_packing.comp`, 5-7 days, hardest). subgroupBallot becomes a groupshared
   InterlockedOr mask. Every packet is assembled in groupshared memory and stored as whole words, because cs_5_0 has no
   byte stores. It must match bit for bit. T5a, T5c.
5. **Host** (`06_frame_and_host.md`, `INTEGRATION.md`, 3-4 days). D3D11 device and resources replacing Granite, staging
   buffer readback, CPU packetize, and a C ABI (`pwd3d11_*`) that mirrors `pyrowave.h`, so etherquic-host's Windows path
   can pick it when Vulkan is missing or CPU-only. T5b, then T8 in etherquic.
6. **Validation** (`VALIDATION.md`, 2-3 days). Dump per-stage buffers from the Vulkan encoder (RTX 2060 SUPER on Linux) and
   the D3D11 one, diff stage by stage (with injected Vulkan bands), then diff packets, then decode and compare PSNR. T6a-T6d,
   then T7 on AN515.

## Build and test
- Toolchain: MSVC + Windows SDK (d3d11, dxgi, fxc for cs_5_0). CMake is available on HYTE. Flags are in `AGENTS.md` §4.
- Run on AN515 (HD 6670, FL11_0, no Vulkan) only in task T7: copy binaries over the 192.168.0.24 hop and run them there.
- Timing: D3D11 timestamp queries, 20 warm-up + 200 frames, median/p90 per stage (`VALIDATION.md` §9). Budget at 720p:
  ~5-7 ms total.
- Reference timings for the Vulkan encoder: `prototype/pwbench.cpp` on the RTX and on lavapipe.

## Status
- Spec and onboarding docs: done.
- DWT prototype: done (`prototype/`).
- Everything else: not started. Open decisions are in `docs/OPEN_QUESTIONS.md`.
