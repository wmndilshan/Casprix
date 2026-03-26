@echo off
setlocal EnableDelayedExpansion

echo ============================================
echo  Download pre-built Skia for Android ARM64
echo ============================================
echo.

set OUT_DIR=%~dp0..\..\third_party\skia\lib-android-arm64
set TAR_FILE=%OUT_DIR%\skia-android-arm64.tar.gz

if exist "%OUT_DIR%\libskia.a" (
    echo [OK] Skia ARM64 already exists: %OUT_DIR%\libskia.a
    goto :done
)

mkdir "%OUT_DIR%" 2>nul

echo Downloading pre-built Skia for android-arm64...
echo.

pushd "%~dp0"
java DownloadFile.java ^
    "https://github.com/aspect-build/aspect-skia/releases/latest/download/skia-android-arm64.tar.gz" ^
    "%TAR_FILE%"
popd

if exist "%TAR_FILE%" (
    echo Extracting downloaded archive...
    pushd "%OUT_DIR%"
    tar xzf skia-android-arm64.tar.gz
    popd
    if exist "%OUT_DIR%\libskia.a" (
        del "%TAR_FILE%" >nul 2>&1
        echo [OK] Skia ARM64 ready from download.
        goto :done
    )
)

echo.
echo Pre-built download failed. Falling back to local source build...
echo.
call "%~dp0build_skia_arm64.bat"
if exist "%OUT_DIR%\libskia.a" (
    echo [OK] Skia ARM64 ready from local source build.
    goto :done
)

echo.
echo ERROR: Could not prepare %OUT_DIR%\libskia.a
echo.
exit /b 1

:done
echo.
echo Location: %OUT_DIR%
dir /b "%OUT_DIR%"
exit /b 0
