@echo off
REM T2a acceptance test
setlocal EnableDelayedExpansion

set ALL_PASS=1

REM Check 1: RDO_TABLE
echo Checking RDO_TABLE...
findstr /c:rdo_distortion_scale_table_444 src\tables_rdo.inc >nul 2>&1
if !ERRORLEVEL! neq 0 (
    echo FAIL RDO_TABLE
    set ALL_PASS=0
) else (
    echo PASS RDO_TABLE
)

REM Check 2: T-Q-5
echo Checking T-Q-5...
if exist src\probe_fs_q2.inc (
    findstr /c:fs_q2_qs_expected src\probe_fs_q2.inc >nul 2>&1
    if !ERRORLEVEL! equ 0 (
        echo PASS T-Q-5
    ) else (
        echo FAIL T-Q-5
        set ALL_PASS=0
    )
) else (
    echo FAIL T-Q-5
    set ALL_PASS=0
)

REM Check 3: OQ-1
echo Checking OQ-1...
findstr /c:OQ-1 docs\OPEN_QUESTIONS.md >nul 2>&1
if !ERRORLEVEL! equ 0 (
    findstr /c:OQ-1 docs\OPEN_QUESTIONS.md | findstr /c:Closed >nul 2>&1
    if !ERRORLEVEL! equ 0 (
        echo PASS OQ-1
    ) else (
        echo FAIL OQ-1
        set ALL_PASS=0
    )
) else (
    echo FAIL OQ-1
    set ALL_PASS=0
)

REM Check 4: OQ-3
echo Checking OQ-3...
findstr /c:OQ-3 docs\OPEN_QUESTIONS.md >nul 2>&1
if !ERRORLEVEL! equ 0 (
    findstr /c:OQ-3 docs\OPEN_QUESTIONS.md | findstr /c:Closed >nul 2>&1
    if !ERRORLEVEL! equ 0 (
        echo PASS OQ-3
    ) else (
        echo FAIL OQ-3
        set ALL_PASS=0
    )
) else (
    echo FAIL OQ-3
    set ALL_PASS=0
)

if !ALL_PASS! equ 1 (
    echo ALL PASS
    exit /b 0
) else (
    echo SOME FAILURES
    exit /b 1
)
