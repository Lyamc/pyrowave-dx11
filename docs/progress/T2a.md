# T2a Progress

## Done
1. **rdo_distortion_scale table**: Dumped 60-entry table (4:4:4) and 42-entry table (4:2:0) from reference build on 192.168.0.24 (gcc 15.3.0). Verified bit-exact with MSVC on HYTE. Saved to `src/tables_rdo.inc` (committed ce10e53).
2. **FS-Q2 probe (T-Q-5)**: Computed 16 qs values = 1/(code/8 + 0.25) on both MSVC and gcc - bit-exact. All values correctly rounded. Saved to `src/probe_fs_q2.inc`.
3. **FS-Q1 boundary analysis**: Generated 100 boundary test cases where (inv-0.25)*8 is near integer. See `tools/probe.cpp` (CPU reference only; GPU probe deferred).
4. **OQ-3 f16 rounding sweep**: CPU RNE reference implemented and tested. Results: RNE confirmed, denormals flush to zero below 2^-24, overflow to +/-inf at 65505+. GPU D3D11 f16 test in `tools/f16_rounding_test.cpp` (UAV binding issue - needs fixing).
5. **OQ-1**: Closed - FS-Q2 values are correctly rounded (table in probe_fs_q2.inc).
6. **OQ-3**: Closed - CPU RNE confirmed; GPU sweep deferred to T6/T7.
7. **Acceptance test**: `tests/accept_T2a.cmd` passes all 4 criteria (RDO_TABLE, T-Q-5, OQ-1, OQ-3).

## Open / Deferred
- GPU D3D11 f16 rounding test: UAV binding returns zeros. Needs fix in tools/f16_rounding_test.cpp.
- HD 6670 f16 sweep: Deferred to T7 per task constraints.
- FS-Q1 GPU probe: Deferred to T6 when full pipeline is available.

## Status: ACCEPTANCE PASSING
All 4 acceptance criteria verified: RDO_TABLE, T-Q-5, OQ-1, OQ-3.

