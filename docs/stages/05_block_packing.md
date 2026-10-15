# Stage 5: block packing (`block_packing.comp`)

Status: spec. This is the hardest stage: it produces the bitstream bytes. Source of truth: `R/shaders/block_packing.comp`
(`bp:N`), `R/pyrowave_encoder.cpp:40-51, 211-285, 1002-1088`, `R/pyrowave_common.hpp:14-30`, `R/bitstream/bitstream.md:183-213, 334-449`.

## 1. What the stage does

One 64-thread group packs 2x2 32x32 blocks: 16 threads per 32x32 block, one per 8x8 block (`bp:224-232`).
For each 8x8 block it applies the rate-control plane drop `quant` (from stage 4) to the code word, then copies the
retained magnitude planes from the stage-2 payload and re-packs the sign bits of the coefficients that are still
significant. Per 32x32 block it allocates a word-aligned packet in the bitstream buffer and writes header, code words,
q-scale bytes, planes and signs (`bp:267-378`).

## 2. Dispatches and constants

Per band (quant loop order): `((res32.x+1)/2) x ((res32.y+1)/2)` groups (`R/pyrowave_encoder.cpp:233-270`).
cbuffer = `BlockPackingPushData` (`R/pyrowave_encoder.cpp:40-51`; `bp:84-95`): `int2 resolution @0;
int2 resolution_32x32_blocks @8; int2 resolution_8x8_blocks @16; uint quant_resolution_code @24; uint sequence_code @28;
int block_offset_32x32 @32; int block_stride_32x32 @36; int block_offset_8x8 @40; int block_stride_8x8 @44` (48 bytes).
`quant_resolution_code = encode_quant(1/quant_res)` (`R/pyrowave_encoder.cpp:255-256`); `sequence_code = sequence_count`
(257). It is incremented as `(n+1) & 7` at the start of every `encode()`, so the first frame carries 1 (`R/pyrowave_encoder.cpp:1198`, mask `R/pyrowave_common.hpp:73`).

## 3. Resources

| Vulkan binding (`R/pyrowave_encoder.cpp:216-221`) | HLSL | Layout |
|---|---|---|
| 0 bitstream (`uint`, `uint16_t`, `uint8_t` aliases, `bp:31-44`) | `u0` RWByteAddressBuffer | packets, word-aligned; buffer = `target + meta_size` bytes (`R/pyrowave_c.cpp:1064`) |
| 1 bitstream meta `BitstreamPacket {offset; num_words}` | `u1` RWByteAddressBuffer | 8 bytes per 32x32 block at `8*block_index`; offset in **words** (`R/pyrowave_common.hpp:14-18`) |
| 2 block meta (stage 2) | `t0` ByteAddressBuffer | |
| 3 payloads (`bitstream_payload_counter @4`, `data @8`) | `u2` RWByteAddressBuffer (needs an atomic, so a UAV) | read payload bytes at `8 + offset` with aligned `Load` + shift |
| 4 block stats (stage 2) | `t1` ByteAddressBuffer | |
| 5 quant buffer (stage 4) | `t2` ByteAddressBuffer | `int` per 32x32 block |

That is 3 UAVs. The FL11_0 limit of 8 is not a problem.

## 4. Packet format produced (normative; see `R/bitstream/bitstream.md:183-213, 334-449`)

```
word 0: ballot (16) | payload_words (12) << 16 | sequence (3) << 28 | extended(=0) << 31      bp:302-303
word 1: modify_quant_code(quant_resolution_code, quant) (8) | block_index (24) << 8          bp:304-305
u16 CodeWords[N]   at byte 8            N = popcount(ballot); compacted in ballot bit order    bp:353-354
u8  QScale[N]      at byte 8 + 2N       = code_word >> 16 (low 4: q planes, high 4: quant scale)   bp:355-356
u8  Payload[]      at byte 8 + 3N       per 8x8 block in ballot order, per subblock 0..7, out_planes bytes MSB first
u8  SignPayload[]  after Payload        1 bit per significant coefficient, LSB first in each byte      bp:184-222, 361-378
padding to payload_words*4              never written by the reference
```

