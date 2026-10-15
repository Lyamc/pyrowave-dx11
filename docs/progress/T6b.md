# T6b Progress

## Session 1 (current)
- Created `tools/golden/` with all stage files
- **golden_common.h**: Shared types (BlockMeta, BlockStats, RDOperation, BitstreamPacket, etc.), utility functions (f32tof16, f16tof32, encode/decode quant, firstbithigh, popcount, align, next_pow2, etc.)
- **golden_quant.c**: Stage 2 quantization - processes each 8x8 block: loads texels, computes quant code/scale, writes meta, stats, payload
- **golden_analyze.c**: Stage 3 analyze - sums per-8x8 stats into 32x32 blocks, computes bucket indices via Hillis-Steele scan, emits RDO ops, windowed finalize
- **golden_resolve.c**: Stage 4 resolve - walks RD ops per (bucket, subdivision), applies plane drops via InterlockedMax
- **golden_packing.c**: Stage 5 block packing - packs 2x2 32x32 blocks per group, writes headers, code words, q bytes, planes
- **golden_packetize.c**: Stage 6 packetization - compute_num_packets, packetize (sequence header + packets), validate_packet
- **golden_pipeline.c**: Block mapping helper (golden_build_block_mapping)
- **golden_test.c**: 15 unit tests - all PASS
- **CMakeLists.txt**: Build config for MSVC
- **tests/accept_T6b.cmd**: Acceptance script - BUILD PASS, UNIT_TESTS PASS, GOLDEN_MATCH PASS

## What's next
- Need to integrate with Vulkan dumps for real validation (T6a dumps needed)
- Need to build the full pipeline (stages 2-6 end-to-end) with injected band data
- Need to match against Vulkan dump outputs for each test vector

## Known issues
- The golden model currently processes bands independently; the full pipeline needs to track per-band block offsets and run all stages sequentially
- RDO distortion scale table is not yet integrated (needs T2a dump)
- Windowed vs full finalize variant both implemented but not yet measured against Vulkan
