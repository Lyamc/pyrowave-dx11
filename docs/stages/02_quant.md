# Stage 2: quantization (`wavelet_quant.comp`)

Status: spec. This file replaces the unreviewed draft `docs/quant_stage_spec.md`. The corrections are listed in §9.
Source of truth: `R/shaders/wavelet_quant.comp` (cited as `wq:N`), `R/shaders/dwt_quant_scale.h`, `R/shaders/constants.h`,
`R/shaders/dwt_swizzle.h`, `R/pyrowave_encoder.cpp:28-38, 111-176, 419-492`, `R/pyrowave_common.hpp:93-110, 124-145`.

## 1. What the stage does

For every 8x8 block of every transmitted band, the stage:

1. finds the block's maximum magnitude;
2. picks a per-block quant scale (4-bit code);
3. quantizes to integer magnitudes with truncation;
4. writes a 32-bit code word (2 bits of plane count per 4x2 subblock, 4 bits of "quality planes", 4 bits of quant scale);
5. writes the full-precision bit-plane payload (a sign byte, then the magnitude planes MSB first, per 4x2 subblock) into a scratch buffer;
6. writes rate/distortion statistics for every possible plane drop `q` (consumed by stages 3 and 5).

The payload buffer is an encoder-internal scratch buffer. Its layout is not part of the bitstream, which gives the port
freedom (§5).

## 2. Dispatches

From `R/pyrowave_encoder.cpp:436-483`: for level 0..4, component 0..2 (skip level 0 chroma for 4:2:0), band
`(level == 4 ? 0 : 1)..3`, one dispatch of `ceil(w/32) x ceil(h/32)` groups of 128 threads. `w, h` is the band size
`AW >> (level+1)`, `AH >> (level+1)`. That is 42 dispatches for 4:2:0.

cbuffer, identical member order to `QuantizerPushData` (`R/pyrowave_encoder.cpp:28-38`) and `wq:60-70`. HLSL packing
gives the same offsets as the C struct: `int2 resolution @0; int2 resolution_8x8_blocks @8; float2 inv_resolution @16;
float input_layer @24; float quant_resolution @28; int block_offset @32; int block_stride @36; float rdo_distortion_scale @40`.
Pad the C struct to 48 bytes.

| Field | Value (`R/pyrowave_encoder.cpp:453-469`) |
|---|---|
| `resolution` | band `w, h` |
| `resolution_8x8_blocks` | `(w+7)/8, (h+7)/8` |
| `inv_resolution` | `1.0f/w, 1.0f/h` (only needed for the Gather variant) |
| `input_layer` | `float(band)` |
| `quant_resolution` | `1.0f / decode_quant(encode_quant(1.0f / get_quant_resolution(level, component, band)))`. `get_quant_resolution` returns a power of two ≤ 4096 (`R/pyrowave_encoder.cpp:148-176`, precision ≥ 1), so this value is that same power of two and `texel * quant_resolution` is exact |
| `block_offset`, `block_stride` | `block_meta[component][level][band].block_offset_8x8 / block_stride_8x8` (`R/pyrowave_common.cpp:213-243`) |
| `rdo_distortion_scale` | `get_quant_rdo_distortion_scale(level, component, band) / 256` (`R/pyrowave_encoder.cpp:111-146, 463`). Uses `std::sqrt/exp/pow`, whose last-ULP results differ between glibc and the MSVC CRT. **Rule:** do not recompute it at run time. Embed the 60-entry table (per chroma mode) dumped from the Linux reference build as hex floats (task T2a) |

The specialization constant `SkipQuantScale` (`wq:19`) is always false: the encoder sets mask 0 (`R/pyrowave_encoder.cpp:425`).
Compile it out.

## 3. Resources