The `ballot` bit is `local_block_index.y*4 + local_block_index.x` (row-major), and `N` and the compaction index come from it
(`bp:270-272, 325-326`). For subgroup sizes 32 and 64 the reference extracts this block's 16 bits from the subgroup ballot;
the result is the same for all sizes.

**Don't-care bits.** The reference leaves two things undefined: (a) the bytes between the end of the sign payload and
`payload_words*4`, which are never written and hold stale buffer contents; (b) the bits of the last sign byte above the last
sign bit, which are copied from uninitialized `shared_sign_bank` words (`bp:180, 372-378`). The decoder ignores both
(`R/bitstream/bitstream.md:202-204`). The port writes 0 there. Every byte comparison against Vulkan must mask these bits
(SPEC §5, VALIDATION §4). The mask comes from the CPU parser (§9).

## 5. Exact computation (normative)

Per thread (`bp:226-320`): `blk = index >> 4`; block32 = `2*group + (blk & 1, blk >> 1)`; `lb = index & 15`,
`local = (lb & 3, lb >> 2)`; block8 = `4*block32 + local`.

```
quant = in32 ? quant_buf[bo32 + bs32*y32 + x32] : 0
(meta, nb) = in8 ? (block_meta[b8], stats[b8].errors[min(num_planes, quant)] >> 16) : ({0,0}, 0)
cw = quantize_code_word(meta.code_word, quant)          bp:107-142, literal
active = (cw & 0xffff) != 0
plane_bytes = compute_required_8x8_size(cw)             bp:97-105
sign_bits = nb - plane_bytes*8                          (uint)
bits_meta = nb != 0 ? nb + 24 : 0
words = ceil(Σ_16 bits_meta / 32); if words != 0: words += 2
writes_header = in32 && lb == 15
base = (writes_header && words != 0) ? InterlockedAdd(payload@4, words) : 0, broadcast to the 16 lanes   bp:290-293
lane 15, in32: meta_out[blk32] = {base, words}; if words != 0: write words 0 and 1   bp:295-309
N = popcount(local_ballot)
plane_off = 4*base + 3N + 8 + exclusive_scan_16(plane_bytes)
sign_off_bits = exclusive_scan_16(sign_bits)
sign_region = 4*base + 3N + 8 + Σ_16 plane_bytes
total_sign_bytes = (Σ_16 sign_bits + 7) / 8
active thread: hdr = popcount(local_ballot & ((1 << lb) - 1))
   u16 at 4*base + 8 + 2*hdr = cw & 0xffff;  u8 at 4*base + 8 + 2N + hdr = (cw >> 16) & 0xff
   in = meta.offset; out = plane_off
   for sb in 0..7:
      out_planes = ((cw >> 2sb) & 3) + q_bits(cw);  in_planes = ((meta.cw >> 2sb) & 3) + q_bits(meta.cw); if in_planes: in_planes++
      sign = payload[in]
      if out_planes: sig = OR of payload[in+1 .. in+out_planes], copied to out++;
                     for each set bit k of sig in ascending order: sign bit (sign >> k) & 1 at bit position sign_off_bits++
      in += in_planes
```

`modify_quant_code(c, quant)`: `e = (c >> 3) & 31; e = max(e - quant, 0)`; reinsert (`bp:161-167`).

## 6. GLSL features and cs_5_0 replacements

