/*
 * apk_builder.c ??? Main APK build pipeline implementation
 *
 * Stages:
 *   1. Validate config
 *   2. Compile .cpx ??? ARM64 .so  (via Casperix compiler cross-compilation)
 *   3. Generate AndroidManifest.xml
 *   4. Pack resources (strings.xml, icon)
 *   5. Package everything into unsigned APK (ZIP)
 *   6. Sign with debug or user keystore
 *   7. zipalign output
 */

#include "apk_builder.h"
#include "manifest.h"
#include "zip_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define PATH_SEP "\\"
#  define MKDIR(p)          _mkdir(p)
#  define SNPRINTF          _snprintf
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  define PATH_SEP "/"
#  define MKDIR(p)          mkdir(p, 0755)
#  define SNPRINTF          snprintf
#endif

/* ========================================================================
 * Internal helpers
 * ======================================================================== */

static void log_step(const ApkBuildConfig* cfg, const char* msg) {
    if (cfg->verbose) printf("[apk-builder] %s\n", msg);
}

static int run_cmd(const ApkBuildConfig* cfg, const char* cmd) {
    if (cfg->verbose) printf("[apk-builder] CMD: %s\n", cmd);
    int r = system(cmd);
    if (r != 0) fprintf(stderr, "[apk-builder] FAILED (exit %d): %s\n", r, cmd);
    return r;
}

static int file_exists(const char* path) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    return access(path, F_OK) == 0;
#endif
}

static int dir_exists(const char* path) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static void make_absolute_path(const char* input, char* output, size_t output_size) {
    if (!output || output_size == 0) return;
    if (!input || !input[0]) {
        output[0] = '\0';
        return;
    }
#ifdef _WIN32
    {
        DWORD len = GetFullPathNameA(input, (DWORD)output_size, output, NULL);
        if (len == 0 || len >= output_size) {
            SNPRINTF(output, output_size, "%s", input);
        }
    }
#else
    if (input[0] == '/') {
        SNPRINTF(output, output_size, "%s", input);
        return;
    }
    {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd))) {
            SNPRINTF(output, output_size, "%s/%s", cwd, input);
        } else {
            SNPRINTF(output, output_size, "%s", input);
        }
    }
#endif
}

static void normalize_cmake_path(char* path) {
    if (!path) return;
    for (; *path; ++path) {
        if (*path == '\\') *path = '/';
    }
}

static int patch_generated_c_source(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0L, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size < 0) { fclose(f); return -1; }

    char* data = (char*)malloc((size_t)size + 1);
    if (!data) { fclose(f); return -1; }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return -1;
    }
    data[size] = '\0';
    fclose(f);

    for (char* p = data; (p = strstr(p, "_New(")) != NULL; ++p) {
        p[1] = 'n';
    }

    f = fopen(path, "wb");
    if (!f) { free(data); return -1; }
    if (fwrite(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);
    free(data);
    return 0;
}

static void mkdir_p(const char* path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p; *p = '\0';
            MKDIR(tmp);
            *p = c;
        }
    }
    MKDIR(tmp);
}

static int resolve_latest_sdk_build_tools(const ApkBuildConfig* cfg, char* output, size_t output_size) {
    if (!cfg->android_sdk[0]) return -1;

#ifdef _WIN32
    {
        char bt_base[1024];
        static char bt_found[512];
        char dir_cmd[1024];

        bt_found[0] = '\0';
        snprintf(bt_base, sizeof(bt_base), "%s\\build-tools", cfg->android_sdk);
        snprintf(dir_cmd, sizeof(dir_cmd),
                 "cmd /C \"FOR /D %%G IN (\\\"%s\\\\*\\\") DO @SET \\\"_BT=%%G\\\" & ECHO %%_BT%%\"",
                 bt_base);

        FILE* pipe = _popen(dir_cmd, "r");
        if (!pipe) return -1;

        while (fgets(bt_found, sizeof(bt_found), pipe)) {}
        _pclose(pipe);

        {
            size_t len = strlen(bt_found);
            while (len > 0 && (bt_found[len - 1] == '\n' ||
                               bt_found[len - 1] == '\r' ||
                               bt_found[len - 1] == ' ')) {
                bt_found[--len] = '\0';
            }
            if (len == 0) return -1;
        }

        snprintf(output, output_size, "%s", bt_found);
        return 0;
    }
#else
    (void)output;
    (void)output_size;
    return -1;
#endif
}