| Vulkan | HLSL (cs_5_0) | Layout |
|---|---|---|
| `binding 0 sampler2DArray uTexture` + `border_sampler` (CLAMP_TO_BORDER, transparent black, `R/pyrowave_common.cpp:52-56`) | `Texture2DArray<float> t0` (band SRV, 4 layers) | read via `Load(int4(x, y, band, 0))`. Out-of-bounds `Load` returns 0 in D3D11, the same as the border colour. A `GatherRed` with `D3D11_TEXTURE_ADDRESS_BORDER` (border 0) and offsets `(1,1)`, `(3,1)` is the literal alternative |
| `binding 1 SSBOMeta { BlockMeta meta[]; }` | `RWByteAddressBuffer u0` | 8 bytes per 8x8 block at `8*block_index`: `+0 code_word`, `+4 offset` |
| `binding 2 SSBOBlockStats { BlockStats stats[]; }` | `RWByteAddressBuffer u1` | 64 bytes per block at `64*block_index`: `+0 num_planes`, `+4+4q errors[q]` for q = 0..14. `errors[q]` = `f16(square_error) | (payload_cost << 16)`, because `QuantStats { float16_t square_error; uint16_t payload_cost; }` puts the f16 in the low half (`wq:23-27`; host view `R/pyrowave_common.hpp:93-104`) |
| `binding 3 Payloads { uint counter @0; uint8_t data[] @8; }` | `RWByteAddressBuffer u2` | `+0` payload byte counter (this stage), `+4` bitstream word counter (stage 5, `R/shaders/block_packing.comp:56-60`), `+8...` payload bytes. Buffer size `AW*AH*2` (`R/pyrowave_encoder.cpp:194-197`) plus the padding from §5 |

`block_index = block_offset + by*block_stride + bx` (`wq:191`).

## 4. Exact per-thread computation (normative)

Thread mapping (`wq:294-306`): `local_index` = `SV_GroupIndex` (0..127). `cluster = local_index >> 3` (0..15), one
8x8 block per cluster: `block_x = cluster & 3`, `block_y = cluster >> 2`. `lane = local_index & 7` is the 4x2 subblock
index within the 8x8 block. `local_coord = unswizzle8x8(lane << 3)` = `(4*(lane >> 2), 2*(lane & 3))`, which gives subblock
order 0..3 down the left column and 4..7 down the right (`R/bitstream/bitstream.md:367-377`).
`coord = 32*group + 8*(block_x, block_y) + local_coord`.

The **cluster** is the 8 consecutive threads of one 8x8 block. Every subgroup operation in this shader is clustered
over those 8 lanes (`ClusteredAdd/Max(…, 8)`, ShuffleXor 1/2/4, ShuffleUp masked by `& 7`, Shuffle to `| 7`). None of
them crosses a cluster, so the result does not depend on the subgroup size, which the reference lets float from 8 to
128 (`R/pyrowave_encoder.cpp:426-428`).

Texels (`wq:308-312`): `t0 = (T(x,y), T(x,y+1), T(x+1,y), T(x+1,y+1))`, `t1` the same at `x+2`. That is Gather offset `(1,1)` / `(3,1)`
with `.wxzy`, giving pixel order `0 2 4 6 / 1 3 5 7` (`R/bitstream/bitstream.md:381-387`). `precise s = t * quant_resolution`.

`in_bounds = all(block (bx,by) < resolution_8x8_blocks)` (`wq:313`). It is uniform across the cluster. Out-of-bounds clusters
write **nothing** (their `block_index` would alias another block).

Per cluster (`wq:172-203`):

