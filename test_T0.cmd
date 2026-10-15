@echo off
REM test_T0.cmd -- T0 acceptance script
REM Run from repo root: call test_T0.cmd
REM Prints PASS/FAIL per criterion, exits 0 only if all pass.

setlocal enabledelayedexpansion

set ALL_PASS=1

REM Build
echo [build] cmake + ninja ...
call tools\devenv.cmd cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >nul 2>&1
if !errorlevel! neq 0 (
    echo FAIL BUILD
    set ALL_PASS=0
    goto :report
)
call tools\devenv.cmd cmake --build build >nul 2>&1
if !errorlevel! neq 0 (
    echo FAIL BUILD
    set ALL_PASS=0
    goto :report
)
echo PASS BUILD

REM Run tests and capture output
echo [test] running test_t0 ...
build\test_t0.exe > build\test_output.txt 2>&1

REM Parse test output for individual criteria
set CREATES=0
set REJECTS=0
set REJECTS_FLAG=0
set TIN0=0

findstr /C:"CREATE_DEVICE ... PASS" build\test_output.txt >nul && set CREATES=1
findstr /C:"REJECT_WARP ... PASS" build\test_output.txt >nul && set REJECTS=1
findstr /C:"REJECT_WARP_FLAG ... PASS" build\test_output.txt >nul && set REJECTS_FLAG=1
findstr /C:"T-IN-0 ... PASS" build\test_output.txt >nul && set TIN0=1

type build\test_output.txt

if !CREATES! equ 1 (echo PASS CREATE_DEVICE) else (echo FAIL CREATE_DEVICE & set ALL_PASS=0)
if !REJECTS! equ 1 (echo PASS REJECT_WARP) else (echo FAIL REJECT_WARP & set ALL_PASS=0)
if !REJECTS_FLAG! equ 1 (echo PASS REJECT_WARP_FLAG) else (echo FAIL REJECT_WARP_FLAG & set ALL_PASS=0)
if !TIN0! equ 1 (echo PASS T-IN-0) else (echo FAIL T-IN-0 & set ALL_PASS=0)

:report
if !ALL_PASS! equ 0 exit /b 1
exit /b 0