static int resolve_latest_sdk_build_tool(const ApkBuildConfig* cfg,
                                         const char* tool_name,
                                         char* output,
                                         size_t output_size) {
    if (!cfg->android_sdk[0] || !tool_name || !tool_name[0]) return -1;

#ifdef _WIN32
    {
        char bt_base[1024];
        char dir_cmd[1024];
        char line[512];

        snprintf(bt_base, sizeof(bt_base), "%s\\build-tools", cfg->android_sdk);
        snprintf(dir_cmd, sizeof(dir_cmd),
                 "cmd /C \"dir /b /ad /o-n \\\"%s\\\"\"",
                 bt_base);

        FILE* pipe = _popen(dir_cmd, "r");
        if (!pipe) return -1;

        while (fgets(line, sizeof(line), pipe)) {
            char candidate_dir[1024];
            char candidate_tool[1024];
            size_t len = strlen(line);

            while (len > 0 && (line[len - 1] == '\n' ||
                               line[len - 1] == '\r' ||
                               line[len - 1] == ' ')) {
                line[--len] = '\0';
            }
            if (len == 0) continue;

            snprintf(candidate_dir, sizeof(candidate_dir), "%s\\%s", bt_base, line);
            snprintf(candidate_tool, sizeof(candidate_tool), "%s\\%s",
                     candidate_dir, tool_name);
            if (file_exists(candidate_tool)) {
                snprintf(output, output_size, "%s", candidate_tool);
                _pclose(pipe);
                return 0;
            }
        }

        _pclose(pipe);
    }
#else
    (void)cfg;
    (void)tool_name;
    (void)output;
    (void)output_size;
#endif

    return -1;
}

static int resolve_android_jar(const ApkBuildConfig* cfg, char* output, size_t output_size) {
    if (!cfg->android_sdk[0]) return -1;

    {
        char preferred[1024];
        snprintf(preferred, sizeof(preferred), "%s\\platforms\\android-%d\\android.jar",
                 cfg->android_sdk, cfg->target_sdk);
        if (file_exists(preferred)) {
            snprintf(output, output_size, "%s", preferred);
            return 0;
        }
    }

#ifdef _WIN32
    {
        char platforms_base[1024];
        static char platform_found[512];
        char dir_cmd[1024];

        platform_found[0] = '\0';
        snprintf(platforms_base, sizeof(platforms_base), "%s\\platforms", cfg->android_sdk);
        snprintf(dir_cmd, sizeof(dir_cmd),
                 "cmd /C \"FOR /D %%G IN (\\\"%s\\\\android-*\\\") DO @SET \\\"_PL=%%G\\\" & ECHO %%_PL%%\"",
                 platforms_base);

        FILE* pipe = _popen(dir_cmd, "r");
        if (!pipe) return -1;

        while (fgets(platform_found, sizeof(platform_found), pipe)) {}
        _pclose(pipe);

        {
            size_t len = strlen(platform_found);
            while (len > 0 && (platform_found[len - 1] == '\n' ||
                               platform_found[len - 1] == '\r' ||
                               platform_found[len - 1] == ' ')) {
                platform_found[--len] = '\0';
            }
            if (len == 0) return -1;
        }

        snprintf(output, output_size, "%s\\android.jar", platform_found);
        return file_exists(output) ? 0 : -1;
    }
#else
    (void)output;
    (void)output_size;
    return -1;
#endif
}

static int jar_update_path(const ApkBuildConfig* cfg, const char* apk_path,
                           const char* base_dir, const char* rel_path) {
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "jar uf \"%s\" -C \"%s\" \"%s\"",
             apk_path, base_dir, rel_path);
    return run_cmd(cfg, cmd);
}

static int copy_file_binary(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) return -1;

    FILE* out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }

    {
        char buffer[65536];
        size_t read_count;
        while ((read_count = fread(buffer, 1, sizeof(buffer), in)) > 0) {
            if (fwrite(buffer, 1, read_count, out) != read_count) {
                fclose(in);
                fclose(out);
                return -1;
            }
        }
    }

    fclose(in);
    fclose(out);
    return 0;
}

/* Resolve NDK/SDK from env vars or well-known Windows locations if config fields are empty */
static void resolve_paths(ApkBuildConfig* cfg_copy) {
    /* ?????? SDK ?????? */
    if (!cfg_copy->android_sdk[0]) {
        /* 1. Standard env vars */
        const char* env = getenv("ANDROID_HOME");
        if (!env) env = getenv("ANDROID_SDK_ROOT");
        /* 2. Well-known Windows default install path */
        if (!env) {
            const char* localapp = getenv("LOCALAPPDATA");
            if (!localapp) localapp = "C:\\Users\\User\\AppData\\Local";
            static char win_sdk[512];
            snprintf(win_sdk, sizeof(win_sdk), "%s\\Android\\Sdk", localapp);
            /* Verify it exists by checking for platform-tools/adb.exe */
            char adb_test[600];
            snprintf(adb_test, sizeof(adb_test),
                     "%s\\platform-tools\\adb.exe", win_sdk);
            FILE* f = fopen(adb_test, "rb");
            if (f) { fclose(f); env = win_sdk; }
        }
        if (env)
            snprintf(cfg_copy->android_sdk, sizeof(cfg_copy->android_sdk), "%s", env);
    }

    /* ?????? NDK ?????? */
    if (!cfg_copy->android_ndk[0]) {
        const char* env = getenv("ANDROID_NDK_HOME");
        if (!env) env = getenv("ANDROID_NDK");
        if (!env) env = getenv("NDK_HOME");

        /* Try to find newest NDK inside the SDK we just resolved */
        if (!env && cfg_copy->android_sdk[0]) {
            /* SDK/ndk/<version> ??? grab highest version directory */
            char ndk_base[600];
            snprintf(ndk_base, sizeof(ndk_base), "%s\\ndk", cfg_copy->android_sdk);

            /* Use a glob-style search via dir command on Windows */
            char cmd[800];
            snprintf(cmd, sizeof(cmd),
                     "for /d %%v in (\"%s\\*\") do @echo %%v", ndk_base);

            /* Try a known common NDK version subfolder naming */
            /* We read the first line from dir output ??? best effort */
#ifdef _WIN32
            static char ndk_found[512];
            char dir_cmd[800];
            snprintf(dir_cmd, sizeof(dir_cmd),
                     "FOR /D %%G IN (\"%s\\*\") DO @SET \"_NDK_LATEST=%%G\"&& ECHO %%G",
                     ndk_base);
            FILE* pipe = _popen(dir_cmd, "r");
            if (pipe) {
                if (fgets(ndk_found, sizeof(ndk_found), pipe)) {
                    /* Strip trailing \n */
                    size_t len = strlen(ndk_found);
                    while (len > 0 && (ndk_found[len-1] == '\n' ||
                                       ndk_found[len-1] == '\r' ||
                                       ndk_found[len-1] == ' '))
                        ndk_found[--len] = '\0';
                    env = ndk_found;
                }
                _pclose(pipe);
            }
#endif
        }
        if (env)
            snprintf(cfg_copy->android_ndk, sizeof(cfg_copy->android_ndk), "%s", env);
    }

    if (cfg_copy->android_sdk[0])
        printf("[apk-builder]   SDK: %s\n", cfg_copy->android_sdk);
    if (cfg_copy->android_ndk[0])
        printf("[apk-builder]   NDK: %s\n", cfg_copy->android_ndk);
}

