# Validation plan

This plan describes how to show that the D3D11 encoder meets SPEC.md RQ1-RQ3, and how to time it on AN515. Nothing in it has been
run yet. The tools named here (`tools/vkref/`, `tools/pwcompare.py`, `tools/golden/`) do not exist; tasks T6a-T6d in
`AGENTS.md` create them.

## 1. Machines and roles

| Machine | Role | Notes |
|---|---|---|
| HYTE (Windows, dev) | builds everything (MSVC, fxc); runs the D3D11 encoder on its own GPU for functional tests; runs the comparison tool | local repo `C:\Build\codecs\pyrowave-dx11` |
| 192.168.0.24 (Linux) | Vulkan reference: builds `reference/pyrowave` plus the `tools/vkref` patches; produces golden dumps; decodes for PSNR | holds `~/scratch/pyrowave` and `~/Build/etherquic`. Assumed to be the RTX 2060 SUPER machine that `prototype/README.md:37` measured; confirm (OQ-19) |
| AN515 (192.168.0.58, via .24) | target: HD 6670 functional run and timing; lavapipe Vulkan baseline | **only touch it once a task explicitly says so** (T7). Copy binaries over the .24 hop, as `PLAN.md` describes |

Golden data is produced once per test vector on the RTX, checked into `testdata/golden/` only if small (< 5 MB per
vector; otherwise kept on .24 and referenced by sha256), and consumed on HYTE and AN515.

## 2. Reference device choice

The comparison target is the **RTX 2060 SUPER (subgroup size 32)**, not lavapipe, because:
- On lavapipe the encoder runs with 16-wide subgroups (`LP_NATIVE_VECTOR_WIDTH=512`, `eq:media/src/pyrowave.rs:200-229`).
  `subgroupAdd` over a 16-lane cluster is then a whole-subgroup add whose reduction order is LLVM's, not the ShuffleXor
  butterfly the RTX path uses (`R/shaders/analyze_rate_control.comp`, FS-A2). The port fixes the butterfly order.
- lavapipe's `log2`, division and `exp2` come from LLVM IR lowering and may differ from NVIDIA's by an ULP (FS-A1, FS-Q1).

lavapipe is still a **second reference**. Any FS flip that differs between the RTX and lavapipe shows how sensitive that
point is, and that sets the tolerance in §6. Decision tracked as OQ-2.

## 3. Dumping Vulkan intermediates (`tools/vkref`)

`reference/` stays read-only. `tools/vkref/` holds a patch series (`git format-patch` against a0d2e71) plus a small driver.
On .24, apply the series to a scratch copy (`~/scratch/pyrowave-vkref`), never to `reference/`.

### 3.1 Level 0: public API only (no patch)

`pyrowave_encoder_get_mapped_raw_bitstream` (`R/pyrowave.h:587-589`) returns the mapped meta (`BitstreamPacket[block_count_32x32]`)
and the bitstream buffer after `encode_cpu`. Together with `pyrowave_encoder_packetize` this is enough for end-to-end
checks (T-H-*, T-P-4) without any patch.

### 3.2 Level 1: intermediate buffers (patch `0001-dump-hooks`)

Add to `Encoder::Impl` an optional `dump_hook(stage, name, const Buffer&)`. Call it after each of `dwt()`, `quant()`,
`analyze_rdo()`, `resolve_rdo()` and `block_packing()` (`R/pyrowave_encoder.cpp:1160-1179, 1196-1226`). Each call adds a
full barrier and a copy to a `CachedHost` buffer, so the dump build is never used for timing. Buffers dumped:

| Stage | Buffer (name in `R/pyrowave_encoder.cpp:178-209`) | Compare as |
|---|---|---|
| 1 | each band image `component_layer_views[c][l]` layer b (via `get_wavelet_band`, `:1271-1274`), read back as f32 (R16F widened) | float, tolerance §6 |
| 2 | `meta-buffer` (BlockMeta 8 B per 8x8) | code word exact; `offset` semantically (it points into payload) |
| 2 | `block-stat-buffer` (BlockStats 64 B per 8x8) | f16 fields with tolerance; integer fields exact |
| 2 | `payload-data` | per block, through meta offsets (bytes of block k exact) |
| 3 | `bucket-buffer` | consumed/totals exact; RD op list as a multiset per bucket (order comes from atomics) |
| 4 | `quant-buffer` | exact |
| 5 | meta + bitstream (`BitstreamBuffers`) | per packet through meta offsets, don't-care bits masked (SPEC §5) |