```
precise m_sub  = max(max4(|s0|), max4(|s1|))                    // per thread
precise m_wave = max over the 8 threads of m_sub                   // exact; order-free
if (m_wave < 1.0) { code = 6; qs = 1.0 }                           // ENCODE_QUANT_IDENTITY (dwt_quant_scale.h:13)
else {
  e      = exponent such that (m_wave - 0.25) = f * 2^e, f in [0.5, 1)   // frexp, wq:104-105
  target = float(1 << e) - 0.25
  inv    = m_wave / target                                         // DIVISION: float-sensitive point FS-Q1
  code   = uint(ceil((inv - 0.25) * 8.0))                          // dwt_quant_scale.h:15-19
  qs     = 1.0 / (float(code) / 8.0 + 0.25)                        // DIVISION: FS-Q2
}
s *= qs; m_wave *= qs; m_sub *= qs
inv_quant_sq = (1.0 / (quant_resolution * qs))^2                   // stats only: FS-Q4
a = abs(int4(s))            // float->int truncates toward zero
max_abs = int(m_wave); b4x2 = int(m_sub)
if (max_abs == 0): lane 0 writes meta = {0,0}, num_planes = 0, errors[0] = 0; the cluster is done   (wq:194-203)
msb = firstbithigh(max_abs)
```

`compute_quant_stats(q)` per thread (`wq:139-165`):

```
sh   = b4x2 >> q
early = (sh > 0) ? 1 : 0
m    = msb - q; qp = 0
if (m >= 3) { qp = m - 2; early = qp + 1; sh >>= qp }
early += firstbithigh(sh) + 1          // relies on findMSB(0) == -1 (sh == 0 gives +0)
late_bits = 8 * ClusterSum(max(early - 1, 0)) + ClusterSum(num_sig(q))
```

`num_sig(q)` and the per-thread error (`wq:113-128`): `v = |s|`, `iv = floor(v * 2^-q)` for t0 and `trunc(...)` for t1
(identical here because `v ≥ 0`), `num_sig` = count of `iv != 0` over 8 values, `iv += (iv != 0) ? 0.5 : 0`,
`iv = trunc(iv * 2^q)`, `err = v - iv`, `e_t = (dot(err0,err0) + dot(err1,err1)) * rdo_distortion_scale * inv_quant_sq`.
The order of the two multiplies is `wq:127` then `wq:146`. The cluster error is `ClusterSum(e_t)` (`wq:243`). Compute
`2^±q` by building the float from bits (`asfloat((127 ± q) << 23)`), not with `exp2`, so that it is exact.

Outputs, written by lane 0 of an active cluster (`wq:205-256`):

- `scan` = inclusive cluster prefix of `early(0)`; cluster bytes = `scan` at lane 7; thread byte offset =
  `cluster_base + scan - early(0)` (`wq:208-217`).
- `code_word = (qp(0) << 16)`, then `bitfieldInsert(code_word, code, 20, 4)`, OR `(plane_code_lane << 2*lane)` for the 8
  lanes, where `plane_code = firstbithigh(sh(0)) + 1` (`wq:219-229`). `qp` is **not** masked to 4 bits before the shift.
  Reproduce that literally.
- `meta[block] = {code_word, cluster_base}`; `num_planes = msb + 1`; `errors[0] = f16(0) | (late_bits(0) << 16)` (`wq:231-238`).
- for `q = 1..msb`: `errors[q] = f16(min(ClusterSum(e_t(q)), 60000)) | (late_bits(q) << 16)` (`wq:240-251`).
- `errors[msb+1] = f16(min(60000, ClusterSum((dot(s0,s0)+dot(s1,s1)) * inv_quant_sq))) | 0` (`wq:253-256`). This term
  has **no** `rdo_distortion_scale`, unlike `errors[1..msb]`. Bug-compatible; see OQ-6.
- Payload, per thread, if `need_sign = sh(0) != 0 || qp(0) != 0` (`wq:258-289`): one sign byte (bit k = `s[k] < 0.0`,
  k = 0..3 from t0, 4..7 from t1), then for `q = qp(0) + plane_code - 1` down to 0 one byte with bit k = bit q of `a[k]`.
  The thread's byte count equals `early(0)`.

`uint16_t(late_bits)` keeps the low 16 bits. `errors` has 15 entries, so `msb + 1 ≤ 14` is required. The reference does not
check it (OQ-7). The port must count violations into a debug counter and must not write past entry 14.

