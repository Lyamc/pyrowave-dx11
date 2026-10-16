@echo off
REM accept_T6d.cmd -- Acceptance test for T6d: pwcompare.py and genvectors.py
REM Usage: accept_T6d.cmd (run from repo root)
REM Prints PASS/FAIL for each criterion, exits 0 only if all pass.

set ALL_PASS=1
set BASE_DIR=%~dp0..
set PYTHON=C:\Users\Lyam\AppData\Local\Programs\Python\Python313\python.exe
set TEMP_DIR=%TEMP%\pwcompare_test

REM Clean up any previous test run
if exist "%TEMP_DIR%" rmdir /s /q "%TEMP_DIR%"
mkdir "%TEMP_DIR%"

REM --- GENVECTORS ---
echo accept_T6d: testing genvectors.py...

%PYTHON% "%BASE_DIR%\tools\genvectors.py" "%TEMP_DIR%\vectors" -v grad noise flat0 -s 16x16 128x128 -b 32768 2>&1 | findstr /C:"error" /C:"Error" /C:"Traceback" >nul
if %errorlevel% equ 0 (
    echo FAIL GENVECTORS
    set ALL_PASS=0
    goto :check_dump
)

REM Check that output files exist
set GEN_OK=1
if not exist "%TEMP_DIR%\vectors\grad_16x16\manifest.json" set GEN_OK=0
if not exist "%TEMP_DIR%\vectors\grad_16x16\grad_16x16.yuv" set GEN_OK=0
if not exist "%TEMP_DIR%\vectors\noise_128x128\manifest.json" set GEN_OK=0
if not exist "%TEMP_DIR%\vectors\noise_128x128\noise_128x128.yuv" set GEN_OK=0
if not exist "%TEMP_DIR%\vectors\flat0_16x16\manifest.json" set GEN_OK=0
if not exist "%TEMP_DIR%\vectors\flat0_16x16\frame_0000\frame_0000.yuv" set GEN_OK=0

if not %GEN_OK% equ 1 (
    echo FAIL GENVECTORS -- missing output files
    set ALL_PASS=0
) else (
    echo PASS GENVECTORS
)

REM --- Create dump dirs for pwcompare testing ---
:check_dump
echo accept_T6d: creating dump dirs...

%PYTHON% "%BASE_DIR%\tools\mkdump.py" "%TEMP_DIR%\dir_a" 9 32768
if %errorlevel% neq 0 (
    echo FAIL PWCOMPARE_SETUP
    set ALL_PASS=0
    goto :end
)
%PYTHON% "%BASE_DIR%\tools\mkdump.py" "%TEMP_DIR%\dir_b" 9 32768
if %errorlevel% neq 0 (
    echo FAIL PWCOMPARE_SETUP
    set ALL_PASS=0
    goto :end
)
%PYTHON% "%BASE_DIR%\tools\mkdump.py" "%TEMP_DIR%\dir_c" 9 32768
if %errorlevel% neq 0 (
    echo FAIL PWCOMPARE_SETUP
    set ALL_PASS=0
    goto :end
)
%PYTHON% "%BASE_DIR%\tools\mkdump.py" "%TEMP_DIR%\dir_d" 12 16384
if %errorlevel% neq 0 (
    echo FAIL PWCOMPARE_SETUP
    set ALL_PASS=0
    goto :end
)

REM Verify dump dirs have manifest.json
if not exist "%TEMP_DIR%\dir_a\manifest.json" goto :dump_fail
if not exist "%TEMP_DIR%\dir_b\manifest.json" goto :dump_fail
goto :dump_ok

:dump_fail
echo FAIL PWCOMPARE_SETUP -- no manifest
set ALL_PASS=0
goto :end

:dump_ok
echo accept_T6d: manifest files found

REM --- PWCOMPARE: identical directories ---
echo accept_T6d: testing pwcompare.py (identical dirs)...

%PYTHON% "%BASE_DIR%\tools\pwcompare.py" "%TEMP_DIR%\dir_a" "%TEMP_DIR%\dir_b" >"%TEMP_DIR%\cmp_same.txt" 2>&1
findstr /C:"VERDICT: PASS" "%TEMP_DIR%\cmp_same.txt" >nul
if %errorlevel% neq 0 (
    echo FAIL PWCOMPARE_IDENTICAL
    type "%TEMP_DIR%\cmp_same.txt"
    set ALL_PASS=0
) else (
    echo PASS PWCOMPARE_IDENTICAL
)

REM --- PWCOMPARE: different directories ---
echo accept_T6d: testing pwcompare.py (different dirs)...

