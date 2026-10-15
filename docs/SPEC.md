# PyroWave encoder on Direct3D 11 (cs_5_0): authoritative specification

Status: spec. No port code exists yet. This file and `docs/stages/*.md` are normative. Where they disagree with
`reference/pyrowave/`, the reference wins and the spec is the thing that is wrong. File an entry in
`docs/OPEN_QUESTIONS.md` and fix the spec.

## 0. Conventions

- `R/` means `reference/pyrowave/` (upstream Themaister/pyrowave @ a0d2e71, read-only). `R/x:N` is file x, line N.
- Shader citations, all under `R/shaders/`: `wq:N` = `wavelet_quant.comp`, `ar:N` = `analyze_rate_control.comp`,
  `arf:N` = `analyze_rate_control_finalize.comp`, `rr:N` = `resolve_rate_control.comp`, `bp:N` = `block_packing.comp`.
- `eq:` means etherquic @ cecf3f4 (checkout `~/Build/etherquic` on 192.168.0.24; github.com/Lyamc/etherquic).
- `W, H`: picture size. `AW = max(align(W, 32), 128)`, `AH` likewise (`R/pyrowave_common.cpp:253-256`). Band size at
  level l is `AW >> (l+1)` x `AH >> (l+1)` (`R/bitstream/bitstream.md:144-152`).
- Precision variant: `PYROWAVE_PRECISION = 1` only (the default; `R/CMakeLists.txt:39-47`).
- Chroma: 4:2:0 is required (what etherquic sends, `eq:media/src/pyrowave.rs:66-67`). 4:4:4 is optional.
- "FS-xx" marks a float-sensitive point, where bit-identity with Vulkan cannot be guaranteed by construction (§6).
- "Bug-compatible" means the reference does something that looks unintended, and the port reproduces it on purpose.

## 1. Requirements

| # | Requirement | Source |
|---|---|---|
| RQ1 | Every frame is a valid PyroWave v1 bitstream that the existing Vulkan decoder accepts and decodes | user goal; `R/bitstream/bitstream.md`; `R/pyrowave_decoder.cpp:157-261` |
| RQ2 | Given identical DWT bands, rate budget and sequence number, the packetized bytes equal the Vulkan encoder's, ignoring the bits the reference leaves undefined (§5). Target device for this comparison: the RTX 2060 SUPER (subgroup 32) | PLAN.md "Hard requirement" |
| RQ3 | Where RQ2 fails only at a documented FS point, decoded PSNR is within 0.05 dB of Vulkan at equal or smaller size | PLAN.md fallback (c) |
| RQ4 | Runs on a Radeon HD 6670 (TeraScale 2) at FL 11_0 with `cs_5_0` only | user constraints |
| RQ5 | Same rate-control contract: `8 + 4*Σ num_words ≤ maximum_bitstream_size & ~3` | `R/pyrowave_c.cpp:1058`, `R/pyrowave_encoder.cpp:293-294` |
| RQ6 | Callable from etherquic as described in `docs/INTEGRATION.md` | |

## 2. Target constraints (FL 11_0, cs_5_0, TeraScale 2)

| Constraint | Consequence |
|---|---|
| No wave intrinsics (they need SM 6.0) | every `subgroup*` op becomes groupshared memory plus `GroupMemoryBarrierWithGroupSync` (catalogue §3) |
| No 16-bit types, no `min16*` used | `float16_t` becomes `f32tof16`/`f16tof32` packed in `uint`; `uint16_t` becomes masks/shifts |
| No byte/short UAV stores (no `Store8`/`Store2`; `RWByteAddressBuffer` stores 32-bit words only) | bytes are assembled in groupshared memory and written as whole words owned by one group (stages 2 and 5) |
| Typed UAV loads only for R32 formats | bands are read through SRVs only; all buffers are raw (`ByteAddressBuffer`) |
| 32 KB groupshared, ≤ 1024 threads/group | largest use: packing 10.5 KB, quant < 9 KB, finalize 2 KB (512 threads) |
| 64-wide wavefronts | never relied on for synchronization |
| 8 UAV slots for the compute stage at FL 11_0 | max 3 used by any stage |
| Barriers only in uniform control flow (`X3663`) | no `return` before a barrier, no barrier in data-dependent loops (§4) |
| No fp16 ALU, no `fma` guarantee (`mad` may or may not be fused) | `precise` where the GLSL has `precise`; exact bit tricks for powers of two |

