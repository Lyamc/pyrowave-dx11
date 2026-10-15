> DRAFT by Strata (swift-1.5-iq2_xs), unreviewed. Known errors to fix: RWByteAddressBuffer has no Store8 in cs_5_0 (use aligned-word InterlockedOr / packed RMW, per PLAN.md); GroupMemoryBarrierWithGroupSync must be reached by all 128 threads (no barriers inside lane-divergent code or early return); the floor-vs-trunc difference in compute_square_error must be checked against the GLSL.

# docs/quant_stage_spec.md

## Port Spec: `wavelet_quant.comp` → HLSL `cs_5_0`

### 1. Workgroup & Thread Mapping

- **Workgroup size**: 128 threads (`numthreads(128, 1, 1)`).
- **Emulated subgroup size**: 32 (NVIDIA standard). `subgroup_id = tid / 32`, `lane = tid % 32`.
- **Thread decomposition** (per thread, within its subgroup):
  ```
  local_index = subgroup_id * 32 + lane
  block_local_index = local_index & 7          // bits [0,3)
  block_x = (local_index >> 3) & 3             // bits [3,5)
  block_y = (local_index >> 5) & 3             // bits [5,7)
  ```
- Each thread encodes 8 texels (2×4). The 8 threads sharing `block_local_index` form a **cluster of 8** (the unit for all clustered ops).
- `unswizzle8x8(block_local_index << 3)` maps the 8 threads to spatial positions within an 8×8 sub-block. **OPEN**: exact swizzle table from `dwt_swizzle.h` must be replicated as a lookup in HLSL.

### 2. Bindings & Push Constants

| Vulkan binding | HLSL resource | Notes |
|---|---|---|
| `set=0, binding=0` `sampler2DArray uTexture` | `Texture2DArray` + `SamplerState` | `GatherOffset(s, uv, offset)` |
| `set=0, binding=1` `writeonly SSBOBlockMeta` | `RWStructuredBuffer<BlockMeta>` | 8 bytes/entry |
| `set=0, binding=2` `writeonly SSBOBlockStats` | `RWStructuredBuffer<BlockStats>` | 64 bytes/entry |
| `set=0, binding=3` `buffer Payloads` | `RWByteAddressBuffer` | counter at byte 0, data at byte 8 |
| `push_constant Registers` | `cbuffer Registers` | see below |

**Registers cbuffer** (byte offsets):
```
0:  int2 resolution
8:  int2 resolution_8x8_blocks
16: float2 inv_resolution
24: float input_layer
28: float quant_resolution
32: int block_offset
36: int block_stride
40: float rdo_distortion_scale
```

### 3. Output Buffer Layouts (packed into 32-bit words)

**BlockMeta** (8 bytes = 2 uints):
```
struct BlockMeta { uint code_word; uint offset; }
```

**BlockStats** (64 bytes = 16 uints):
```
struct BlockStats { uint num_planes; uint packed_errors[15]; }
```
Each `packed_errors[i]` packs `QuantStats`:
```
packed = (f32tof16(square_error) << 16) | (payload_cost & 0xFFFF)
```
`f32tof16` is the cs_5_0 intrinsic. Clamp `square_error` to 60000.0 before conversion.

**Payloads** (`RWByteAddressBuffer`):
- Byte 0–3: `counter` (uint). Use `InterlockedAdd(payloads[0], scan)`.
- Byte 8+: `data[]`. Write single bytes via `payloads.Store8(byte_offset, s)`.

### 4. Per-Thread Math (exact sequence)

```
1. Gather 8 texels via two GatherOffset calls (offsets (1,1) and (3,1)), swizzle .wxzy.
2. Scale by quant_resolution (precise multiply).
3. max_subblock_texel = max(max4(abs(t0)), max4(abs(t1)))
4. max_wave_texels = ClusteredMax(max_subblock_texel, 8)
5. compute_quant_scale(max_wave_texels, quant_code, quant_scale):
     if SkipQuantScale || max_wave_texels < 1.0:
         quant_code = 6; quant_scale = 1.0
     else:
         frexp(max_wave_texels - 0.25, e)
         target_max = exp2(float(e)) - 0.25
         inv_scale = max_wave_texels / target_max
         quant_code = uint(ceil((inv_scale - 0.25) * 8.0))
         quant_scale = 1.0 / (float(quant_code)/8.0 + 0.25)
6. texels *= quant_scale; max_wave_texels *= quant_scale; max_subblock_texel *= quant_scale
7. overall_quant_scale = quant_resolution * quant_scale
   inv_quant = 1.0 / overall_quant_scale
   inv_quant_squared = inv_quant * inv_quant
8. abs_quant_texels = abs(int4(texels))  // truncation toward zero
9. max_absolute_value = int(max_wave_texels)
   block4x2_max = int(max_subblock_texel)
10. If max_absolute_value == 0: write zero meta/stats, return.
11. msb = firstbithigh(max_absolute_value)
12. For q = 0..msb: compute_quant_stats(texels, q, msb, block4x2_max, inv_quant_squared)
13. Encode code_word, scan, global_offset, payload bytes.
```