## 5. cs_5_0 design (normative structure, informative details)

### 5.1 Feature replacement table

| GLSL (line) | cs_5_0 |
|---|---|
| `subgroupClusteredMax(float, 8)` (175) | groupshared `float gs_max[128]`, barrier, max of the 8 cluster entries |
| `subgroupClusteredAdd(uint/int, 8)` (126, 163) | integer sum of 8 groupshared entries (exact in any order) |
| `subgroupClusteredAdd(float, 8)` (243, 254) | sum of 8 values in the fixed tree order `((e0+e1)+(e2+e3))+((e4+e5)+(e6+e7))`: the result of a ShuffleXor 1, 2, 4 butterfly. The Vulkan driver's own order is unknown (FS-Q3) |
| `subgroupShuffleUp` scan (84-93) | exclusive prefix of the cluster's `early(0)` from groupshared |
| `atomicAdd(counter, scan)` by lane 7 + `subgroupShuffle(…, | 7)` (213-215) | one `InterlockedAdd(u2, 0, group_bytes_aligned, base)` per **group**, broadcast through groupshared (§5.3) |
| `subgroupShuffleXor` OR-merge (225-228) | OR of the 8 lanes' `plane_code << 2*lane` read from groupshared |
| `uint8_t data[]` stores (267, 286) | bytes assembled in a groupshared staging array with `InterlockedOr`, then whole-word `Store` (§5.3). cs_5_0 has no `Store8`/`Store2`; a 32-bit RMW on device memory shared with another group would race |
| `float16_t(x)` | `f32tof16(x)` (low 16 bits) |
| `uint16_t(x)` | `x & 0xffff` |
| `findMSB(int)` (162, 205, 223) | `firstbithigh(uint(x))`. The argument is never negative here; use the `uint` overload, because `firstbithigh(int)` on a negative value searches for the first 0 bit. `firstbithigh(0u)` returns `0xFFFFFFFF` (-1 as int), which `wq:162` relies on (test T-Q-0) |
| `bitfieldExtract/Insert` | shifts and masks |
| `frexp(x, e)` (105) | integer exponent from bits: `e = int((asuint(y) >> 23) & 0xff) - 126` for normal `y = m_wave - 0.25 ≥ 0.75`. Do not use HLSL `frexp`, whose exponent is a float |
| `ldexp(v, ±q)` (116, 124) | multiply by `asfloat(uint(127 ± q) << 23)` (exact) |
| `precise` (174-175, 311-312) | HLSL `precise` on the same variables, and also on `s *= qs`, so that fxc cannot fuse or reassociate `t * quant_resolution * qs` |
| `textureGatherOffset` (309-310) | `Load` (recommended) or `GatherRed` with immediate offsets |

### 5.2 Barrier-correctness rules

The GLSL returns early (`wq:194-203` zero block, `wq:314` out of bounds) and loops `q = 1..msb` with a
per-cluster `msb`. Subgroup operations are legal there; `GroupMemoryBarrierWithGroupSync` is not. Rules:

1. No barrier after a data-dependent `return`, inside an `if` on per-thread or per-cluster data, or inside a loop whose trip
   count is not a literal or cbuffer value. Express "this cluster is done" as a predicate flag (`active`), never as a `return`.
2. Every groupshared array written by one thread and read by another needs a barrier between the write and the read; a
   reused array needs a barrier before it is overwritten.
3. The 64-wide TeraScale wavefront must not be used as an implicit barrier.
4. Compile with `/WX` so that fxc's X3663 ("thread sync operation found in varying flow control") fails the build.

### 5.3 Recommended structure: five barriers, no barrier inside the q loop

The per-q statistics need cluster sums, so a literal port puts 2 barriers into each of up to 14 iterations. Instead,
share the 8 texels once and let each lane compute *its* q values for the whole cluster from groupshared:

- P0: zero the staging array; load texels; `s = t * quant_resolution`; `m_sub` -> `gs_max`. **Barrier 1.**
- P1: `m_wave`, `code`, `qs`, scaled `s`, ints, `msb`, `active = in_bounds && max_abs != 0`, `early(0)`, `qp(0)`, `sh(0)`.
  Store `s` (8 floats) -> `gs_tex[128][8]`, `b4x2` -> `gs_b4x2`, `active ? early(0) : 0` -> `gs_early`. **Barrier 2.**
- P2: cluster exclusive scan and total of `gs_early`; lane 0 stores the cluster total in `gs_ctotal[16]`. **Barrier 3.**
- P3: thread 0 sums the 16 totals in cluster order, rounds up to a multiple of 4 and, if non-zero, does one
  `InterlockedAdd(u2, 0, …)` to get `group_base`, which goes to groupshared. **Barrier 4.**
- P4: `cluster_base = group_base + Σ gs_ctotal[c' < cluster]`. Each thread ORs its `early(0)` bytes into the staging array at
  `cluster_base - group_base + scan_excl`. Lane 0 writes meta, `num_planes`, `errors[0]` and `errors[msb+1]`. For the per-q
  stats, lane j computes `q ∈ {1+j, 9+j}` with `q ≤ msb`. For each of those it evaluates `e_t(q)`, `early_t(q)` and
  `num_sig_t(q)` for all 8 threads of the cluster from `gs_tex`/`gs_b4x2`, sums them in the fixed tree order and writes
  `errors[q]`. Use the same function for every per-thread term, so fxc emits one instruction sequence. **Barrier 5.**
- P5: all 128 threads copy staging words `[0, group_bytes_aligned/4)` to `u2` at `8 + group_base + 4*i` with `Store`. These
  words belong to this group alone, because `group_base` and the size are multiples of 4.

Groupshared: `gs_tex` 4 KB, `gs_max` 512 B, `gs_b4x2`/`gs_early` 1 KB, staging 640 words = 2.5 KB (128 threads x at most 19
bytes when `msb ≤ 16`; with the `msb ≤ 13` bound from §4 it is 15 bytes), plus a few scalars. Under 9 KB, well inside 32 KB.

Allowed deviations from the reference, because they never reach the bitstream:

- payload allocation is per group and padded to 4 bytes, instead of per cluster and unpadded. The counter value, the
  offsets and the order of blocks in the payload buffer differ from Vulkan (Vulkan's order already varies run to run,
  since it comes from atomics). Size the buffer `AW*AH*2 + 4*total_quant_groups + 8`;
- the bytes between blocks (padding) are 0.

Not allowed: anything that changes `code_word`, the per-block payload bytes, `num_planes` or any `errors[q]` value.

## 6. Rounding semantics (summary)

| Point | Semantics | Exact across GPUs? |
|---|---|---|
| `t * quant_resolution` | power-of-two multiply | yes |
| `m_wave - 0.25`, exponent, `float(1<<e) - 0.25` | exact for the value range | yes |
| FS-Q1 `m_wave / target` -> `ceil` | division is not correctly rounded in GLSL (2.5 ULP allowed) nor guaranteed in D3D11; a 1-ULP difference flips `code` only when `(inv-0.25)*8` is within 1 ULP of an integer | no; measure the mismatch rate |
| FS-Q2 `1/(code/8+0.25)` | 16 possible inputs. Recommended: a 16-entry table of correctly rounded `1.0f/x` from the CPU, **after** test T-Q-5 shows which values the RTX and lavapipe produce. If the Vulkan GPUs do not round correctly, copy their values | per code; OQ-1 |
| `s * qs`, `int()` truncation | IEEE multiply, truncation toward zero in both APIs | yes, given the same `qs` |
| FS-Q3 cluster float sums, `dot` | order/fusion implementation-defined in Vulkan | no; affects stats only |
| FS-Q4 `1/(qr*qs)`, squared | division | stats only |
| f16 of stats | `f32tof16` rounding vs `float16_t()` (OQ-3) | to verify |