## 3. Replacement catalogue (applies to every stage)

| GLSL | cs_5_0 replacement | Exactness |
|---|---|---|
| `gl_SubgroupID*gl_SubgroupSize + gl_SubgroupInvocationID` (1-D groups) | `SV_GroupIndex` | exact |
| `subgroupClusteredAdd(int/uint, N)`, `subgroupAdd(uint)` | write `gs[t]`; barrier; integer sum of the N entries of the cluster | exact |
| `subgroupClusteredAdd(float, N)`, `subgroupAdd(float)` | same, summed in the xor-butterfly tree order `((e0+e1)+(e2+e3))+…` | matches a ShuffleXor 1, 2, 4(, 8) butterfly bit for bit; other orders are FS |
| `subgroupClusteredMax` | groupshared, max of N entries | exact (order-free) |
| `subgroupShuffleUp` scans (`inclusive_*_clustered*`) | serial prefix over ≤16 groupshared entries, or Hillis-Steele with read/barrier/write/barrier | exact |
| `subgroupShuffle(x, lane | M)` broadcast | groupshared slot written by the source lane, read after a barrier | exact |
| `subgroupShuffleXor` OR-merge of disjoint bit fields | OR of the cluster's groupshared entries | exact |
| `subgroupBallot` | `InterlockedOr` into a zeroed `groupshared uint` | exact |
| `subgroupInclusiveAdd(uint)` over the group | groupshared scan | exact |
| `subgroupBarrier()`, `barrier()` | `GroupMemoryBarrierWithGroupSync()` (strictly stronger) | |
| `atomicAdd/atomicMax` on SSBO | `RWByteAddressBuffer.InterlockedAdd/InterlockedMax` (signed max chosen by an `int` operand) | exact; ordering differences only move scratch allocations |
| `atomicAnd/atomicOr` on `shared` | `InterlockedAnd/InterlockedOr` on `groupshared` | exact |
| `uint8_t`/`uint16_t` SSBO store | groupshared staging + `InterlockedOr` at byte position, then word `Store` | exact |
| `uint8_t`/`uint16_t` SSBO load | aligned `Load`, shift, mask | exact |
| `float16_t` store/load | `f32tof16` / `f16tof32` | rounding: OQ-3 |
| `findMSB(x ≥ 0)` | `firstbithigh(uint(x))`; 0 gives `0xFFFFFFFF` = -1 | exact (test T-Q-0) |
| `findLSB`, `bitCount`, `bitfieldExtract/Insert` | `firstbitlow`, `countbits`, shift/mask | exact |
| `frexp`, `ldexp` with integer exponent | bit extraction / `asfloat((127+k) << 23)` | exact |
| `textureGather[Offset]` | `Gather[Red]` with the same offsets, or integer `Load` (out of bounds returns 0 = transparent-black border) | exact addressing |
| `imageStore` to R16F/R32F array | `RWTexture2DArray<float>` store | R16F conversion rounding: OQ-3 |
| push constants | `cbuffer` with the same member order; each push struct is laid out so HLSL packing equals C packing (checked per stage) | |
| specialization constants | `static const` or a cbuffer field (`SkipQuantScale` = false, `DCShift`, `RangeScale` per dispatch) | |

## 4. Barrier-correctness rules (apply to every stage)

- B1. `GroupMemoryBarrierWithGroupSync` must be reached by all threads. It may appear only at top level or in loops whose
  trip count is a literal or cbuffer value. GLSL early `return`s (quant `wq:194-203, 314`; resolve `rr:41-59`) become
  predicate flags.
