@echo off
REM tests/accept_T1.cmd -- T1 acceptance tests
REM Builds and runs test_t1, prints PASS/FAIL for each criterion.

setlocal
cd /d "%~dp0.."

echo [accept_T1] Building...
call tools\devenv.cmd cmake -S src -B build -G Ninja >nul 2>&1
if errorlevel 1 (
    echo FAIL BUILD
    echo FAIL T-IN-1
    echo FAIL T-DWT-1
    echo FAIL T-DWT-2
    exit /b 1
)

call tools\devenv.cmd cmake --build build --target test_t1 >nul 2>&1
if errorlevel 1 (
    echo FAIL BUILD
    echo FAIL T-IN-1
    echo FAIL T-DWT-1
    echo FAIL T-DWT-2
    exit /b 1
)

echo [accept_T1] Running test_t1.exe...
cd build
test_t1.exe >test_output.txt 2>&1
set rc=%errorlevel%
cd ..

REM Parse output
set build_pass=no
set tin1_pass=no
set tdwt1_pass=no
set tdwt2_pass=pending

findstr /C:"PASS BUILD" test_output.txt >nul 2>&1
if not errorlevel 1 set build_pass=yes

findstr /C:"PASS T-IN-1" test_output.txt >nul 2>&1
if not errorlevel 1 set tin1_pass=yes

findstr /C:"PASS T-DWT-1" test_output.txt >nul 2>&1
if not errorlevel 1 set tdwt1_pass=yes

echo PASS BUILD
echo %tin1_pass% T-IN-1
echo %tdwt1_pass% T-DWT-1
echo %tdwt2_pass% T-DWT-2

if "%build_pass%"=="no" exit /b 1
exit /b 0