### 3.3 Level 2: band injection (patch `0002-inject-bands`)

`Encoder::encode_pre_transformed(cmd, buffers, quant_scale)` (`R/pyrowave_encoder.cpp:1181-1194`) runs quant..packing on
bands already in the band images. The patch adds a C entry point
`pyrowave_encoder_encode_bands(encoder, const float *bands_f32, size_t count, const pyrowave_rate_control*)`. It uploads the
bands into `wavelet_img_high_res`/`_low_res` (rounding to R16F for levels 0-1 exactly as the D3D11 side does; OQ-3) and then
calls `encode_pre_transformed(cmd, buffers, -1.0f)`. `-1` selects `get_quant_resolution` per band, the same as normal
encoding (`:255, :453`).

Caveat: `encode_pre_transformed` does **not** increment `sequence_count` (only `encode` does, `:1198`). The driver must
bump it itself (the patch adds the increment to the new entry point), or the sequence numbers drift between modes.

### 3.4 Dump format

One directory per (vector, mode, device):
```
manifest.json   {"vector":..., "W":..., "H":..., "budget":..., "sequence":..., "device":"RTX 2060 SUPER/555.xx",
                 "ref":"a0d2e71+vkref-<sha>", "buffers":[{"name":"band_c0_l0_b1","stage":1,"dtype":"f32",
                 "shape":[368,640],"file":"band_c0_l0_b1.bin","sha256":...}, ...]}
*.bin           raw little-endian, no header
```
The D3D11 encoder's debug build (`PWD3D11_DEBUG_DUMP`, T6c) writes the same format with the same names, so one comparison
tool serves both.

## 4. Comparison tool (`tools/pwcompare.py`)

Python 3.13 (HYTE: `C:\Users\Lyam\AppData\Local\Programs\Python\Python313\python.exe`), numpy only.
`pwcompare.py <dirA> <dirB> [--stage N] [--tol-file tol.json]` prints, per buffer:
- bands: max |Δ|, max ULP (f16 for L0-1, f32 for L2-4), count over tolerance;
- meta: number of 8x8 blocks whose code word differs, listed with (level, comp, band, bx, by) and the decoded
  `quant_code`/`msb`, so FS-Q1 flips can be told apart from logic bugs;
- payload: per block, first differing byte index;
- stats: per field max |Δ| and how many differ;
- bucket: per bucket, differences in consumed/total and in the RD op multiset;
- quant buffer: differing 32x32 blocks;
- packets: first differing packet and byte, with the don't-care mask applied, and the parsed header of both packets;
- verdict: PASS / FS-ONLY (every difference traces to a listed FS point) / FAIL.

To trace a difference to an FS point, the tool recomputes that point on the CPU from the *dumped inputs*
(`tools/golden`) and checks that the two sides are on opposite sides of a rounding boundary (for example
`m_wave/target` within 2 ULP of an integer).

## 5. Test modes

| Mode | What runs on each side | Checks |
|---|---|---|
| M1 end to end | both encoders from the same I420 | RQ1 (decoder accepts), RQ3 (PSNR) |
| M2 same bands | the Vulkan bands are dumped and injected into D3D11 (and back into Vulkan via §3.3) | RQ2: packet bytes equal |
| M3 per stage | the D3D11 stage N is fed with Vulkan's stage N-1 outputs (D3D11 debug entry `pwd3d11_debug_run_stage`, T6c) | isolates a failing stage |
| M4 golden model | `tools/golden` (CPU C++ model of stages 2-6, T6b) against both GPUs | separates "port bug" from "Vulkan FS" |

## 6. Tolerances

| Item | Tolerance |
|---|---|
| bands, all levels | max abs ≤ 4e-3 (prototype measured 2.4e-3 at 720p and 1.7e-3 at 1080p, `prototype/README.md` "Correctness"); T-DWT-2 |
| bands L0-1 (R16F) | additionally ≥ 99 % of texels within 1 f16 ULP (proposal, OQ-11) |
| M2 code words | exact, except blocks where pwcompare proves an FS-Q1 boundary; target 0 on the RTX, report the count |
| M2 quantized coefficients and payload | exact for blocks whose code word matches |
| stats f16 | ≤ 2 f16 ULP (FS-Q3/Q4) |
| quant buffer, packets | exact in M2 when stats are bit-equal; otherwise FS-ONLY with ≤ 0.1 % of 32x32 blocks differing |
| decoded PSNR (M1, M2) | \|ΔPSNR\| ≤ 0.05 dB at equal size; size ≤ budget always |