/* ========================================================================
 * apk_config_init ??? Sensible defaults
 * ======================================================================== */


static int ensure_android_skia(const ApkBuildConfig* cfg, const char* build_dir, const char* ndk) {
#ifdef _WIN32
    const char* skia_lib = "third_party\\skia\\lib-android-arm64\\libskia.a";
#else
    const char* skia_lib = "third_party/skia/lib-android-arm64/libskia.a";
#endif

    if (file_exists(skia_lib)) return 0;

#ifdef _WIN32
    {
        char log_path[1024];
        char cmd[4096];

        snprintf(log_path, sizeof(log_path), "%s/skia-build.log", build_dir);
        printf("[apk-builder]   Skia ARM64 lib missing. Building from source...\n");
        snprintf(cmd, sizeof(cmd),
                 "cmd /C \"\"tools\\android\\build_skia_arm64.bat\" -NdkRoot \"%s\" > \"%s\" 2>&1\"",
                 ndk,
                 log_path);

        if (run_cmd(cfg, cmd) != 0) {
            fprintf(stderr, "[apk-builder] Skia ARM64 build log: %s\n", log_path);
            return -1;
        }
    }
#else
    (void)cfg;
    (void)build_dir;
    (void)ndk;
    fprintf(stderr, "[apk-builder] Missing Android Skia library: %s\n", skia_lib);
    return -1;
#endif

    if (!file_exists(skia_lib)) {
        fprintf(stderr, "[apk-builder] Skia ARM64 library missing after build: %s\n", skia_lib);
        return -1;
    }

    return 0;
}
void apk_config_init(ApkBuildConfig* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->main_activity, sizeof(cfg->main_activity), "MainActivity");
    snprintf(cfg->version_name,  sizeof(cfg->version_name),  "1.0.0");
    snprintf(cfg->app_label,     sizeof(cfg->app_label),     "");
    cfg->version_code = 1;
    cfg->min_sdk      = 24;
    cfg->target_sdk   = 34;
    cfg->abis[0]      = APK_ABI_ARM64_V8A;
    cfg->abi_count    = 1;
    cfg->debug_build  = 1;
    cfg->verbose      = 1;
}

/* ========================================================================
 * Stage 1: Compile .cpx -> generated C -> Android shared library
 * ======================================================================== */

