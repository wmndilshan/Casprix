@echo off
setlocal EnableDelayedExpansion

echo ============================================
echo  ND/Casprix — Android ARM64 Build
echo  (Skia-native, no Java, like Flutter)
echo ============================================
echo.

:: ── Configuration ────────────────────────────────────────────────────────────
set PROJECT_DIR=%~dp0..\..
set ANDROID_SDK=%LOCALAPPDATA%\Android\Sdk
set ANDROID_ABI=arm64-v8a
set ANDROID_PLATFORM=android-24
set MIN_SDK=24
set TARGET_SDK=34
set BUILD_DIR=%PROJECT_DIR%\build-android

:: ── Find NDK ─────────────────────────────────────────────────────────────────
set NDK_DIR=
for /d %%d in ("%ANDROID_SDK%\ndk\*") do set "NDK_DIR=%%d"
if not defined NDK_DIR (
    echo ERROR: No NDK found in %ANDROID_SDK%\ndk\
    echo Run setup_ndk.bat first.
    pause & exit /b 1
)
echo [OK] NDK: %NDK_DIR%

set TOOLCHAIN=%NDK_DIR%\build\cmake\android.toolchain.cmake
if not exist "%TOOLCHAIN%" (
    echo ERROR: CMake toolchain not found at %TOOLCHAIN%
    pause & exit /b 1
)
echo [OK] Toolchain: %TOOLCHAIN%

:: ── Find build-tools (for zipalign + apksigner) ─────────────────────────────
set BT_DIR=
for /d %%d in ("%ANDROID_SDK%\build-tools\*") do set "BT_DIR=%%d"
if not defined BT_DIR (
    echo WARNING: No build-tools found — APK signing will be skipped
) else (
    echo [OK] build-tools: %BT_DIR%
)

:: ── Find aapt2 ───────────────────────────────────────────────────────────────
set AAPT2=
if defined BT_DIR (
    if exist "%BT_DIR%\aapt2.exe" set "AAPT2=%BT_DIR%\aapt2.exe"
)
if defined AAPT2 (
    echo [OK] aapt2: %AAPT2%
) else (
    echo WARNING: aapt2 not found
)

:: ── Step 1: Cross-compile Skia + Casprix for ARM64 ──────────────────────────
echo.
echo ── Step 1: CMAKE Configure (ARM64) ──
echo.

cmake -B "%BUILD_DIR%" ^
    -G "Ninja" ^
    -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN%" ^
    -DANDROID_ABI=%ANDROID_ABI% ^
    -DANDROID_PLATFORM=%ANDROID_PLATFORM% ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DSKIA_DIR="%PROJECT_DIR%\third_party\skia" ^
    -S "%PROJECT_DIR%\runtime\android"

if %ERRORLEVEL% neq 0 (
    echo.
    echo CMake configure failed.
    echo Make sure Ninja is installed: winget install Ninja-build.Ninja
    pause & exit /b 1
)

echo.
echo ── Step 2: Build libMainActivity.so ──
echo.

cmake --build "%BUILD_DIR%" --config Release -j 8

if %ERRORLEVEL% neq 0 (
    echo.
    echo Build failed. Check errors above.
    pause & exit /b 1
)

set SO_FILE=%BUILD_DIR%\libMainActivity.so
if not exist "%SO_FILE%" (
    echo ERROR: libMainActivity.so not produced
    pause & exit /b 1
)
echo [OK] Native library: %SO_FILE%

:: ── Step 3: Package APK ──────────────────────────────────────────────────────
echo.
echo ── Step 3: Package APK ──
echo.

set APK_DIR=%BUILD_DIR%\apk
set APK_UNSIGNED=%APK_DIR%\casprix-unsigned.apk
set APK_ALIGNED=%APK_DIR%\casprix-aligned.apk
set APK_SIGNED=%APK_DIR%\casprix.apk

if exist "%APK_DIR%" rmdir /s /q "%APK_DIR%"
mkdir "%APK_DIR%"
mkdir "%APK_DIR%\lib\arm64-v8a"

:: Copy native library
copy /y "%SO_FILE%" "%APK_DIR%\lib\arm64-v8a\libMainActivity.so" >nul

:: Generate AndroidManifest.xml
(
echo ^<?xml version="1.0" encoding="utf-8"?^>
echo ^<manifest xmlns:android="http://schemas.android.com/apk/res/android"
echo     package="com.casprix.app"
echo     android:versionCode="1"
echo     android:versionName="1.0"^>
echo     ^<uses-sdk android:minSdkVersion="%MIN_SDK%" android:targetSdkVersion="%TARGET_SDK%" /^>
echo     ^<uses-feature android:glEsVersion="0x00030000" android:required="true" /^>
echo     ^<application android:label="Casprix" android:hasCode="false"^>
echo         ^<activity android:name="android.app.NativeActivity" android:exported="true"
echo             android:configChanges="orientation|keyboardHidden|screenSize"
echo             android:theme="@android:style/Theme.DeviceDefault.NoActionBar"
echo             android:windowSoftInputMode="adjustResize"^>
echo             ^<meta-data android:name="android.app.lib_name" android:value="MainActivity" /^>
echo             ^<intent-filter^>
echo                 ^<action android:name="android.intent.action.MAIN" /^>
echo                 ^<category android:name="android.intent.category.LAUNCHER" /^>
echo             ^</intent-filter^>
echo         ^</activity^>
echo     ^</application^>
echo ^</manifest^>
) > "%APK_DIR%\AndroidManifest.xml"