**`compute_square_error(v, q)`** (called per q):
```
v0 = abs(t0); v1 = abs(t1)
iv0 = floor(v0 * exp2(-float(q)))
iv1 = trunc(v1 * exp2(-float(q)))
num_sig = count(iv0 != 0) + count(iv1 != 0)   // per-thread, 8 values
iv0 += (iv0 != 0) ? 0.5 : 0.0
iv1 += (iv1 != 0) ? 0.5 : 0.0
iv0 = trunc(iv0 * exp2(float(q)))
iv1 = trunc(iv1 * exp2(float(q)))
err0 = v0 - iv0; err1 = v1 - iv1
square_error = (dot(err0,err0) + dot(err1,err1)) * rdo_distortion_scale
num_sig = ClusteredAdd(num_sig, 8)
```

**`compute_quant_stats`** (per q):
```
block4x2_shifted = block4x2_max >> q
encode_cost_early = (block4x2_shifted > 0) ? 1 : 0
msb_adj = msb - q
quality_planes = 0
if msb_adj >= 3:
    quality_planes = msb_adj - 2
    encode_cost_early = quality_planes + 1
    block4x2_shifted >>= quality_planes
encode_cost_early += firstbithigh(block4x2_shifted) + 1
encode_cost_late_bits = 8 * ClusteredAdd(max(encode_cost_early - 1, 0), 8) + wave8_num_significants
```

### 5. Subgroup Operations → Groupshared Replacements

All ops operate within a subgroup of 32. Cluster ops use groups of 8.

| Vulkan op | HLSL replacement |
|---|---|
| `subgroupClusteredAdd(v, 8)` | Write `v` to `gs[tid]`; `GroupBarrier(0)`; sum `gs[base..base+7]` where `base = lane & ~7` |
| `subgroupClusteredMax(v, 8)` | Write `v` to `gs[tid]`; barrier; max over `gs[base..base+7]` |
| `subgroupShuffleUp(v, i)` | Write `v` to `gs[tid]`; barrier; read `gs[lane - i]` if `lane >= i` |
| `subgroupShuffle(v, srcLane)` | Write `v` to `gs[tid]`; barrier; read `gs[srcLane]` |
| `subgroupShuffleXor(v, mask)` | Write `v` to `gs[tid]`; barrier; read `gs[lane ^ mask]` |

**`scan_clustered8(v)`** (inclusive prefix sum within cluster of 8):
```
gs[tid] = v; GroupBarrier(0);
base = lane & ~7;
sum = 0;
for j = 0..(lane & 7): sum += gs[base + j];
v = sum;
```

**Groupshared arrays**: Use `groupshared int gs_int[128]` and `groupshared float gs_flt[128]`. Index by `tid` (0–127). Each subgroup uses its own 32-entry slice. Multiple barriers per function call are fine; ensure no data races between subgroups (they don't interact).

### 6. Atomic Replacement

`atomicAdd(payload_data.counter, scan)` executed only by `lane == 7` of each cluster:
```
if (lane == 7)
    global_offset = InterlockedAdd(payloads[0], scan);
global_offset = subgroupShuffle(global_offset, lane | 7);  // broadcast from lane 7
```
In HLSL: `InterlockedAdd` on `RWByteAddressBuffer` at byte offset 0. The shuffle broadcast reads `gs_int[base + 7]` after barrier.

### 7. Payload Byte Writes

After computing `byte_offset = scan + global_offset`:
- Sign byte: `payloads.Store8(byte_offset, s); byte_offset++;`
- Plane bytes: loop `q = plane_iterations-1` down to 0, each writing one byte.
- `Store8` is available in cs_5_0 on `RWByteAddressBuffer`.

### 8. OPEN Items

- **OPEN**: `unswizzle8x8` exact mapping table (from `dwt_swizzle.h`). Must be replicated as a static const array in HLSL.
- **OPEN**: `SkipQuantScale` specialization constant. In HLSL, pass as a `cbuffer` bool; the shader must branch on it.
- **OPEN**: `precise` keyword semantics. HLSL `precise` prevents reassociation. Apply to all multiply chains involving `quant_resolution` and `quant_scale`.
- **OPEN**: `f32tof16` rounding mode. Verify it matches Vulkan's `float16_t()` constructor (round-to-nearest-even). Test with boundary values.
- **OPEN**: `frexp` in HLSL returns mantissa in [0.5, 1). Verify sign handling for negative inputs (max_wave_texels is always ≥ 0.75 after the guard).
- **OPEN**: `firstbithigh(0)` is undefined in HLSL. Guard: `block4x2_shifted` is guaranteed > 0 when `encode_cost_early` is computed (the `> 0` check precedes it). Verify no path reaches `firstbithigh(0)`.
- **OPEN**: Subgroup size assumption. If the target GPU reports a different subgroup size (e.g., 16 on some AMD), the cluster-of-8 logic breaks. **Fix subgroup size to 32** via `numthreads` layout and validate on HD 6670.