ApkError apk_stage_compile(const ApkBuildConfig* cfg, const char* build_dir) {
    static const char* abi_names[] = {
        "arm64-v8a", "armeabi-v7a", "x86_64", "x86"
    };

    const char* compiler = "casprix";
    const char* ndk = cfg->android_ndk;
    const char* act = cfg->main_activity[0] ? cfg->main_activity : "MainActivity";
    int abi_count = cfg->abi_count > 0 ? cfg->abi_count : 1;

    if (!ndk[0]) {
        fprintf(stderr, "[apk-builder] Android NDK path is required for Android builds.\n");
        return APK_ERR_NDK_NOT_FOUND;
    }

    if (file_exists("build\\casprix.exe")) {
        compiler = "build\\casprix.exe";
    } else if (file_exists("build\\Release\\casprix.exe")) {
        compiler = "build\\Release\\casprix.exe";
    }

    {
        char generated_base[1024];
        char generated_c[1024];
        char compiler_log[1024];
        char cmd[4096];

        snprintf(generated_base, sizeof(generated_base), "%s/cpx_app", build_dir);
        snprintf(generated_c, sizeof(generated_c), "%s.c", generated_base);
        snprintf(compiler_log, sizeof(compiler_log), "%s/casprix-emit-c.log", build_dir);

#ifdef _WIN32
        snprintf(cmd, sizeof(cmd),
            "cmd /C \"\"%s\" \"%s\" --mir --emit-c --output \"%s\" > \"%s\" 2>&1\"",
            compiler,
            cfg->input_file,
            generated_base,
            compiler_log);
#else
        snprintf(cmd, sizeof(cmd),
            "%s \"%s\" --mir --emit-c --output \"%s\" > \"%s\" 2>&1",
            compiler,
            cfg->input_file,
            generated_base,
            compiler_log);
#endif

        if (run_cmd(cfg, cmd) != 0) {
            fprintf(stderr, "[apk-builder] Casprix emit-c log: %s\n", compiler_log);
            return APK_ERR_COMPILE_FAILED;
        }

        if (!file_exists(generated_c)) {
            fprintf(stderr, "[apk-builder] Generated C file missing: %s\n", generated_c);
            return APK_ERR_COMPILE_FAILED;
        }

        if (patch_generated_c_source(generated_c) != 0) {
            fprintf(stderr, "[apk-builder] Failed to patch generated C source: %s\n", generated_c);
            return APK_ERR_COMPILE_FAILED;
        }

        printf("[apk-builder]   Generated C -> %s\n", generated_c);
    }

    if (ensure_android_skia(cfg, build_dir, ndk) != 0) {
        return APK_ERR_COMPILE_FAILED;
    }

    for (int i = 0; i < abi_count; i++) {
        int abi_idx = (int)cfg->abis[i];
        char lib_dir[1024];
        char lib_dir_abs[1024];
        char cmake_build_dir[1024];
        char generated_c[1024];
        char generated_c_abs[1024];
        char so_path[1024];
        char skia_dir_abs[1024];
        char toolchain_file[1024];
        char configure_log[1024];
        char build_log[1024];
        char cmd[4096];

        if (abi_idx < 0 || abi_idx > 3) abi_idx = 0;

        snprintf(lib_dir, sizeof(lib_dir), "%s/lib/%s", build_dir, abi_names[abi_idx]);
        snprintf(cmake_build_dir, sizeof(cmake_build_dir), "%s/cmake-android-%s", build_dir, abi_names[abi_idx]);
        snprintf(generated_c, sizeof(generated_c), "%s/cpx_app.c", build_dir);
        snprintf(configure_log, sizeof(configure_log), "%s/cmake-configure-%s.log", build_dir, abi_names[abi_idx]);
        snprintf(build_log, sizeof(build_log), "%s/cmake-build-%s.log", build_dir, abi_names[abi_idx]);

        mkdir_p(lib_dir);
        mkdir_p(cmake_build_dir);
        make_absolute_path(lib_dir, lib_dir_abs, sizeof(lib_dir_abs));
        make_absolute_path(generated_c, generated_c_abs, sizeof(generated_c_abs));
        make_absolute_path("third_party/skia-src", skia_dir_abs, sizeof(skia_dir_abs));
        snprintf(toolchain_file, sizeof(toolchain_file), "%s/build/cmake/android.toolchain.cmake", ndk);
        normalize_cmake_path(lib_dir_abs);
        normalize_cmake_path(generated_c_abs);
        normalize_cmake_path(skia_dir_abs);
        normalize_cmake_path(toolchain_file);
        snprintf(so_path, sizeof(so_path), "%s/lib%s.so", lib_dir_abs, act);

        if (file_exists(so_path)) {
            printf("[apk-builder]   Reusing existing ABI %s -> %s\n", abi_names[abi_idx], so_path);
            continue;
        }

        snprintf(cmd, sizeof(cmd),
            "cmake -G Ninja -S \"runtime/android\" -B \"%s\" "
            "-DCPX_APP_SOURCE=\"%s\" "
            "-DCPX_OUTPUT_DIR=\"%s\" "
            "-DCPX_ACTIVITY_NAME=\"%s\" "
            "-DCMAKE_BUILD_TYPE=Release "
            "-DCMAKE_TOOLCHAIN_FILE=\"%s\" "
            "-DANDROID_ABI=\"%s\" "
            "-DANDROID_PLATFORM=android-%d "
            "-DSKIA_DIR=\"%s\" "
            "-DCMAKE_C_COMPILER_WORKS=TRUE "
            "-DCMAKE_CXX_COMPILER_WORKS=TRUE "
            "-DCMAKE_C_COMPILER_FORCED=TRUE "
            "-DCMAKE_CXX_COMPILER_FORCED=TRUE "
            "> \"%s\" 2>&1",
            cmake_build_dir,
            generated_c_abs,
            lib_dir_abs,
            act,
            toolchain_file,
            abi_names[abi_idx],
            cfg->min_sdk,
            skia_dir_abs,
            configure_log);

        if (run_cmd(cfg, cmd) != 0) {
            fprintf(stderr, "[apk-builder] Android CMake configure log: %s\n", configure_log);
            return APK_ERR_COMPILE_FAILED;
        }

        snprintf(cmd, sizeof(cmd),
            "cmake --build \"%s\" --config Release > \"%s\" 2>&1",
            cmake_build_dir,
            build_log);

        if (run_cmd(cfg, cmd) != 0) {
            fprintf(stderr, "[apk-builder] Android CMake build log: %s\n", build_log);
            return APK_ERR_COMPILE_FAILED;
        }

        if (!file_exists(so_path)) {
            fprintf(stderr, "[apk-builder] Shared library missing: %s\n", so_path);
            return APK_ERR_COMPILE_FAILED;
        }

        printf("[apk-builder]   Built ABI %s -> %s\n", abi_names[abi_idx], so_path);
    }

    return APK_OK;
}

