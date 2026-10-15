# Stage 6: per-frame orchestration, readback and packetization

Status: spec. Source of truth: `R/pyrowave_encoder.cpp:178-209, 604-639, 1090-1226`, `R/pyrowave_c.cpp:1017-1156,
1549-1684`, `R/pyrowave_common.cpp:191-256`.

## 1. Init-time state (per encoder, per size)

Mirror `WaveletBuffers::init` and `Encoder::Impl::init_block_meta` (`R/pyrowave_common.cpp:213-256`,
`R/pyrowave_encoder.cpp:178-209`):

- `AW, AH` (stage 1); band textures (stage 1).
- `block_meta[c][l][b] = {block_offset_8x8, block_stride_8x8, block_offset_32x32, block_stride_32x32, block_count_32x32}`
  accumulated for level 4 down to 0, component 0..2 (skip level-0 chroma for 4:2:0), band `(l == 4 ? 0 : 1)..3`. This is
  the bitstream's block index order (`R/bitstream/bitstream.md:504-528`). Port it verbatim; the bitstream depends on it.
- `block_32x32_to_8x8_mapping` (`R/pyrowave_common.cpp:191-211`), needed only by the CPU validator.
- Buffers:

| Buffer | Size | 720p / 1080p (4:2:0) | View(s) |
|---|---|---|---|
| block meta | `block_count_8x8 * 8` | 177,600 / 393,000 | raw UAV (stage 2), raw SRV (stage 5) |
| block stats | `block_count_8x8 * 64` | 1,420,800 / 3,144,000 | raw UAV (2), raw SRV (3, 5) |
| payload | `AW*AH*2` + group padding (stage 2 §5.3) | 1,884,160+ / 4,177,920+ | raw UAV; plus a 2-element raw UAV for clearing the counters |
| quant | `block_count_32x32 * 4` | 5,892 / 13,044 | raw UAV (4), raw SRV (5) |
| bucket | `64 + 8192 + 128*per_sub*16*8` | 2,105,408 / 4,202,560 (`per_sub` 128 / 256) | raw UAV (3), raw SRV (4) |
| bitstream | `target + block_count_32x32*8` (`R/pyrowave_c.cpp:1064`) | depends on budget | raw UAV (5) |
| bitstream meta | `block_count_32x32 * 8` (`R/pyrowave_encoder.cpp:1304-1307`) | 11,784 / 26,088 | raw UAV (5) |
| staging (per frame context) | bitstream + meta sizes | | `D3D11_USAGE_STAGING`, `CPU_ACCESS_READ` |

(The 720p/1080p numbers come from the formulas above: 22,200 / 49,125 8x8 blocks and 1,473 / 3,261 32x32 blocks.)

Raw views need `D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS` and `DXGI_FORMAT_R32_TYPELESS` with
`D3D11_BUFFER_UAV_FLAG_RAW` / `D3D11_BUFFEREX_SRV_FLAG_RAW`. Every buffer is raw, so the byte offsets in the stage docs
apply directly.

## 2. Per-frame command sequence (normative order)

| # | Step | Reference |
|---|---|---|
| 1 | `sequence = (sequence + 1) & 7` | `R/pyrowave_encoder.cpp:1198` |
| 2 | upload the 3 planes (stage 0) | `R/pyrowave_c.cpp:1486-1543` |
| 3 | clear payload bytes 0..7 (both counters), the whole bucket buffer, the whole quant buffer: `ClearUnorderedAccessViewUint` | `R/pyrowave_encoder.cpp:1211-1213` |
| 4 | DWT: 13 dispatches (4:2:0) | stage 1 |
| 5 | quant: 42 dispatches | stage 2 |
| 6 | analyze: 42 dispatches, then finalize: 1 | stage 3 |
| 7 | resolve: 1 dispatch of 2048 groups | stage 4 |
| 8 | packing: 42 dispatches | stage 5 |
| 9 | `CopySubresourceRegion` bitstream meta and bitstream into this context's staging buffers | `R/pyrowave_c.cpp:1123-1131` |
| 10 | `Map(READ)` on demand, in `compute_num_packets`/`packetize` (which wait on the fence, `R/pyrowave_c.cpp:1555-1557, 1607-1608`) | |

