/*
 * main.c — Command-line interface for the Casperix APK Builder
 *
 * Usage:
 *   apk-builder [options] --input <file.cpx>
 *
 * Required:
 *   --input  <path>      Source .cpx file
 *   --package <name>     Android package name (e.g. com.example.myapp)
 *   --name   <text>      App display name
 *
 * Optional:
 *   --output  <path>     Output APK path (default: app-release.apk)
 *   --version-code <n>   Integer version (default: 1)
 *   --version-name <s>   Version string (default: 1.0.0)
 *   --min-sdk  <n>       Minimum Android API level (default: 24)
 *   --target-sdk <n>     Target Android API level (default: 34)
 *   --ndk  <path>        Android NDK path (or set ANDROID_NDK_HOME)
 *   --sdk  <path>        Android SDK path (or set ANDROID_HOME)
 *   --keystore <path>    Signing keystore (.jks / .keystore)
 *   --ks-pass  <pass>    Keystore password
 *   --key-alias <alias>  Key alias in keystore
 *   --key-pass  <pass>   Key password
 *   --icon  <path>       App icon PNG (512×512)
 *   --assets <dir>       Assets directory to bundle
 *   --fonts  <dir>       Font directory to bundle
 *   --activity <name>    Main activity name (default: MainActivity)
 *   --abi  <name>        ABI to build: arm64-v8a|armeabi-v7a|x86_64|x86
 *                        (can be repeated; default: arm64-v8a)
 *   --release            Release build (no debug flag in manifest)
 *   --quiet              Suppress verbose output
 *   --keep-build         Keep intermediate build directory
 *   --help               Print this help
 */

#include "apk_builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char* prog) {
    printf(
        "Casprix APK Builder v1.1.0\n"
        "Usage: %s [options] --input <file.cpx> --package <id> --name <Title>\n"
        "\n"
        "Required:\n"
        "  --input <path>      Source Casprix (.cpx) file\n"
        "  --package <id>      Android package (e.g. com.casprix.app)\n"
        "  --name <title>      App display name\n"
        "\n"
        "Options:\n"
        "  --output <path>     Output APK (default: app-release.apk)\n"
        "  --abi <name>        Target ABI (arm64-v8a, x86_64, etc.)\n"
        "  --release           Production build (no debug symbols)\n"
        "  --help              Show this help message\n",
        prog);
}

static ApkAbi parse_abi(const char* name) {
    if (strcmp(name, "arm64-v8a")   == 0) return APK_ABI_ARM64_V8A;
    if (strcmp(name, "armeabi-v7a") == 0) return APK_ABI_ARMEABI_V7A;
    if (strcmp(name, "x86_64")      == 0) return APK_ABI_X86_64;
    if (strcmp(name, "x86")         == 0) return APK_ABI_X86;
    fprintf(stderr, "apk-builder: unknown ABI '%s', defaulting to arm64-v8a\n", name);
    return APK_ABI_ARM64_V8A;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    ApkBuildConfig cfg;
    apk_config_init(&cfg);

    int abi_set = 0;  /* Track if user explicitly set ABIs */

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }

#define NEXT_ARG(dest, field) \
        else if (strcmp(arg, dest) == 0) { \
            if (i + 1 >= argc) { \
                fprintf(stderr, "apk-builder: %s requires a value\n", dest); return 1; \
            } \
            snprintf(cfg.field, sizeof(cfg.field), "%s", argv[++i]); \
        }

        NEXT_ARG("--input",        input_file)
        NEXT_ARG("--package",      package_name)
        NEXT_ARG("--name",         app_name)
        NEXT_ARG("--output",       output_apk)
        NEXT_ARG("--activity",     main_activity)
        NEXT_ARG("--version-name", version_name)
        NEXT_ARG("--ndk",          android_ndk)
        NEXT_ARG("--sdk",          android_sdk)
        NEXT_ARG("--keystore",     keystore_path)
        NEXT_ARG("--ks-pass",      keystore_pass)
        NEXT_ARG("--key-alias",    key_alias)
        NEXT_ARG("--key-pass",     key_pass)
        NEXT_ARG("--icon",         icon_path)
        NEXT_ARG("--assets",       assets_dir)
        NEXT_ARG("--fonts",        font_dir)

        else if (strcmp(arg, "--version-code") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "apk-builder: --version-code requires a value\n"); return 1; }
            cfg.version_code = atoi(argv[++i]);
        }
        else if (strcmp(arg, "--min-sdk") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "apk-builder: --min-sdk requires a value\n"); return 1; }
            cfg.min_sdk = atoi(argv[++i]);
        }
        else if (strcmp(arg, "--target-sdk") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "apk-builder: --target-sdk requires a value\n"); return 1; }
            cfg.target_sdk = atoi(argv[++i]);
        }
        else if (strcmp(arg, "--abi") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "apk-builder: --abi requires a value\n"); return 1; }
            if (!abi_set) { cfg.abi_count = 0; abi_set = 1; }
            if (cfg.abi_count < 4)
                cfg.abis[cfg.abi_count++] = parse_abi(argv[++i]);
        }
        else if (strcmp(arg, "--release") == 0) {
            cfg.debug_build = 0;
        }
        else if (strcmp(arg, "--a11y-shim") == 0) {
            cfg.accessibility_shim = 1;    /* force on */
        }
        else if (strcmp(arg, "--no-a11y-shim") == 0) {
            cfg.accessibility_shim = 0;    /* force off */
        }
        else if (strcmp(arg, "--quiet") == 0) {
            cfg.verbose = 0;
        }
        else if (strcmp(arg, "--keep-build") == 0) {
            cfg.keep_intermediates = 1;
        }
        else {
            fprintf(stderr, "apk-builder: unknown option '%s'\n", arg);
            fprintf(stderr, "Run '%s --help' for usage.\n", argv[0]);
            return 1;
        }
    }

    /* Validate required fields */
    if (!cfg.input_file[0]) {
        fprintf(stderr, "apk-builder: --input is required\n"); return 1;
    }
    if (!cfg.package_name[0]) {
        fprintf(stderr, "apk-builder: --package is required\n"); return 1;
    }
    if (!cfg.app_name[0]) {
        fprintf(stderr, "apk-builder: --name is required\n"); return 1;
    }

    apk_config_print(&cfg);

    ApkBuildResult result = apk_build(&cfg);

    if (result.code != APK_OK) {
        fprintf(stderr, "\napk-builder: Build failed: %s\n", result.message);
        return 1;
    }

    printf("\n✓ APK created: %s\n", result.output_path);
    printf("  Build time : %lldms\n", result.build_time_ms);
    printf("\nInstall on device:\n");
    printf("  adb install %s\n\n", result.output_path);
    return 0;
}
