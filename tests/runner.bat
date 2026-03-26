@echo off
setlocal enabledelayedexpansion
set ROOT=%~dp0..
set CASPRIX=%ROOT%\build\casprix.exe
set TESTS=%~dp0compiler
set PASS=0
set FAIL=0

echo === Casprix Compiler Test Suite ===
echo Binary : %CASPRIX%
echo Tests  : %TESTS%
echo.

if not exist "%CASPRIX%" (
    echo ERROR: casprix.exe not found at %CASPRIX% -- run cmake build first
    exit /b 1
)

for %%f in ("%TESTS%\test_*.cpx") do (
    set NAME=%%~nf
    "%CASPRIX%" "%%f" >nul 2>&1
    if !errorlevel! == 0 (
        echo   PASS  !NAME!
        set /a PASS+=1
    ) else (
        echo   FAIL  !NAME!
        set /a FAIL+=1
    )
)

echo.
echo Results: %PASS% passed, %FAIL% failed
if %FAIL% gtr 0 exit /b 1
