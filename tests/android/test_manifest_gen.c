#include "manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static int fail(const char* msg) {
    fprintf(stderr, "manifest test failed: %s\n", msg);
    return 1;
}

static char* read_file(const char* path, long* out_size) {
    FILE* f = fopen(path, "rb");
    char* data;
    long size;

    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    data = (char*)malloc((size_t)size + 1);
    if (!data) {
        fclose(f);
        return NULL;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return NULL;
    }

    data[size] = '\0';
    fclose(f);

    if (out_size) *out_size = size;
    return data;
}

static int contains(const char* haystack, const char* needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static int make_temp_path(char* out, size_t out_size) {
#ifdef _WIN32
    char tmp_dir[MAX_PATH];
    char tmp_file[MAX_PATH];

    if (!GetTempPathA((DWORD)sizeof(tmp_dir), tmp_dir)) return 0;
    if (!GetTempFileNameA(tmp_dir, "cpx", 0, tmp_file)) return 0;
    snprintf(out, out_size, "%s", tmp_file);
    return 1;
#else
    char tmpl[] = "/tmp/casprix_manifest_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return 0;
    close(fd);
    snprintf(out, out_size, "%s", tmpl);
    return 1;
#endif
}

int main(void) {
    ApkBuildConfig cfg;
    ManifestBuilder mb;
    char manifest_path[1024];
    char* xml;
    long xml_size = 0;
    int rc = 1;

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.package_name, sizeof(cfg.package_name), "com.casprix.android.test");
    snprintf(cfg.app_name, sizeof(cfg.app_name), "Casprix Android Test");
    snprintf(cfg.main_activity, sizeof(cfg.main_activity), "MainActivity");
    snprintf(cfg.version_name, sizeof(cfg.version_name), "1.0.0");
    cfg.version_code = 1;
    cfg.min_sdk = 24;
    cfg.target_sdk = 34;

    manifest_builder_init(&mb, &cfg);
    if (manifest_add_permission(&mb, "android.permission.INTERNET") != 0) {
        return fail("manifest_add_permission failed");
    }
    if (manifest_add_activity(&mb,
                              "android.app.NativeActivity",
                              MANIFEST_ACTIVITY_NATIVE,
                              cfg.app_name,
                              cfg.main_activity) != 0) {
        return fail("manifest_add_activity failed");
    }

    if (!make_temp_path(manifest_path, sizeof(manifest_path))) {
        return fail("could not create temp manifest path");
    }

    if (manifest_write_builder(&mb, manifest_path) != 0) {
        remove(manifest_path);
        return fail("manifest_write_builder failed");
    }

    xml = read_file(manifest_path, &xml_size);
    remove(manifest_path);
    if (!xml) return fail("could not read generated manifest");

    if (!contains(xml, "<activity") ||
        !contains(xml, "android:name=\"android.app.NativeActivity\"")) {
        fprintf(stderr, "%s\n", xml);
        free(xml);
        return fail("NativeActivity declaration missing");
    }

    if (!contains(xml, "android:minSdkVersion=\"24\"")) {
        fprintf(stderr, "%s\n", xml);
        free(xml);
        return fail("minSdkVersion 24 missing");
    }

    if (!contains(xml, "android:targetSdkVersion=\"34\"")) {
        fprintf(stderr, "%s\n", xml);
        free(xml);
        return fail("targetSdkVersion 34 missing");
    }

    if (!contains(xml, "<uses-permission android:name=\"android.permission.INTERNET\"")) {
        fprintf(stderr, "%s\n", xml);
        free(xml);
        return fail("INTERNET permission missing");
    }

    free(xml);
    rc = 0;
    return rc;
}
