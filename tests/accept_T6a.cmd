@echo off
setlocal enabledelayedexpansion
pushd "%~dp0.." >nul
set BASE_DIR=%CD%
popd >nul
C:\Users\Lyam\AppData\Local\Programs\Python\Python313\python.exe "%BASE_DIR%\tests\accept_T6a.py"
exit /b !ERRORLEVEL!