D3D11 makes UAV writes of one `Dispatch` visible to the next, which covers the reference's barriers
(`R/pyrowave_encoder.cpp:486-487, 404-411, 334-335, 277-279`). The runtime refuses to bind a resource as SRV and UAV at the
same time and silently unbinds one of them, so unbind explicitly between stages and treat the debug-layer hazard warnings
as errors.

Optional optimizations (identical output, separate tasks): merge each stage's per-band dispatches into one (T2c, T5c);
copy only `payload@4` words of bitstream (needs a second readback round trip, so measure first); two frame contexts
(`PYROWAVE_MAX_FRAME_CONTEXTS = 2`, `R/pyrowave.h:485-493`).

## 3. Packetization (CPU, normative)

Port these verbatim (no GPU involvement):

- `compute_num_critical_packets` / `compute_num_packets` (`R/pyrowave_encoder.cpp:604-639`): a running packet size
  starting at `sizeof(BitstreamSequenceHeader) = 8`; skip blocks with `num_words == 0`; split when
  `size + packet_size + padding > boundary`.
- `packetize` (`R/pyrowave_encoder.cpp:1090-1158`): write the 8-byte `BitstreamSequenceHeader`
  (`R/pyrowave_common.hpp:75-91`) with `width_minus_1`, `height_minus_1` (encoder size, not aligned), `sequence` read from
  the header of `meta[0]`'s packet (1112), `extended = 1`, `code = 0` (start of frame), `total_blocks` = number of blocks
  with `num_words != 0`, `chroma_resolution` (0 = 4:2:0); the colour fields stay 0. Then append blocks in block index
  order (`memcpy` of `num_words*4` bytes from `bitstream + 4*offset`).
- Edge case: when `meta[0].num_words == 0` the reference reads the sequence from whatever packet sits at word
  `meta[0].offset_u32` (0). That is normally another block of the same frame, but stale data if the frame has no blocks
  at all. Reproduce the read; the CPU test asserts the read value equals `sequence` whenever any block exists.
- C API errors: `target = maximum_bitstream_size & ~3`; reject 0 or > `UINT32_MAX` (`R/pyrowave_c.cpp:1058-1062`).

## 4. Features and replacements

| Vulkan | D3D11 |
|---|---|
| `cmd.fill_buffer` | `ClearUnorderedAccessViewUint` (raw UAV) |
| `cmd.copy_buffer` to `CachedHost` | `CopySubresourceRegion` to a `STAGING` buffer, then `Map(D3D11_MAP_READ)` |
| `Fence` per context | `D3D11_QUERY_EVENT` or simply `Map` blocking (no `D3D11_MAP_FLAG_DO_NOT_WAIT`) |
| timestamps per stage (`register_time_interval`) | `D3D11_QUERY_TIMESTAMP` pairs inside `D3D11_QUERY_TIMESTAMP_DISJOINT` (as `prototype/dx11dwt.cpp:167-190`) |
| Granite device / queue | `D3D11CreateDevice` on the first hardware adapter, FL 11_0 requested exactly (`prototype/dx11dwt.cpp:346-359`). Refuse software adapters (`DXGI_ADAPTER_FLAG_SOFTWARE`, WARP) |

## 5. Test oracle

- T-H-1: CPU unit tests of `packetize`/`compute_num_packets` on synthetic meta/bitstream arrays against the reference
  functions built on Linux (same inputs, byte-compare the output and packet lists).
- T-H-2: end to end: packet lists and bytes equal to Vulkan's under the don't-care mask (SPEC §5, VALIDATION §4).
- T-H-3: the host record (`u32 count, {u32 size, bytes}*`, `eq:media/src/pyrowave.rs:470-511`) built from D3D11 output
  decodes in etherquic's own decoder path (`Decoder::decode`, `eq:media/src/pyrowave.rs:548-568`).
