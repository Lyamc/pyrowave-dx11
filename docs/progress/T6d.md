# T6d Progress

## Done
- Created `tools/pwcompare.py`: compares two PyroWave dump directories, reports per-buffer details and verdict (PASS/FS-ONLY/FAIL)
- Created `tools/genvectors.py`: generates deterministic synthetic YUV420P test vectors (grad, checker1/8/32, noise, flat0/255/128, text, edge)
- Created `tools/mkdump.py`: helper to create minimal dump directories for pwcompare testing
- Created `tests/accept_T6d.cmd`: acceptance test with criteria PWCOMPARE_IDENTICAL, PWCOMPARE_DIFFERENT, PWCOMPARE_MISSING_DIR, PWCOMPARE_STAGE, GENVECTORS, GENVECTORS_ALL, PWCOMPARE_HELP, GENVECTORS_UNKNOWN
- Fixed batch file bug: parentheses in `echo` statements inside `if...else` blocks broke batch file parsing. Replaced with `--` dashes.

## All 8 criteria PASS
- PASS GENVECTORS
- PASS PWCOMPARE_IDENTICAL
- PASS PWCOMPARE_DIFFERENT
- PASS PWCOMPARE_MISSING_DIR
- PASS PWCOMPARE_STAGE
- PASS GENVECTORS_ALL
- PASS PWCOMPARE_HELP
- PASS GENVECTORS_UNKNOWN

## Files
- `tools/pwcompare.py` - Comparison tool (633 lines)
- `tools/genvectors.py` - Test vector generator (319 lines)
- `tools/mkdump.py` - Dump directory helper (86 lines)
- `tests/accept_T6d.cmd` - Acceptance test (193 lines)