/* ========================================================================
 * Stage 2: Generate AndroidManifest.xml
 * ======================================================================== */

ApkError apk_stage_manifest(const ApkBuildConfig* cfg, const char* build_dir) {
    char path[2048];
    snprintf(path, sizeof(path), "%s/AndroidManifest.xml", build_dir);
    int r = manifest_write(cfg, path);
    if (r != 0) return APK_ERR_MANIFEST_FAILED;
    printf("[apk-builder]   Manifest ??? %s\n", path);
    return APK_OK;
}

/* ========================================================================
 * Stage 3: Pack resources
 * ======================================================================== */

ApkError apk_stage_resources(const ApkBuildConfig* cfg, const char* build_dir) {
    char res_dir [1024]; snprintf(res_dir,  sizeof(res_dir),  "%s/res",             build_dir);
    char draw_dir[1024]; snprintf(draw_dir, sizeof(draw_dir), "%s/res/drawable",     build_dir);
    mkdir_p(res_dir);

    /* strings.xml */
    int r = manifest_write_strings(cfg, res_dir);
    if (r != 0) return APK_ERR_RESOURCE_FAILED;

    /* Icon */
    if (cfg->icon_path[0]) {
        mkdir_p(draw_dir);
        char cmd[2048];
#ifdef _WIN32
        snprintf(cmd, sizeof(cmd), "copy /Y \"%s\" \"%s\\ic_launcher.png\"",
                 cfg->icon_path, draw_dir);
#else
        snprintf(cmd, sizeof(cmd), "cp \"%s\" \"%s/ic_launcher.png\"",
                 cfg->icon_path, draw_dir);
#endif
        run_cmd(cfg, cmd);
    } else {
        mkdir_p(draw_dir);
        manifest_write_default_icon(draw_dir);
    }

    /* Copy user assets if provided */
    if (cfg->assets_dir[0]) {
        char assets_out[1024];
        snprintf(assets_out, sizeof(assets_out), "%s/assets", build_dir);
        mkdir_p(assets_out);
        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
#ifdef _WIN32
                 "xcopy /E /I /Y \"%s\" \"%s\"",
#else
                 "cp -r \"%s/\"* \"%s/\"",
#endif
                 cfg->assets_dir, assets_out);
        run_cmd(cfg, cmd);
    }

    /* Copy user fonts if provided */
    if (cfg->font_dir[0]) {
        char fonts_out[1024];
        snprintf(fonts_out, sizeof(fonts_out), "%s/assets/fonts", build_dir);
        mkdir_p(fonts_out);
        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
#ifdef _WIN32
                 "xcopy /E /I /Y \"%s\" \"%s\"",
#else
                 "cp -r \"%s/\"* \"%s/\"",
#endif
                 cfg->font_dir, fonts_out);
        run_cmd(cfg, cmd);
    }

    printf("[apk-builder]   Resources packed ??? %s\n", res_dir);
    return APK_OK;
}

/* ========================================================================
 * Stage 4: Package into unsigned APK (ZIP)
 *
 * APK structure:
 *   AndroidManifest.xml
 *   lib/arm64-v8a/lib<Activity>.so
 *   res/values/strings.xml
 *   res/drawable/ic_launcher.xml  (or .png)
 *   assets/**   (optional)
 * ======================================================================== */

