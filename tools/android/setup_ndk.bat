@echo off
setlocal EnableDelayedExpansion

echo ============================================
echo  ND/Casprix — Android NDK Setup
echo ============================================

:: ── Locate Android SDK ───────────────────────────────────────────────────────
set ANDROID_SDK=%LOCALAPPDATA%\Android\Sdk
if not exist "%ANDROID_SDK%" (
    echo ERROR: Android SDK not found at %ANDROID_SDK%
    echo Install Android Studio or set ANDROID_HOME manually.
    pause & exit /b 1
)
echo [OK] SDK: %ANDROID_SDK%

:: ── Check for sdkmanager ─────────────────────────────────────────────────────
set SDKMGR=
for /d %%d in ("%ANDROID_SDK%\cmdline-tools\*") do (
    if exist "%%d\bin\sdkmanager.bat" set "SDKMGR=%%d\bin\sdkmanager.bat"
)
if not defined SDKMGR (
    if exist "%ANDROID_SDK%\tools\bin\sdkmanager.bat" (
        set "SDKMGR=%ANDROID_SDK%\tools\bin\sdkmanager.bat"
    )
)

if defined SDKMGR (
    echo [OK] sdkmanager: %SDKMGR%
    echo.
    echo Installing NDK 25.2.9519653 and build-tools 34.0.0...
    echo.
    call "%SDKMGR%" --install "ndk;25.2.9519653" "build-tools;34.0.0" "platforms;android-34"
    if %ERRORLEVEL% neq 0 (
        echo.
        echo sdkmanager install failed. Try installing via Android Studio SDK Manager instead.
        pause & exit /b 1
    )
    echo.
    echo [OK] NDK and build-tools installed successfully!
) else (
    echo.
    echo sdkmanager not found. Install NDK manually:
    echo   1. Open Android Studio
    echo   2. File ^> Settings ^> SDK Manager ^> SDK Tools
    echo   3. Check "NDK (Side by side)" and "Android SDK Build-Tools"
    echo   4. Click Apply
    echo.
    echo Or download cmdline-tools:
    echo   https://developer.android.com/studio#command-line-tools-only
    echo   Extract to: %ANDROID_SDK%\cmdline-tools\latest\
    echo   Then re-run this script.
    pause & exit /b 1
)

:: ── Verify NDK ───────────────────────────────────────────────────────────────
set NDK_DIR=%ANDROID_SDK%\ndk\25.2.9519653
if not exist "%NDK_DIR%" (
    echo ERROR: NDK dir not found at %NDK_DIR%
    pause & exit /b 1
)
echo [OK] NDK: %NDK_DIR%

:: ── Verify build-tools ───────────────────────────────────────────────────────
set BT_DIR=%ANDROID_SDK%\build-tools\34.0.0
if not exist "%BT_DIR%" (
    echo WARNING: build-tools 34.0.0 not found at %BT_DIR%
) else (
    echo [OK] build-tools: %BT_DIR%
)

echo.
echo ============================================
echo  DONE! NDK is ready.
echo  Next: run build_android.bat to build Skia
echo  for ARM64 and create the APK.
echo ============================================
pause
