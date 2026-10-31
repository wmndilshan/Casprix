@echo off
setlocal

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=dev"

if /I "%PRESET%"=="release" (
    ctest --test-dir build/release --output-on-failure
    exit /b %ERRORLEVEL%
)

if /I not "%PRESET%"=="dev" if /I not "%PRESET%"=="ci" (
    echo usage: %~nx0 [dev^|release^|ci] 1>&2
    exit /b 64
)

ctest --preset "%PRESET%"