## 7. Test vectors

Synthetic (generated by `tools/genvectors.py`, deterministic seed, I420 `.yuv` plus a `.json` header):
- `grad`: horizontal/vertical ramps (exercises the low bands and the DC shift);
- `checker1/8/32`: checkerboards with 1-, 8- and 32-pixel cells (high bands, block edges, the mirror border);
- `noise`: uniform noise (largest payload, the worst-case packing paths, many planes);
- `flat0/flat255/flat128`: constant frames (all-zero blocks, empty packets, `firstbithigh(0)`);
- `text`: rendered UI text (realistic desktop content for etherquic);
- `edge`: one-pixel lines at x = W-1, y = H-1 (alignment padding, `AW`/`AH` handling).

Real content: three 1080p desktop captures and one video frame, stored on .24 (not committed; sha256 recorded).

Sizes: 256x144 (the caps probe, `eq:media/src/caps.rs:43`), 16x16 (minimum, `eq:media/src/pyrowave.rs:441`), 128x128,
1280x720, 1366x768 (not a multiple of 32: AW 1376, AH 768), 1920x1080, 2560x1440.

Budgets: `default_frame_bytes(W,H) = max(W*H*3/16, 32 KiB)` (`eq:media/src/pyrowave.rs:586-589`, about 1.5 bpp); tight =
16 KiB (the floor at `eq:media/src/pyrowave.rs:448`); generous = W*H (8 bpp, little rate control); plus a budget
not divisible by 4 (exercises `& ~3`, `R/pyrowave_c.cpp:1058`).

Sequences: 10 consecutive frames per vector, so the sequence number wraps (`SequenceCountMask = 7`,
`R/pyrowave_common.hpp:73`).

## 8. PSNR checks

- Decode both bitstreams with the reference decoder (`pyrowave_decoder_*` C API, `R/pyrowave.h:621-670`) on .24, and also
  with the etherquic decoder path (`eq:media/src/pyrowave.rs:548-568`) to prove RQ1 end to end.
- PSNR per plane on 8-bit I420 against the source: `10*log10(255^2/MSE)`. Report Y, Cb and Cr, plus
  `PSNR_YUV = (6*Y + Cb + Cr)/8` (a project convention, not from the reference). `R/psnr.cpp` is a Vulkan/ffmpeg tool
  for sweeping target sizes (`:634-700`); it can be used for extra sweeps (`--pyrowave-target-size-range`) but is not
  required.
- Pass: every vector, every budget: |ΔPSNR_Y| ≤ 0.05 dB and |ΔPSNR_YUV| ≤ 0.05 dB, and D3D11 size ≤ budget.

## 9. Timing methodology on AN515 (run in T7 only)

- Build: Release, shaders precompiled with `fxc /O3`, no dump hooks, no debug layer.
- Per stage: `D3D11_QUERY_TIMESTAMP_DISJOINT` around the frame, `D3D11_QUERY_TIMESTAMP` at each stage boundary
  (upload, DWT, quant, analyze, resolve, packing, readback copy), as `prototype/dx11dwt.cpp` already does for the DWT.
  Read the queries one frame late so they do not stall.
- Wall time: `QueryPerformanceCounter` from `encode_cpu` entry to the end of `packetize`, which includes Map/Unmap of the
  staging buffers and the CPU packetize.
- 20 warm-up frames, then 200 measured frames of the `text` and `noise` vectors at 720p and 1080p with the default budget.
  Report median and p90 per stage, and the sum.
- Baseline, same machine, same vectors: lavapipe via `prototype/pwbench.cpp` (`pwbench.exe libpyrowave-shared-1.dll
  vulkan-1.dll W H 200 <budget>`), whose numbers are in `prototype/README.md:35-40`. The CPU load of both runs is recorded
  too (lavapipe saturates the 5950X).
- Expected: about 5-7 ms at 720p and 9.5-14 ms at 1080p by the prototype extrapolation (`prototype/README.md:44-47`), the same as the `PLAN.md` budget.
