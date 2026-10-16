# T6d Progress

## Done
- Created `tools/pwcompare.py`: compares two PyroWave dump directories, reports per-buffer details and verdict (PASS/FS-ONLY/FAIL)
- Created `tools/genvectors.py`: generates deterministic synthetic YUV420P test vectors (grad, checker1/8/32, noise, flat0/255/128, text, edge)
- Created `tools/mkdump.py`: helper to create minimal dump directories for pwcompare testing
- Created `tests/accept_T6d.cmd`: acceptance test with criteria PWCOMPARE_IDENTICAL, PWCOMPARE_DIFFERENT, PWCOMPARE_MISSING_DIR, PWCOMPARE_STAGE, GENVECTORS, GENVECTORS_ALL, PWCOMPARE_HELP, GENVECTORS_UNKNOWN

## Known issues
- The batch file uses `set ALL_PASS=1` and `%ALL_PASS%` for tracking test results. When tested through `cmd /c "..."` from PowerShell, `%` expansion is intercepted by PowerShell. The harness runs the batch file directly, so this should work correctly.
- pwcompare.py handles shape mismatches between dump directories by using raw data sizes instead of manifest shapes.

## Files created
- `tools/pwcompare.py` - Comparison tool (633 lines)
- `tools/genvectors.py` - Test vector generator (317 lines)
- `tools/mkdump.py` - Dump directory helper (85 lines)
- `tests/accept_T6d.cmd` - Acceptance test (198 lines)