ApkError apk_stage_package(const ApkBuildConfig* cfg, const char* build_dir) {
    static const char* abi_names[] = {
        "arm64-v8a", "armeabi-v7a", "x86_64", "x86"
    };

    char unsigned_apk[2048];
    char manifest_path[1024];
    char res_dir[1024];
    char android_jar[1024];
    char compiled_res[1024];
    char aapt2_path[1024];
    char aapt_path[1024];
    char cmd[4096];

    snprintf(unsigned_apk, sizeof(unsigned_apk), "%s/app-unsigned.apk", build_dir);
    snprintf(manifest_path, sizeof(manifest_path), "%s/AndroidManifest.xml", build_dir);
    snprintf(res_dir, sizeof(res_dir), "%s/res", build_dir);
    snprintf(compiled_res, sizeof(compiled_res), "%s/compiled_res.zip", build_dir);

    if (resolve_latest_sdk_build_tool(cfg, "aapt2.exe", aapt2_path, sizeof(aapt2_path)) == 0 &&
        resolve_android_jar(cfg, android_jar, sizeof(android_jar)) == 0) {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" compile --dir \"%s\" -o \"%s\"",
                 aapt2_path, res_dir, compiled_res);
        if (run_cmd(cfg, cmd) == 0) {
            snprintf(cmd, sizeof(cmd),
                     "\"%s\" link --manifest \"%s\" -I \"%s\""
                     " --auto-add-overlay -o \"%s\" \"%s\"",
                     aapt2_path, manifest_path, android_jar, unsigned_apk, compiled_res);
            if (run_cmd(cfg, cmd) == 0) {
                const char* act = cfg->main_activity[0] ? cfg->main_activity : "MainActivity";
                int abi_count = cfg->abi_count > 0 ? cfg->abi_count : 1;
                for (int i = 0; i < abi_count; i++) {
                    int abi_idx = (int)cfg->abis[i];
                    char so_rel_path[1024];
                    if (abi_idx < 0 || abi_idx > 3) abi_idx = 0;
                    snprintf(so_rel_path, sizeof(so_rel_path),
                             "lib/%s/lib%s.so", abi_names[abi_idx], act);
                    if (jar_update_path(cfg, unsigned_apk, build_dir, so_rel_path) != 0) {
                        return APK_ERR_ZIP_FAILED;
                    }
                }

                {
                    char assets_dir[1024];
                    snprintf(assets_dir, sizeof(assets_dir), "%s/assets", build_dir);
                    if (dir_exists(assets_dir) &&
                        jar_update_path(cfg, unsigned_apk, build_dir, "assets") != 0) {
                        return APK_ERR_ZIP_FAILED;
                    }
                }

                printf("[apk-builder]   Packaged -> %s\n", unsigned_apk);
                return APK_OK;
            }
        }
    }

    if (resolve_latest_sdk_build_tool(cfg, "aapt.exe", aapt_path, sizeof(aapt_path)) == 0 &&
        resolve_android_jar(cfg, android_jar, sizeof(android_jar)) == 0) {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" package -f -M \"%s\" -S \"%s\" -I \"%s\" -F \"%s\"",
                 aapt_path, manifest_path, res_dir, android_jar, unsigned_apk);
        if (run_cmd(cfg, cmd) != 0) return APK_ERR_ZIP_FAILED;

        {
            const char* act = cfg->main_activity[0] ? cfg->main_activity : "MainActivity";
            int abi_count = cfg->abi_count > 0 ? cfg->abi_count : 1;
            for (int i = 0; i < abi_count; i++) {
                int abi_idx = (int)cfg->abis[i];
                char so_rel_path[1024];
                if (abi_idx < 0 || abi_idx > 3) abi_idx = 0;
                snprintf(so_rel_path, sizeof(so_rel_path),
                         "lib/%s/lib%s.so", abi_names[abi_idx], act);
                if (jar_update_path(cfg, unsigned_apk, build_dir, so_rel_path) != 0) {
                    return APK_ERR_ZIP_FAILED;
                }
            }
        }

        {
            char assets_dir[1024];
            snprintf(assets_dir, sizeof(assets_dir), "%s/assets", build_dir);
            if (dir_exists(assets_dir) &&
                jar_update_path(cfg, unsigned_apk, build_dir, "assets") != 0) {
                return APK_ERR_ZIP_FAILED;
            }
        }

        printf("[apk-builder]   Packaged -> %s\n", unsigned_apk);
        return APK_OK;
    }

    {
        ZipWriter* zw = zip_writer_open(unsigned_apk);
        if (!zw) return APK_ERR_ZIP_FAILED;

        zip_writer_add_file(zw, "AndroidManifest.xml", manifest_path, 1);

        {
            const char* act = cfg->main_activity[0] ? cfg->main_activity : "MainActivity";
            int abi_count = cfg->abi_count > 0 ? cfg->abi_count : 1;
            for (int i = 0; i < abi_count; i++) {
                int abi_idx = (int)cfg->abis[i];
                char so_path[1024];
                char so_archive[1024];
                if (abi_idx < 0 || abi_idx > 3) abi_idx = 0;
                snprintf(so_path, sizeof(so_path),
                         "%s/lib/%s/lib%s.so", build_dir, abi_names[abi_idx], act);
                snprintf(so_archive, sizeof(so_archive),
                         "lib/%s/lib%s.so", abi_names[abi_idx], act);
                zip_writer_add_file(zw, so_archive, so_path, 0);
            }
        }

        {
            char str_path[1024];
            snprintf(str_path, sizeof(str_path), "%s/res/values/strings.xml", build_dir);
            zip_writer_add_file(zw, "res/values/strings.xml", str_path, 1);
        }

        {
            char draw_dir[1024];
            char icon_png[1024];
            char icon_xml[2048];
            FILE* test;

            snprintf(draw_dir, sizeof(draw_dir), "%s/res/drawable", build_dir);
            snprintf(icon_png, sizeof(icon_png), "%s/ic_launcher.png", draw_dir);
            snprintf(icon_xml, sizeof(icon_xml), "%s/ic_launcher.xml", draw_dir);

            test = fopen(icon_png, "rb");
            if (test) {
                fclose(test);
                zip_writer_add_file(zw, "res/drawable/ic_launcher.png", icon_png, 0);
            } else {
                test = fopen(icon_xml, "rb");
                if (test) {
                    fclose(test);
                    zip_writer_add_file(zw, "res/drawable/ic_launcher.xml", icon_xml, 1);
                }
            }
        }

        zip_writer_close(zw);
    }

    printf("[apk-builder]   Packaged -> %s\n", unsigned_apk);
    return APK_OK;
}