- B2. Any groupshared value written by one thread and read by another has a barrier between the write and the read. Reusing
  an array needs a barrier before the overwrite (write-after-read), as in `R/shaders/dwt.comp:123-124`.
- B3. Within a dispatch, groups communicate only through atomics, and nothing may depend on the order atomics execute in.
  Across dispatches, D3D11 makes UAV writes visible to the next `Dispatch`.
- B4. Never use the TeraScale 64-lane wavefront as a barrier, even for 64-thread groups.
- B5. Build all shaders with `fxc /T cs_5_0 /WX`, so that X3663 and loop-unroll warnings fail the build.
- B6. A device-memory word that two groups can both touch is written only with atomics. Scratch layouts are chosen so
  this never happens (word-aligned per-group allocations).

## 5. Bit-exactness definition

- The packetized frame (sequence header + packets in block index order) is compared byte for byte, after masking the
  don't-care bits: (a) bytes between the parsed end of a packet's sign payload and `payload_words*4`; (b) bits above the
  last sign bit in the last sign byte. The reference never initializes either (`R/shaders/block_packing.comp:180, 361-378`;
  `R/bitstream/bitstream.md:202-204`); the port writes zeros. The mask comes from the CPU packet parser (stage 5 §9).
- Scratch buffers (payload, bitstream before packetization, counters) are compared **semantically** (per block, through
  the meta offsets), never as raw bytes. Their order comes from atomics and differs from run to run even within Vulkan.
- `sequence` must be equal. The harness starts both encoders from the same `sequence_count` (VALIDATION §3).

## 6. Float-sensitive points (complete list)

| Id | Where | What can differ | Effect |
|---|---|---|---|
| FS-D | DWT lifting, fp16 shared, `mad` contraction | DWT bands (~1e-3) | everything downstream; accepted (PLAN) |
| FS-Q1 | `wq:107` `m_wave / target` -> `ceil` | per-8x8 quant code | code word, payload, stats |
| FS-Q2 | `wq:109` `1/(code/8+0.25)` | per-8x8 scale | quantized integers |
| FS-Q3 | `wq:127, 243, 254` float cluster sums and `dot` | stats f16 | rate control only |
| FS-Q4 | `wq:184-185` `1/(qr*qs)`, squared | stats | rate control only |
| FS-A1 | `ar:81-82` division, `log2`, `+0.5` truncation | bucket index | which blocks lose planes |
| FS-A2 | `ar:186-202` 16-lane float sum (subgroupAdd on lavapipe vs butterfly on RTX) | distortion per 32x32 | bucket index |
| FS-C | `R/pyrowave_encoder.cpp:111-146` `rdo_distortion_scale` via libm (`exp`, `pow`, `sqrt`) | stats | removed by embedding the reference table (stage 2 §2) |
| FS-H | f16 rounding (`f32tof16` vs `float16_t`/`packHalf2x16`, R16F store) | DWT, stats | OQ-3 |

Integer logic after quantization (code words, payload, cost, analyze integers, resolve, packing, packetization) must be
exact.

## 7. Pipeline

```
I420 planes (CPU, eq:media/src/yuv.rs:31)
  └─ stage 0 upload: 3 x R8_UNORM textures
      └─ stage 1 DWT: 13 dispatches -> 13 band arrays (4 layers; R16F L0-1, R32F L2-4)
          └─ stage 2 quant: 42 dispatches -> block meta (8 B/8x8), block stats (64 B/8x8), payload bytes
              └─ stage 3 analyze: 42 dispatches + finalize -> bucket buffer (consumed, savings, RD ops)
                  └─ stage 4 resolve: 1 dispatch (2048 groups) -> quant[32x32]
                      └─ stage 5 packing: 42 dispatches -> bitstream packets + BitstreamPacket meta
                          └─ stage 6 readback + CPU packetize -> sequence header + packets
```

