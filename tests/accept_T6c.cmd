@echo off
REM accept_T6c.cmd -- T6c acceptance test
REM Builds test_t6c and runs it. Prints PASS/FAIL for each criterion.
REM Usage: run from repo root, no arguments.

setlocal

set ALL_PASS=1

REM Build
call tools\devenv.cmd cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >nul 2>&1
if errorlevel 1 (
    echo FAIL BUILD
    echo FAIL ROUNDTRIP
    exit /b 1
)

call tools\devenv.cmd cmake --build build --target test_t6c >nul 2>&1
if errorlevel 1 (
    echo FAIL BUILD
    echo FAIL ROUNDTRIP
    exit /b 1
)

REM Run test_t6c and capture output
set TMP_OUT=%TEMP%\t6c_out.txt
build\test_t6c.exe > "%TMP_OUT%" 2>&1

REM Check BUILD from test output (test prints PASS BUILD)
findstr /C:"PASS BUILD" "%TMP_OUT%" >nul 2>&1
if not errorlevel 1 (
    echo PASS BUILD
) else (
    echo FAIL BUILD
    set ALL_PASS=0
)

REM Check ROUNDTRIP from test output
findstr /C:"PASS ROUNDTRIP" "%TMP_OUT%" >nul 2>&1
if not errorlevel 1 (
    echo PASS ROUNDTRIP
) else (
    echo FAIL ROUNDTRIP
    set ALL_PASS=0
)

REM Check that dump files were actually written
set DUMP_DIR=%TEMP%\pyrowave_d3d11_t6c_32x32
if exist "%DUMP_DIR%\manifest.json" (
    if exist "%DUMP_DIR%\band_c0_l0_b0.bin" (
        echo INFO dumps written correctly
    )
)

del "%TMP_OUT%" >nul 2>&1

if "%ALL_PASS%"=="0" exit /b 1
exit /b 0
