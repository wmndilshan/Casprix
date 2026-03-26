@echo off
REM scripts\build.bat — Build Casprix on Windows (MinGW / MSVC)
setlocal EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_ROOT=%SCRIPT_DIR%.."
set "BUILD_DIR=%PROJECT_ROOT%\build"
set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Release"

echo === Casprix Build ===
echo   Project:    %PROJECT_ROOT%
echo   Build type: %BUILD_TYPE%
echo.

cmake -S "%PROJECT_ROOT%" -B "%BUILD_DIR%" ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DBUILD_COMPILER=ON ^
    -DBUILD_RUNTIME=ON ^
    -DBUILD_TESTS=ON

cmake --build "%BUILD_DIR%" --config %BUILD_TYPE%

echo.
echo === Build complete ===
echo   Compiler:  %BUILD_DIR%\casprix.exe
echo   Runtime:   %BUILD_DIR%\libcasprix_runtime.a