Per-frame buffers, sizes and the command order are in `docs/stages/06_frame_and_host.md` §1-2.

## 8. Per-stage summary

Each row is detailed in its stage document (inputs and outputs with exact layouts, feature replacements, barrier rules,
rounding, oracle).

| Stage | Doc | Group | Inputs -> outputs | Subgroup / atomic / 8-16-bit features replaced | Barrier risk | Exact? | Oracle |
|---|---|---|---|---|---|---|---|
| 0 input | `stages/00_input.md` | n/a | I420 -> R8 SRVs | none | none | yes | T-IN-0/1 |
| 1 DWT | `stages/01_dwt.md` | 64 | planes/LL -> bands | SubgroupID index, packHalf2x16 | low (already uniform) | no (FS-D) | T-DWT-1/2 |
| 2 quant | `stages/02_quant.md` | 128 | bands -> meta, stats, payload | ClusteredAdd/Max(8), ShuffleUp/Xor/Shuffle, atomicAdd, u8 stores, f16/u16 stats | **high**: early returns and per-cluster loops | integers yes, given FS-Q1/Q2 equal | T-Q-0..5 |
| 3 analyze | `stages/03_analyze_rate_control.md` | 64 + 512 | stats -> buckets | subgroupAdd/ShuffleXor(16), ShuffleUp max-scan, atomicAdd | medium: emit runs on one subgroup only | integers yes; FS-A1/A2 | T-A-1..4 |
| 4 resolve | `stages/04_resolve_rate_control.md` | 64 | buckets -> quant | InclusiveAdd, Shuffle, atomicMax | medium: early returns, data-dependent loop | yes | T-R-1..3 |
| 5 packing | `stages/05_block_packing.md` | 64 | meta, payload, stats, quant -> packets | Ballot, ClusteredAdd(16), ShuffleUp, Shuffle, atomicAdd, shared atomicAnd/Or, u8/u16 stores | medium: per-thread variable loops (no barrier inside) | yes, modulo don't-care bits | T-P-1..4 |
| 6 host | `stages/06_frame_and_host.md` | n/a | packets -> frame | fill/copy buffer, fences | n/a | yes | T-H-1..3 |

## 9. The hardest cs_5_0 gaps (ranked)

1. **Byte-granular scattered writes** (quant payload, all of block packing). No sub-word UAV store exists, and Vulkan's
   per-cluster/per-packet atomic allocations put bytes from different groups into the same word. Solution: groupshared
   staging plus word-aligned allocation per group, so each device word has exactly one writer (B6). This needs
   the max-size bounds of stage 2 §4 and stage 5 §6 and a debug overflow counter.
2. **Barriers inside divergent subgroup code** (quant). Every reduction and shuffle sits behind per-cluster early returns
   and a per-cluster `q` loop. Solution: predicate flags plus sharing the texels once, so the `q` loop needs no barrier
   (stage 2 §5.3).
3. **Float reduction order and transcendental precision** (FS-Q3, FS-A1, FS-A2). These are not cs_5_0 gaps but
   cross-vendor facts: Vulkan itself gives different rate-control decisions on the RTX and on lavapipe. Solution:
   fix the butterfly order (matches the RTX), embed the CPU-side tables, and measure the residual flip rate. RQ3 covers
   what remains.
4. **Bug-compatible behaviour the port must copy on purpose**: the windowed finalize scan (stage 3 §5, OQ-5),
   `errors[msb+1]` without `rdo_distortion_scale` (OQ-6), unmasked `quality_planes << 16` (stage 2 §4), the 16-bit
   `block_offset`/`saving` packing (OQ-8), sequence taken from `meta[0]`'s packet (stage 6 §3).
5. **Dispatch overhead**: 141 dispatches (13 + 42 + 43 + 1 + 42) + 3 clears + 2 copies per frame, with D3D11 serializing UAV dispatches and
   many tiny level 3-4 bands. Expect GPU bubbles on TeraScale. Merging per-band dispatches is allowed (output identical).