%PYTHON% "%BASE_DIR%\tools\pwcompare.py" "%TEMP_DIR%\dir_c" "%TEMP_DIR%\dir_d" >"%TEMP_DIR%\cmp_diff.txt" 2>&1
findstr /C:"VERDICT:" "%TEMP_DIR%\cmp_diff.txt" >nul
if %errorlevel% neq 0 (
    echo FAIL PWCOMPARE_DIFFERENT -- no verdict
    type "%TEMP_DIR%\cmp_diff.txt"
    set ALL_PASS=0
) else (
    findstr /C:"VERDICT: FAIL" "%TEMP_DIR%\cmp_diff.txt" >nul
    if %errorlevel% equ 0 (
        echo PASS PWCOMPARE_DIFFERENT
    ) else (
        findstr /C:"VERDICT: FS-ONLY" "%TEMP_DIR%\cmp_diff.txt" >nul
        if %errorlevel% equ 0 (
            echo PASS PWCOMPARE_DIFFERENT
        ) else (
            echo FAIL PWCOMPARE_DIFFERENT -- unexpected verdict
            type "%TEMP_DIR%\cmp_diff.txt"
            set ALL_PASS=0
        )
    )
)

REM --- PWCOMPARE: missing directory ---
echo accept_T6d: testing pwcompare.py (missing dir)...

%PYTHON% "%BASE_DIR%\tools\pwcompare.py" "%TEMP_DIR%\dir_a" "%TEMP_DIR%\nonexistent" >"%TEMP_DIR%\cmp_missing.txt" 2>&1
findstr /C:"ERROR" "%TEMP_DIR%\cmp_missing.txt" >nul
if %errorlevel% neq 0 (
    echo FAIL PWCOMPARE_MISSING_DIR
    type "%TEMP_DIR%\cmp_missing.txt"
    set ALL_PASS=0
) else (
    echo PASS PWCOMPARE_MISSING_DIR
)

REM --- PWCOMPARE: --stage filter ---
echo accept_T6d: testing pwcompare.py (--stage filter)...

%PYTHON% "%BASE_DIR%\tools\pwcompare.py" "%TEMP_DIR%\dir_a" "%TEMP_DIR%\dir_b" --stage 2 >"%TEMP_DIR%\cmp_stage.txt" 2>&1
findstr /C:"VERDICT:" "%TEMP_DIR%\cmp_stage.txt" >nul
if %errorlevel% neq 0 (
    echo FAIL PWCOMPARE_STAGE
    type "%TEMP_DIR%\cmp_stage.txt"
    set ALL_PASS=0
) else (
    echo PASS PWCOMPARE_STAGE
)

REM --- GENVECTORS: all vectors ---
echo accept_T6d: testing genvectors.py (all vectors)...

%PYTHON% "%BASE_DIR%\tools\genvectors.py" "%TEMP_DIR%\vectors_all" -v grad checker1 checker8 noise flat0 flat255 flat128 text edge -s 16x16 -b 32768 >"%TEMP_DIR%\gen_all.txt" 2>&1
findstr /C:"Generated 9 test vectors" "%TEMP_DIR%\gen_all.txt" >nul
if %errorlevel% neq 0 (
    echo FAIL GENVECTORS_ALL
    type "%TEMP_DIR%\gen_all.txt"
    set ALL_PASS=0
) else (
    echo PASS GENVECTORS_ALL
)

REM --- PWCOMPARE: tool help ---
echo accept_T6d: testing pwcompare.py --help...

%PYTHON% "%BASE_DIR%\tools\pwcompare.py" --help >nul 2>&1
if %errorlevel% neq 0 (
    echo FAIL PWCOMPARE_HELP
    set ALL_PASS=0
) else (
    echo PASS PWCOMPARE_HELP
)

REM --- GENVECTORS: unknown vector ---
echo accept_T6d: testing genvectors.py (unknown vector)...

%PYTHON% "%BASE_DIR%\tools\genvectors.py" "%TEMP_DIR%\bad" -v unknown >"%TEMP_DIR%\gen_bad.txt" 2>&1
findstr /C:"error:" "%TEMP_DIR%\gen_bad.txt" >nul
if %errorlevel% neq 0 (
    echo FAIL GENVECTORS_UNKNOWN
    type "%TEMP_DIR%\gen_bad.txt"
    set ALL_PASS=0
) else (
    echo PASS GENVECTORS_UNKNOWN
)

:end
REM Clean up
if exist "%TEMP_DIR%" rmdir /s /q "%TEMP_DIR%"

if %ALL_PASS% equ 0 (
    echo accept_T6d: SOME FAILURES
    exit /b 1
)
echo accept_T6d: ALL PASS
exit /b 0
