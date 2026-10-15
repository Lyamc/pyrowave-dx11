# Stage 3: rate-control analysis (`analyze_rate_control.comp` + `analyze_rate_control_finalize.comp`)

Status: spec. Source of truth: `R/shaders/analyze_rate_control.comp` (`ar:N`), `R/shaders/analyze_rate_control_finalize.comp`
(`arf:N`), `R/pyrowave_encoder.cpp:17-26, 53-71, 199-208, 344-417`.

## 1. What the stage does

For every 32x32 block (one workgroup), it sums the per-8x8 rate (words) and distortion for each candidate plane drop
`i = 0..15` (`ar:149-217`). Then it turns each step `i-1 -> i` into an "RD operation": a distortion-per-saved-word
bucket index (0..127) and the words saved. It accumulates the savings per (bucket, block subdivision) and records the
operation in a slot that is unique per (bucket, block) (`ar:101-147`). The finalize pass turns the per-bucket totals into a
(windowed, see §5) prefix sum (`arf:14-39`).

## 2. Bucket buffer layout (byte offsets)

Size `64 + 128*16*4 + 128 * per_sub * 16 * 8` (`R/pyrowave_encoder.cpp:203-206`), with
`per_sub = next_pow2(align(block_count_32x32, 16) / 16)` (`R/pyrowave_encoder.cpp:21-26`). Cleared to 0 every frame
(`R/pyrowave_encoder.cpp:1212`).

| Offset | Field | Writer |
|---|---|---|
| 0 | `count` (unused by any shader) | none |
| 4 | `consumed_payload` (uint, words) | `InterlockedAdd` by lane 0 of each analyze group (`ar:127-131`) |
| 64 + 4*k, k < 2048 | `total_savings_per_bucket[k]`, `k = bucket*16 + subdivision` | `InterlockedAdd` (`ar:142`); finalize rewrites it as the scan |
| 8256 + 8*j | `rdo_operations[j]` = `{ int quant; uint block_offset_saving; }`, `j = block_index + bucket*num_blocks_aligned` | plain store (`ar:143-144`); unique slot per (block, bucket) |

`block_offset_saving = block_index | (saving << 16)` (`ar:144`), matching the host struct `RDOperation {int32 quant; uint16 block_offset; uint16 block_saving;}`
(`R/pyrowave_encoder.cpp:66-71`). Requires `block_count_32x32 < 65536` and `saving < 65536`. Neither is checked
upstream (OQ-8). The port asserts the first on the CPU at init.

## 3. Dispatches and constants

Per band, in the quant loop order: `ceil(w/32) x ceil(h/32)` groups of 64 (`R/pyrowave_encoder.cpp:361-398`), then
1 group of 512 for finalize (`R/pyrowave_encoder.cpp:404-411`). cbuffer = `AnalyzeRateControlPushData`
(`R/pyrowave_encoder.cpp:53-64`; `ar:55-66`): `int2 resolution @0; int2 resolution_8x8_blocks @8; int block_offset_8x8 @16;
int block_stride_8x8 @20; int block_offset_32x32 @24; int block_stride_32x32 @28; uint total_wg_count @32;
uint num_blocks_aligned @36; uint block_index_shamt @40`, padded to 48 bytes.
`num_blocks_aligned = per_sub * 16`, `block_index_shamt = log2(per_sub)` (`R/pyrowave_encoder.cpp:388-390`).

Resources: `u0` RWByteAddressBuffer = bucket buffer. `t0` ByteAddressBuffer = block stats (stage 2 layout; a read-only
SRV is enough, `ar:50-53`). Finalize uses `u0` only.

## 4. Exact computation (normative)

Analyze group (`ar:149-221`), `index = SV_GroupIndex`, `lb = index & 15`, `(lx, ly) = (lb & 3, lb >> 2)`,
`g = index >> 4`. Block 8x8 = `4*group + (lx, ly)`; `in8 = all(block8 < resolution_8x8_blocks)`;
`b8 = block_offset_8x8 + block_stride_8x8*y + x`.

For `i ∈ {g, g+4, g+8, g+12}`:
`(dist, cost) = in8 ? (f16tof32(e & 0xffff), e >> 16) : (0, 0)` with `e = stats[b8].errors[min(i, num_planes)]`;
`if (cost != 0) cost += 24`.
Reduce over the 16 lanes with the same `g`: `cost` as an integer sum; `dist` as a float sum in the xor-butterfly tree order
`((((d0+d1)+(d2+d3))+((d4+d5)+(d6+d7))) + (((d8+d9)+(d10+d11))+((d12+d13)+(d14+d15))))` (index = lane `lb`).
Then `if (cost != 0) cost += 64`; `rate[i] = (cost + 31) >> 5`; `dist[i] = sum` (`ar:204-214`).

The reference picks `subgroupAdd` when the subgroup is 16 wide and the butterfly otherwise (`ar:186-202`). The RTX
(subgroup 32) runs the butterfly; lavapipe with `LP_NATIVE_VECTOR_WIDTH=512` (16 lanes, `eq:media/src/pyrowave.rs:217-222`)
runs `subgroupAdd`, whose float order belongs to LLVM. The port implements the butterfly, so it should be bit-exact
with the RTX and only statistically equal to lavapipe (FS-A2, OQ-2).

Emit (threads 0..15 meaningful, `ar:101-147`), with `t` = thread index:

```
cost_t = float(rate[t]), d_t = dist[t]         (t < 16)
bucket_t = (cost_t == rate0) ? 0 : uint(max(60 + 2*log2(max(d_t - dist[0], 0) / (rate0 - cost_t)) + 0.5, 0))
bucket_0 = 0
v = min(bucket_t, 112 + t)                       // 128 - 16 + lane
for s in 1, 2, 4, 8: v = max(v, t >= s ? v[t-s] + s : 0)   // Hillis-Steele, reads the previous step's values
t == 0:  consumed_payload += rate[0]
1 <= t < 16: saving = rate[t-1] - rate[t]; if saving != 0:
        blk = block_offset_32x32 + gy*block_stride_32x32 + gx; sub = blk >> block_index_shamt
        total_savings[v*16 + sub] += saving
        rdo_operations[blk + v*num_blocks_aligned] = { t, blk | (saving << 16) }
```

`saving` is unsigned. If `rate[t] > rate[t-1]` it wraps, exactly as in the GLSL. Reproduce, don't fix (OQ-8).

Finalize (`arf:14-39`), 512 threads, `t = SV_GroupIndex`: `v = uint4 at 64 + 16*t`; `v.y += v.x; v.z += v.y; v.w += v.z`;
`gs[t] = v.w`; barrier; `for (step = 1; step < 256; step *= 2) { barrier; up = t >= step ? gs[t-step] : 0; barrier; v += up; gs[t] = v.w; }`;
store `v`. Port it literally, including the loop bound `gl_WorkGroupSize.x / 2` = 256 (§5).

## 5. Finding: the finalize scan is windowed

With steps 1..128, Hillis-Steele gives each thread the sum of the **256** thread totals ending at itself, not of all
512. Entries for buckets ≥ 64 (thread ≥ 256) therefore miss the totals of threads `0 .. t-256`. The resolve stage's
"empty bucket" test (`total[x] == total[x-1]`) and its `prev_bucket_total` are affected for those buckets. The
upstream intent is probably `step < gl_WorkGroupSize.x`, but bit-exactness requires reproducing the reference.
Recorded as OQ-5. The CPU golden model implements both variants, so the effect can be measured.

## 6. GLSL features and cs_5_0 replacements

| GLSL (line) | cs_5_0 |
|---|---|
| `gl_SubgroupInvocationID + gl_SubgroupSize*gl_SubgroupID` (152) | `SV_GroupIndex` |
| `subgroupAdd` / `subgroupShuffleXor` reductions over 16 (186-202) | groupshared `gs_cost[16][16]`, `gs_dist[16][16]`; barrier; threads 0..15 reduce row `i = t` in the tree order above |
| `subgroupShuffleUp` max-scan (85-99) | groupshared Hillis-Steele, 4 steps, each `read; barrier; write; barrier`, executed by all 64 threads; only `t < 16` compute |
| `if (gl_SubgroupID == 0) emit_rdo_operations()` (219-220) | `if (t < 16)` around the *non-barrier* parts only; the barriers of the scan sit outside |
| `atomicAdd` on SSBO uint (130, 142) | `RWByteAddressBuffer.InterlockedAdd` |
| `float16_t` / `uint16_t` members (38-48) | 32-bit `Load` + `f16tof32(e & 0xffff)` / `e >> 16` |
| `shared` arrays + `barrier()` (finalize) | groupshared + `GroupMemoryBarrierWithGroupSync`, same placement |
| `uvec4` SSBO access (arf:9, 16, 38) | `Load4` / `Store4` at `64 + 16*t` |

## 7. Barrier rules

- Analyze: barrier after the 16x16 groupshared writes; barrier after the 16 row reductions; scan steps as above. The
  GLSL has a single `barrier()` (`ar:217`) because the rest runs on one subgroup. In HLSL every cross-thread exchange
  needs its own group barrier.
- No `return`. Out-of-range blocks are predicated (`in8`) exactly as in the GLSL.
- Finalize: two barriers per iteration, literal port. The trip count is a literal.

## 8. Rounding semantics

- `cost`, `rate`, savings and buckets after the index is computed: exact integers.
- FS-A1: `log2`, the division `(d - d0)/(c0 - c)` and `+ 0.5` then truncation by `uint()`. GLSL and D3D11 `log2` are both
  approximations (vendor ULP). A bucket flips only when the value is near `k + 0.5`. Measure the flip rate (T-A-3).
- `log2(0) = -inf` gives `index = -inf` and bucket 0 in both APIs. `log2` of a negative value gives NaN: the GLSL `max(NaN, 0)` is
  undefined in SPIR-V, while D3D11 `max` returns the non-NaN operand (0). It is reachable only if the cost were non-monotonic (OQ-8).
- FS-A2: the 16-lane float distortion sum (above).

## 9. Test oracle

With **injected Vulkan block stats** (the analyze stage reads only `block_stats`, so this isolates it completely):

- T-A-1: `consumed_payload` exact.
- T-A-2: `rdo_operations` exact (deterministic slots); `total_savings_per_bucket` exact before finalize (dump between
  passes in debug builds) and after.
- T-A-3: if T-A-2 fails, the golden model reports which `(block, i)` had a different bucket and how far
  `60 + 2*log2(...) + 0.5` was from an integer. Mismatches with distance < 1e-5 count as FS-A1, anything else is a bug.
- T-A-4: finalize alone: random `total_savings` input, compare with the CPU model of the windowed scan. Exact.
