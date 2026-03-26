@echo off
REM scripts\run_tests.bat — Run the Casprix test suite on Windows
setlocal EnableDelayedExpansion

set "PROJECT_ROOT=%~dp0.."
set "BUILD_DIR=%PROJECT_ROOT%\build"
set CASPRIX=

if exist "%BUILD_DIR%\casprix.exe" (
    set "CASPRIX=%BUILD_DIR%\casprix.exe"
) else if exist "%BUILD_DIR%\bin\casprix.exe" (
    set "CASPRIX=%BUILD_DIR%\bin\casprix.exe"
) else (
    echo ERROR: casprix.exe not found. Run scripts\build.bat first.
    exit /b 1
)

echo === Casprix Compiler Test Suite ===
echo   Compiler: %CASPRIX%
echo   Tests: %PROJECT_ROOT%\tests\compiler\
echo.

set PASS=0
set FAIL=0

for %%f in ("%PROJECT_ROOT%\tests\compiler\*.cpx") do (
    set "name=%%~nf"
    "%CASPRIX%" "%%f" --parse-only >nul 2>&1
    if !errorlevel! == 0 (
        echo   [PASS] !name!
        set /a PASS+=1
    ) else (
        echo   [FAIL] !name!
        set /a FAIL+=1
    )
)

echo.
echo === Results: %PASS% passed, %FAIL% failed ===
if %FAIL% gtr 0 exit /b 1
