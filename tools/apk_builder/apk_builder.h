/*
 * APK Builder — Packages a Casperix/ND Android program into an .apk file
 *
 * Pipeline:
 *   1. Compile  .cpx → ARM64 native shared library (.so) via Casperix compiler
 *              cross-compiling with --target android-arm64
 *   2. Manifest Generate AndroidManifest.xml from app metadata
 *   3. Resources Pack resources (icons, fonts) into resources.arsc
 *   4. Zip      Bundle everything into an unsigned .apk (ZIP format)
 *   5. Sign     Sign the APK with a debug keystore (v1 JAR signing)
 *   6. Align    zipalign 4-byte alignment for Android runtime loading
 *
 * Prerequisites on PATH:
 *   - casprix      (the Casperix compiler, supports --target android-arm64)
 *   - llvm-ar      (or Android NDK toolchain for linking)
 *   - zipalign     (Android SDK Build Tools)
 *   - apksigner    (Android SDK Build Tools) — optional, falls back to jarsigner
 *
 * Usage:
 *   apk_builder --input MyApp.cpx --package com.example.myapp
 *               --app-name "My App" --version-code 1 --version-name 1.0
 *               --output MyApp.apk [options]
 */

#ifndef APK_BUILDER_H
#define APK_BUILDER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Build Configuration
 * ======================================================================== */

typedef enum {
    APK_ABI_ARM64_V8A = 0,   /* arm64-v8a (default, modern Android) */
    APK_ABI_ARMEABI_V7A,     /* armeabi-v7a (32-bit ARM, legacy) */
    APK_ABI_X86_64,          /* x86_64 (emulator / Chrome OS) */
    APK_ABI_X86,             /* x86 (emulator, legacy) */
} ApkAbi;

typedef enum {
    APK_MIN_SDK_24  = 24,    /* Android 7.0 Nougat (recommended minimum) */
    APK_MIN_SDK_26  = 26,    /* Android 8.0 Oreo */
    APK_MIN_SDK_28  = 28,    /* Android 9.0 Pie */
    APK_MIN_SDK_33  = 33,    /* Android 13 */
} ApkMinSdk;

typedef enum {
    APK_TARGET_SDK_33 = 33,  /* Android 13 */
    APK_TARGET_SDK_34 = 34,  /* Android 14 */
    APK_TARGET_SDK_35 = 35,  /* Android 15 */
} ApkTargetSdk;

typedef struct {
    /* Source */
    char input_file  [512];   /* Path to .cpx source file */
    char package_name[128];   /* e.g. "com.example.myapp" */
    char app_name    [128];   /* Human-readable app name */
    char app_label   [128];   /* Launcher label (defaults to app_name) */
    char main_activity[128];  /* Main activity class name (default: "MainActivity") */

    /* Version */
    int  version_code;        /* Integer version for update comparisons */
    char version_name[32];    /* Human-readable version string e.g. "1.0.0" */

    /* SDK levels */
    int  min_sdk;             /* Minimum Android API level (default: 24) */
    int  target_sdk;          /* Target Android API level (default: 34) */

    /* ABI targets (comma-separated, build all by default) */
    ApkAbi abis[4];
    int    abi_count;

    /* Paths */
    char output_apk   [512];  /* Output .apk path */
    char android_ndk  [512];  /* NDK root (or from ANDROID_NDK_HOME env var) */
    char android_sdk  [512];  /* SDK root (for zipalign/apksigner) */
    char keystore_path[512];  /* Signing keystore (.jks or .keystore) */
    char keystore_pass[128];  /* Keystore password */
    char key_alias    [128];  /* Key alias in keystore */
    char key_pass     [128];  /* Key password */

    /* Resources */
    char icon_path    [512];  /* App icon PNG (512×512) — optional */
    char font_dir     [512];  /* Font directory to bundle — optional */
    char assets_dir   [512];  /* Assets directory — optional */

    /* Build control */
    int  debug_build;         /* 1 = debug signing + debuggable manifest */
    int  verbose;             /* Print detailed build steps */
    int  keep_intermediates;  /* Don't delete build/ temp directory */
} ApkBuildConfig;

/* ========================================================================
 * Build Result
 * ======================================================================== */

typedef enum {
    APK_OK                   = 0,
    APK_ERR_INVALID_CONFIG   = 1,
    APK_ERR_COMPILE_FAILED   = 2,
    APK_ERR_MANIFEST_FAILED  = 3,
    APK_ERR_RESOURCE_FAILED  = 4,
    APK_ERR_ZIP_FAILED       = 5,
    APK_ERR_SIGN_FAILED      = 6,
    APK_ERR_ALIGN_FAILED     = 7,
    APK_ERR_NDK_NOT_FOUND    = 8,
    APK_ERR_SDK_NOT_FOUND    = 9,
    APK_ERR_IO               = 10,
} ApkError;

typedef struct {
    ApkError    code;
    char        message[512];
    char        output_path[512];   /* Populated on success */
    int         apk_size_bytes;
    long long   build_time_ms;
} ApkBuildResult;

/* ========================================================================
 * Public API
 * ======================================================================== */

/* Initialize config with sensible defaults */
void apk_config_init(ApkBuildConfig* config);

/* Main build entry point — runs all pipeline stages */
ApkBuildResult apk_build(const ApkBuildConfig* config);

/* Individual pipeline stages (can be called separately for incremental builds) */
ApkError apk_stage_compile (const ApkBuildConfig* config, const char* build_dir);
ApkError apk_stage_manifest(const ApkBuildConfig* config, const char* build_dir);
ApkError apk_stage_resources(const ApkBuildConfig* config, const char* build_dir);
ApkError apk_stage_package  (const ApkBuildConfig* config, const char* build_dir);
ApkError apk_stage_sign     (const ApkBuildConfig* config, const char* build_dir,
                               const char* unsigned_apk, const char* signed_apk);
ApkError apk_stage_align    (const ApkBuildConfig* config, const char* signed_apk,
                               const char* output_apk);

/* Generate a debug keystore if none exists */
ApkError apk_generate_debug_keystore(const char* keystore_path);

/* Print config summary to stdout */
void apk_config_print(const ApkBuildConfig* config);

/* Return human-readable error string */
const char* apk_error_string(ApkError err);

#ifdef __cplusplus
}
#endif

#endif /* APK_BUILDER_H */