:: Use aapt2 to compile + link resources and create APK
if defined AAPT2 (
    echo Using aapt2 to create APK...
    mkdir "%APK_DIR%\res"

    :: Create a minimal resource: values/strings.xml
    mkdir "%APK_DIR%\res\values"
    (
    echo ^<?xml version="1.0" encoding="utf-8"?^>
    echo ^<resources^>
    echo     ^<string name="app_name"^>Casprix^</string^>
    echo ^</resources^>
    ) > "%APK_DIR%\res\values\strings.xml"

    :: Compile resources
    "%AAPT2%" compile "%APK_DIR%\res\values\strings.xml" -o "%APK_DIR%\compiled_res.zip"

    :: Find android.jar for linking
    set ANDROID_JAR=
    for /d %%d in ("%ANDROID_SDK%\platforms\android-*") do set "ANDROID_JAR=%%d\android.jar"
    if not defined ANDROID_JAR (
        echo ERROR: No android.jar found in platforms/
        pause & exit /b 1
    )
    echo [OK] android.jar: %ANDROID_JAR%

    :: Link into APK
    "%AAPT2%" link ^
        -o "%APK_UNSIGNED%" ^
        --manifest "%APK_DIR%\AndroidManifest.xml" ^
        -I "!ANDROID_JAR!" ^
        "%APK_DIR%\compiled_res.zip" ^
        --auto-add-overlay

    if %ERRORLEVEL% neq 0 (
        echo aapt2 link failed!
        pause & exit /b 1
    )

    :: Inject native library into the APK (it's a ZIP)
    cd /d "%APK_DIR%"
    jar uf "%APK_UNSIGNED%" lib\arm64-v8a\libMainActivity.so
) else (
    :: Fallback: create APK as a ZIP manually using jar
    echo Creating APK manually (no aapt2)...
    cd /d "%APK_DIR%"
    jar cf "%APK_UNSIGNED%" AndroidManifest.xml lib\arm64-v8a\libMainActivity.so
)

if not exist "%APK_UNSIGNED%" (
    echo ERROR: APK creation failed
    pause & exit /b 1
)
echo [OK] Unsigned APK: %APK_UNSIGNED%

:: ── Step 4: Align (zipalign) ─────────────────────────────────────────────────
if defined BT_DIR (
    echo Aligning APK...
    "%BT_DIR%\zipalign.exe" -f 4 "%APK_UNSIGNED%" "%APK_ALIGNED%"
    if %ERRORLEVEL% neq 0 (
        echo WARNING: zipalign failed, using unaligned APK
        copy /y "%APK_UNSIGNED%" "%APK_ALIGNED%" >nul
    )
) else (
    copy /y "%APK_UNSIGNED%" "%APK_ALIGNED%" >nul
)

:: ── Step 5: Sign (debug key) ────────────────────────────────────────────────
echo Signing APK with debug key...

set DEBUG_KS=%USERPROFILE%\.android\debug.keystore
if not exist "%DEBUG_KS%" (
    echo Generating debug keystore...
    keytool -genkeypair -v -keystore "%DEBUG_KS%" ^
        -alias androiddebugkey -keyalg RSA -keysize 2048 ^
        -validity 10000 -storepass android -keypass android ^
        -dname "CN=Android Debug,O=Android,C=US"
)

if defined BT_DIR (
    if exist "%BT_DIR%\apksigner.bat" (
        echo Using apksigner...
        call "%BT_DIR%\apksigner.bat" sign ^
            --ks "%DEBUG_KS%" --ks-pass pass:android ^
            --ks-key-alias androiddebugkey ^
            --out "%APK_SIGNED%" "%APK_ALIGNED%"
    ) else (
        echo Using jarsigner fallback...
        jarsigner -keystore "%DEBUG_KS%" -storepass android ^
            -keypass android -signedjar "%APK_SIGNED%" ^
            "%APK_ALIGNED%" androiddebugkey
    )
) else (
    echo Using jarsigner...
    jarsigner -keystore "%DEBUG_KS%" -storepass android ^
        -keypass android -signedjar "%APK_SIGNED%" ^
        "%APK_ALIGNED%" androiddebugkey
)

if not exist "%APK_SIGNED%" (
    echo ERROR: APK signing failed
    pause & exit /b 1
)
echo [OK] Signed APK: %APK_SIGNED%

:: ── Step 6: Install + Launch ─────────────────────────────────────────────────
echo.
echo ============================================
echo  BUILD COMPLETE: %APK_SIGNED%
echo ============================================
echo.

:: Check for connected device
adb devices | findstr "device$" >nul 2>&1
if %ERRORLEVEL%==0 (
    echo Installing to connected device...
    adb install -r "%APK_SIGNED%"
    if %ERRORLEVEL%==0 (
        echo.
        echo Launching Casprix on device...
        adb shell am start -n com.casprix.app/android.app.NativeActivity
        echo.
        echo ============================================
        echo  APP RUNNING ON DEVICE!
        echo ============================================
    ) else (
        echo ADB install failed.
    )
) else (
    echo No device connected. Install manually:
    echo   adb install "%APK_SIGNED%"
)

pause