Stats differences only move rate-control decisions (stage 3). Integer outputs (code words, payload, costs) must be
bit-exact whenever `qs` and `code` match.

## 7. Test oracle

All with **injected Vulkan bands** (VALIDATION §3.3, mode M2/M3 in §5):

- T-Q-0 (unit): `firstbithigh(0u) == 0xFFFFFFFF`; `f32tof16` of boundary values (halfway cases, 60000, subnormals) vs a CPU RNE reference.
- T-Q-1: `code_word` exact for every in-bounds 8x8 block.
- T-Q-2: payload bytes exact per block. Read `L = Σ_subblocks (n ? n + 1 : 0)` bytes from each implementation's `meta.offset`,
  with `n = q_bits + 2-bit code` from the code word. This is the same walk stage 5 does (`R/shaders/block_packing.comp:333-349`).
  Never compare raw payload buffers or offsets.
- T-Q-3: `num_planes` and every `payload_cost` exact; `square_error` f16: report the exact-match rate and the max ULP
  difference. Pass: costs 100% and errors ≥ 99.9% exact, the rest within 1 f16 ULP (proposed; record measured values).
- T-Q-4: no block has `msb + 1 > 14` (debug counter is 0), and no payload write goes beyond the buffer.
- T-Q-5: quant-scale table probe: the 16 `qs` values computed on the RTX, lavapipe and the HD 6670 (task T2a; OQ-1).
- CPU golden model (`tools/`, task T6b): the same formulas in C++ with the fixed tree order. It classifies each mismatch as
  "D3D11 bug" or "Vulkan float-order effect".

## 8. Performance notes (HD 6670)

The reference runs one dispatch per band (42 per frame, many tiny at levels 3-4: a 40x23 band is 2x1 groups). D3D11
serializes UAV-writing dispatches. Optional task T2c: one dispatch per stage over a band table (structured buffer
with per-band constants, group to band lookup). The output is identical, because only the payload allocation order changes.

## 9. Corrections to the deleted draft (`docs/quant_stage_spec.md`)

| Draft claim | Correct |
|---|---|
| `payloads.Store8(byte_offset, s)` | No `Store8` in cs_5_0 (any SM5 buffer type). Use groupshared byte staging with `InterlockedOr`, then aligned word `Store` (§5.3) |
| groupshared replacements that each "write; GroupBarrier; read", called inside `encode_payload` | Those calls run inside per-cluster divergent code: after the zero-block `return` (`wq:194-203`), inside `if (in_bounds)` (`wq:314`) and in the `q ≤ msb` loop (`wq:240`). Barriers there are invalid. See §5.2 |
| "floor vs trunc must be checked" | `wq:115` takes `abs()` first, so `floor` and `trunc` agree on non-negative inputs. No difference |
| cluster = "the 8 threads sharing `block_local_index`" | the cluster is 8 *consecutive* threads (one 8x8 block); `block_local_index` is the lane *within* the cluster |
| "emulated subgroup size 32 (NVIDIA)" | irrelevant: every operation is clustered by 8 and works for any subgroup size ≥ 8 |
| `packed = (f16(err) << 16) | cost` | reversed. `square_error` is the first member, so `f16` is the **low** half and `cost << 16` the high half |
| `firstbithigh(0)` is undefined, avoid it | D3D11 defines it as `0xFFFFFFFF`, and the shader *needs* `findMSB(0) == -1` at `wq:162` |
| `SkipQuantScale` as a cbuffer bool | always false in the encoder (`R/pyrowave_encoder.cpp:425`); compile it out |
| `precise` only on the `quant_resolution`/`quant_scale` chains, "OPEN" | `precise` exactly where the GLSL has it plus `s *= qs`. The quant path is exact except FS-Q1/FS-Q2 |
| `errors[msb+1]` computed like the other errors | it omits `rdo_distortion_scale` (`wq:254`); reproduce it |