/* ========================================================================
 * Stage 5: Sign the APK
 * ======================================================================== */

ApkError apk_generate_debug_keystore(const char* keystore_path) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "keytool -genkeypair -v -keystore \"%s\""
        " -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000"
        " -storepass android -keypass android"
        " -dname \"CN=Android Debug,O=Android,C=US\" 2>&1",
        keystore_path);
    int r = system(cmd);
    return r == 0 ? APK_OK : APK_ERR_SIGN_FAILED;
}

ApkError apk_stage_sign(const ApkBuildConfig* cfg, const char* build_dir,
                         const char* unsigned_apk, const char* signed_apk) {
    const char* ks   = cfg->keystore_path[0] ? cfg->keystore_path : NULL;
    const char* ksp  = cfg->keystore_pass[0] ? cfg->keystore_pass : "android";
    const char* kp   = cfg->key_pass    [0] ? cfg->key_pass     : "android";
    const char* ka   = cfg->key_alias   [0] ? cfg->key_alias    : "androiddebugkey";

    /* Create debug keystore if none provided */
    char debug_ks[1024];
    if (!ks) {
        const char* user_profile = getenv("USERPROFILE");
        if (user_profile && user_profile[0]) {
            snprintf(debug_ks, sizeof(debug_ks), "%s\\.android\\debug.keystore", user_profile);
        } else {
            debug_ks[0] = '\0';
        }

        if (!debug_ks[0] || !file_exists(debug_ks)) {
            snprintf(debug_ks, sizeof(debug_ks), "%s/debug.keystore", build_dir);
            if (!file_exists(debug_ks)) {
                printf("[apk-builder]   Generating debug keystore...\n");
                if (apk_generate_debug_keystore(debug_ks) != APK_OK) {
                    return APK_ERR_SIGN_FAILED;
                }
            }
        }

        ks = debug_ks;
    }

    char cmd[4096];

    /*
     * Find apksigner in SDK build-tools.
     * Layout: <SDK>/build-tools/<version>/apksigner[.bat]
     * We search for the highest version via a dir listing on Windows.
     */
    char apksigner_path[1024] = "apksigner";   /* default: rely on PATH */
    resolve_latest_sdk_build_tool(cfg, "apksigner.bat",
                                  apksigner_path, sizeof(apksigner_path));

    /* Prefer apksigner (SDK Build Tools) over jarsigner */
    snprintf(cmd, sizeof(cmd),
        "\"%s\" sign --ks \"%s\" --ks-pass pass:%s --key-pass pass:%s"
        " --ks-key-alias %s --min-sdk-version %d --out \"%s\" \"%s\" 2>&1",
        apksigner_path, ks, ksp, kp, ka, cfg->min_sdk, signed_apk, unsigned_apk);

    int r = run_cmd(cfg, cmd);
    if (r != 0) {
        /* Fallback: jarsigner */
        printf("[apk-builder]   apksigner failed, trying jarsigner...\n");
        snprintf(cmd, sizeof(cmd),
            "jarsigner -verbose -sigalg SHA1withRSA -digestalg SHA1"
            " -keystore \"%s\" -storepass %s -keypass %s"
            " -signedjar \"%s\" \"%s\" %s 2>&1",
            ks, ksp, kp, signed_apk, unsigned_apk, ka);
        r = run_cmd(cfg, cmd);
        if (r != 0) return APK_ERR_SIGN_FAILED;
    }

    printf("[apk-builder]   Signed    ??? %s\n", signed_apk);
    return APK_OK;
}

/* ========================================================================
 * Stage 6: zipalign
 * ======================================================================== */

ApkError apk_stage_align(const ApkBuildConfig* cfg, const char* signed_apk,
                          const char* output_apk) {
    char cmd[2048];

    /* Find zipalign in SDK build-tools */
    char zipalign[1024] = "zipalign";
    resolve_latest_sdk_build_tool(cfg, "zipalign.exe", zipalign, sizeof(zipalign));

    /* -p 4 = page alignment for .so, 4-byte alignment for everything else */
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -v -p 4 \"%s\" \"%s\" 2>&1",
             zipalign, signed_apk, output_apk);
    int r = run_cmd(cfg, cmd);
    if (r != 0) {
        printf("[apk-builder]   zipalign failed; copying unsigned APK as output.\n");
        if (copy_file_binary(signed_apk, output_apk) != 0) {
            return APK_ERR_ALIGN_FAILED;
        }
    } else {
        printf("[apk-builder]   Aligned   ??? %s\n", output_apk);
    }
    return APK_OK;
}

/* ========================================================================
 * Main build entry point
 * ======================================================================== */

