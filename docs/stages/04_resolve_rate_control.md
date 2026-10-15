# Stage 4: rate-control resolve (`resolve_rate_control.comp`)

Status: spec. Source of truth: `R/shaders/resolve_rate_control.comp` (`rr:N`), `R/pyrowave_encoder.cpp:287-342`.

## 1. What the stage does

One workgroup per (bucket, subdivision) entry, 2048 in all (`R/pyrowave_encoder.cpp:332`). Each workgroup walks its
RD operations in block order and applies them (`atomicMax` of the plane-drop level into the per-32x32 `quant` buffer)
until the cumulative saving, counted across all earlier buckets, covers the overshoot
`consumed_payload - target_payload_size` (`rr:38-80`).

## 2. Inputs, outputs, constants

| Resource | Layout |
|---|---|
| `t0` ByteAddressBuffer, bucket buffer (stage 3 layout) | read `consumed_payload @4` as int, `total_savings[x] @64+4x` as int, `rdo_operations @8256` |
| `u0` RWByteAddressBuffer, quant buffer | `int quant[block_count_32x32]`, cleared to 0 per frame (`R/pyrowave_encoder.cpp:199-200, 1213`) |
| cbuffer | `uint target_payload_size` = `(target - 8) / 4` words, where `target = maximum_bitstream_size & ~3` (`R/pyrowave_c.cpp:1058`), minus the 8-byte sequence header (`R/pyrowave_encoder.cpp:293-294, 327`); `uint num_blocks_per_subdivision = per_sub` (328) |

The reference sets the group size to the subgroup size (64, 16 or 32, `R/pyrowave_encoder.cpp:298-317`; `rr:10`). The port
uses `numthreads(64,1,1)`. The result does not depend on the chunk size. `should_apply` for operation j is
"global exclusive prefix of savings within this entry + total of earlier buckets < required", and the loop exits only
once `total_saved ≥ required`, after which no later operation could pass the test anyway.

## 3. Exact computation (normative)

```
req = int(consumed_payload) - int(target_payload_size)          // rr:40
x = group id
if x != 0: prev = total[x-1]; if total[x] == prev: skip; req -= prev
else:      if total[0] == 0: skip
if req <= 0: skip
total_saved = 0 (uint)
for i = 0; i < per_sub && total_saved < req; i += 64:
    op = (i + t < per_sub) ? rdo_operations[x*per_sub + i + t] : {0, 0}
    saving = op.bos >> 16; blk = op.bos & 0xffff
    incl = inclusive scan of saving over the 64 threads
    if (total_saved + incl - saving < req && saving != 0): InterlockedMax(quant[blk], op.quant)
    total_saved += incl of thread 63
```

The comparisons are unsigned (`uint` vs `int` promotes to `uint` in GLSL; `req > 0` here, so the values are the same).

## 4. GLSL features and cs_5_0 replacements

| GLSL (line) | cs_5_0 |
|---|---|
| `local_size_x_id = 0` (10) | `numthreads(64,1,1)` |
| early `return`s (41-59) | a group-uniform `skip` flag, computed identically by every thread from the same loads. **No `return` before a barrier** |
| loop with data-dependent exit `total_saved < req` (63) | `[loop] for (i = 0; i < per_sub; i += 64)` (cbuffer bound) with `if (!skip && total_saved < req)` predication on the load and atomic. The scan's barriers run every iteration. `total_saved` is updated identically in all threads from groupshared, so the predicate is uniform. fxc cannot prove that, so do not use it as a loop condition around a barrier |
| `subgroupInclusiveAdd` (72) | groupshared inclusive scan over 64 (Hillis-Steele with barriers, or a serial sum of `gs[0..t]`); integers, so exact in any order |
| `subgroupShuffle(scan, size-1)` (78) | read `gs_scan[63]` after the scan barrier; then a barrier before the next iteration overwrites `gs_scan` |
| `atomicMax(int)` (76) | `InterlockedMax(byte_offset, int value)` on `RWByteAddressBuffer` (the signed form is selected by the `int` argument type) |

## 5. Rounding semantics

Integers only. Bit-exact given identical inputs.

## 6. Test oracle

- T-R-1: with **injected Vulkan bucket buffer** (after finalize): `quant` buffer exact.
- T-R-2: end-to-end from injected bands: `quant` buffer exact whenever T-A-2 passed. If T-A-2 failed by FS-A1, report
  the number of 32x32 blocks with a different `quant` value and their bands.
- T-R-3: rate-control sanity against the frame budget: `8 + 4*Σ num_words ≤ target` (exact rate control is the
  reference's stated property, `R/README.md` "Exact rate control"). Check on every frame of every vector, for both
  implementations. A Vulkan failure is an upstream observation (record it under OQ-9); a D3D11-only failure is a bug.