| GLSL (line) | cs_5_0 |
|---|---|
| `gl_SubgroupInvocationID + gl_SubgroupSize*gl_SubgroupID` (226) | `SV_GroupIndex` |
| `subgroupBallot` + per-size extraction (270-272) | `groupshared uint gs_ballot[4]`, zeroed before barrier 1; `InterlockedOr(gs_ballot[blk], active << lb)`; read after barrier 2 |
| `subgroupClusteredAdd(…, 16)` (285, 317) | integer sum of the 16 groupshared entries of this 32x32 block |
| `inclusive_add_clustered16` via `ShuffleUp` (169-178, 313-314) | serial prefix over the ≤16 groupshared entries (exact integers) |
| `subgroupShuffle(…, | 15)` (293, 320) | broadcast through `groupshared uint gs_base[4]` / totals |
| `atomicAdd(bitstream_payload_counter)` (292) | `InterlockedAdd(u2, 4, words, base)` |
| 8/16-bit SSBO writes (153, 353-356, 366-377) and the 32-bit header writes (302-305) | **all bytes of a packet are assembled in groupshared staging** (`uint gs_pkt[4][640]`, zeroed before barrier 1), using `InterlockedOr` at byte/half/bit granularity. After a barrier the 16 threads of the block copy `words` whole words to `u0` at `4*base` with `Store`. A packet is word-aligned and written only by its own group, so no device-memory RMW race is possible |
| `shared_sign_bank` + `atomicAnd/atomicOr` + full-word fast path (180-222) | unnecessary: OR each sign bit straight into the staging packet at byte `sign_region + (b >> 3)`, bit `b & 7`. This gives the same bit layout for every defined bit; undefined bits become 0 |
| 8-bit SSBO reads `payload_data.data[i]` (150, 340) | `(u2.Load((8+i) & ~3) >> (8*((8+i) & 3))) & 0xff` |
| `float16_t`/`uint16_t` stats (62-72) | 32-bit `Load`, `>> 16` for the cost |
| `subgroupBarrier()` (359) | `GroupMemoryBarrierWithGroupSync()`, at top level |
| `bitCount`, `findLSB`, `bitfieldExtract/Insert` | `countbits`, `firstbitlow`, shifts/masks |

Staging size: with `msb + 1 ≤ 14` (stage 2 §4) a 32x32 packet is at most 8 + 16*(3 + 8*14 + 8) = 1976 bytes (494 words).
Even the 4-bit `q_bits` ceiling (15 + 3 planes) gives 622 words. 640 words per block x 4 = 10 KB of groupshared. If
`words > 640` ever happens, the shader must increment a debug error counter and write nothing for that block. The CPU
side then fails the frame loudly. The test suite must prove the counter stays 0.

## 7. Barrier structure (4 barriers)

1. P0: zero `gs_pkt` (40 words per thread) and `gs_ballot`. **B1.**
2. P1: loads, `quantize_code_word`, `InterlockedOr` ballot, per-lane `plane_bytes`, `sign_bits`, `bits_meta` -> groupshared. **B2.**
3. P2: sums and scans from groupshared; lane 15 allocates and publishes `base`. **B3.**
4. P3: header words (lane 15), code word, q byte, planes and signs into `gs_pkt`; `meta_out`. Divergent per-thread loops are
   fine here, since there is no barrier inside them. **B4.**
5. P4: copy-out of `words` words per block (lanes of that block, stride 16).

There is no `return` anywhere. Out-of-range 8x8/32x32 threads take part in every barrier, with predicates false
(`bp:237-265`).

## 8. Rounding semantics

Integers only. Given identical stage 2-4 outputs the packet bits must be identical, except the don't-care bits (§4).

## 9. Test oracle

- T-P-1: with **injected Vulkan** block meta, payload, stats and quant buffer: for every 32x32 block, `num_words` exact and
  packet bytes exact under the don't-care mask. Packets are located through each side's `meta_out[i].offset`; never compare
  raw bitstream buffers, because packet order comes from atomics.
- T-P-2: CPU parser `validate_packet`, a port of `Encoder::Impl::validate_bitstream` (`R/pyrowave_encoder.cpp:1002-1088`),
  run on every D3D11 packet of every test: header `block_index` equals the slot, `payload_words` equals `num_words`, the walked
  size equals `payload_words`, no 8x8 block out of range. It also produces the don't-care mask (bytes past the walked end;
  bits past `num_significant_values` in the last sign byte).
- T-P-3: the debug counter for staging overflow stays 0.
- T-P-4: the packed frame decodes with the Vulkan decoder (`pyrowave_decoder_push_packet` succeeds for every packet,
  `R/pyrowave_decoder.cpp:157-261`), with PSNR equal to the Vulkan stream's when the bitstreams are equal.
