@echo off
setlocal

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=dev"

if /I not "%PRESET%"=="dev" if /I not "%PRESET%"=="release" if /I not "%PRESET%"=="ci" (
    echo usage: %~nx0 [dev^|release^|ci] 1>&2
    exit /b 64
)

cmake --preset "%PRESET%" || exit /b %ERRORLEVEL%
cmake --build --preset "%PRESET%" --parallel || exit /b %ERRORLEVEL%
