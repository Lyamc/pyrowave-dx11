@echo off
rem tools\devenv.cmd - load the x64 MSVC developer environment (cl, link, fxc, ninja) and optionally run a command in it.
rem Usage:  tools\devenv.cmd                     (sets up env in the current cmd session when CALLed)
rem         tools\devenv.cmd <command> [args]    (runs the command inside the env and returns its exit code)
if defined PWD3D11_DEVENV goto :run
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
)
if not defined VSINSTALL set "VSINSTALL=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not exist "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" (
  echo devenv: vcvars64.bat not found under "%VSINSTALL%" 1>&2
  exit /b 9009
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo devenv: vcvars64.bat failed 1>&2
  exit /b 1
)
set "PATH=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;C:\Program Files\CMake\bin;%PATH%"
rem Force MSVC for CMake (Strawberry Perl puts gcc/ninja/cmake on PATH)
set "CC=cl"
set "CXX=cl"
set "PWD3D11_DEVENV=1"
:run
if "%~1"=="" exit /b 0
%*
exit /b %ERRORLEVEL%