ApkBuildResult apk_build(const ApkBuildConfig* config) {
    ApkBuildResult result;
    memset(&result, 0, sizeof(result));

    clock_t t_start = clock();

    /* Validate */
    if (!config->input_file[0] || !config->package_name[0] || !config->app_name[0]) {
        result.code = APK_ERR_INVALID_CONFIG;
        snprintf(result.message, sizeof(result.message),
                 "input_file, package_name, and app_name are required");
        return result;
    }

    /* Work on a mutable copy so we can resolve env vars */
    ApkBuildConfig cfg = *config;
    resolve_paths(&cfg);

    /* Create build directory: build/<package_name>/ */
    char build_dir[1024];
    snprintf(build_dir, sizeof(build_dir), "build-apk/%s", cfg.package_name);
    mkdir_p(build_dir);

    printf("[apk-builder] ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????????\n");
    printf("[apk-builder] Building APK: %s\n", cfg.app_name);
    printf("[apk-builder]   Package:  %s\n",  cfg.package_name);
    printf("[apk-builder]   Version:  %s (%d)\n", cfg.version_name, cfg.version_code);
    printf("[apk-builder] ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????????\n");

    ApkError err;

    /* Stage 1: Compile */
    log_step(&cfg, "Stage 1/6 ??? Compile .cpx ??? native .so");
    err = apk_stage_compile(&cfg, build_dir);
    if (err != APK_OK) goto fail;

    /* Stage 2: Manifest */
    log_step(&cfg, "Stage 2/6 ??? Generate AndroidManifest.xml");
    err = apk_stage_manifest(&cfg, build_dir);
    if (err != APK_OK) goto fail;

    /* Stage 3: Resources */
    log_step(&cfg, "Stage 3/6 ??? Pack resources");
    err = apk_stage_resources(&cfg, build_dir);
    if (err != APK_OK) goto fail;

    /* Stage 4: Package */
    log_step(&cfg, "Stage 4/6 ??? Package ??? unsigned.apk");
    err = apk_stage_package(&cfg, build_dir);
    if (err != APK_OK) goto fail;

    /* Stage 5: Align, then Stage 6: Sign */
    {
        char unsigned_apk[2048], aligned_apk[2048];
        const char* out = cfg.output_apk[0] ? cfg.output_apk : "app-release.apk";
        snprintf(unsigned_apk, sizeof(unsigned_apk), "%s/app-unsigned.apk", build_dir);
        snprintf(aligned_apk,  sizeof(aligned_apk),  "%s/app-aligned.apk",  build_dir);

        log_step(&cfg, "Stage 5/6 ??? zipalign");
        err = apk_stage_align(&cfg, unsigned_apk, aligned_apk);
        if (err != APK_OK) goto fail;

        log_step(&cfg, "Stage 6/6 ??? Sign APK");
        err = apk_stage_sign(&cfg, build_dir, aligned_apk, out);
        if (err != APK_OK) goto fail;

        snprintf(result.output_path, sizeof(result.output_path), "%s", out);
    }

    if (!cfg.keep_intermediates) {
        /* Optional cleanup of build dir */
    }

    result.code = APK_OK;
    result.build_time_ms = (long long)((clock() - t_start) * 1000 / CLOCKS_PER_SEC);
    printf("[apk-builder] ??? Build complete in %lldms ??? %s\n",
           result.build_time_ms, result.output_path);
    return result;

fail:
    result.code = err;
    snprintf(result.message, sizeof(result.message),
             "Build failed at stage with error: %s", apk_error_string(err));
    fprintf(stderr, "[apk-builder] ??? %s\n", result.message);
    return result;
}

/* ========================================================================
 * Utilities
 * ======================================================================== */

const char* apk_error_string(ApkError err) {
    switch (err) {
        case APK_OK:                   return "OK";
        case APK_ERR_INVALID_CONFIG:   return "Invalid configuration";
        case APK_ERR_COMPILE_FAILED:   return "Compilation failed";
        case APK_ERR_MANIFEST_FAILED:  return "Manifest generation failed";
        case APK_ERR_RESOURCE_FAILED:  return "Resource packing failed";
        case APK_ERR_ZIP_FAILED:       return "ZIP packaging failed";
        case APK_ERR_SIGN_FAILED:      return "APK signing failed";
        case APK_ERR_ALIGN_FAILED:     return "zipalign failed";
        case APK_ERR_NDK_NOT_FOUND:    return "Android NDK not found";
        case APK_ERR_SDK_NOT_FOUND:    return "Android SDK not found";
        case APK_ERR_IO:               return "I/O error";
        default:                       return "Unknown error";
    }
}

void apk_config_print(const ApkBuildConfig* cfg) {
    printf("ApkBuildConfig {\n");
    printf("  input_file:    %s\n",  cfg->input_file);
    printf("  package_name:  %s\n",  cfg->package_name);
    printf("  app_name:      %s\n",  cfg->app_name);
    printf("  version:       %s (%d)\n", cfg->version_name, cfg->version_code);
    printf("  sdk:           min=%d target=%d\n", cfg->min_sdk, cfg->target_sdk);
    printf("  debug_build:   %s\n",  cfg->debug_build ? "yes" : "no");
    printf("  android_ndk:   %s\n",  cfg->android_ndk[0] ? cfg->android_ndk : "(from env)");
    printf("  android_sdk:   %s\n",  cfg->android_sdk[0] ? cfg->android_sdk : "(from env)");
    printf("  output_apk:    %s\n",  cfg->output_apk[0]  ? cfg->output_apk  : "app-release.apk");
    printf("}\n");
}




