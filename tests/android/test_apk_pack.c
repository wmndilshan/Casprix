#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <process.h>
#define MKDIR(path) _mkdir(path)
#define RMDIR(path) _rmdir(path)
#define PATH_SEP '\\'
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(path) mkdir((path), 0755)
#define RMDIR(path) rmdir(path)
#define PATH_SEP '/'
#endif

#include "../../tools/apk_builder/zip_writer.h"

typedef struct {
    const char* name;
    const uint8_t* data;
    size_t size;
} ZipExpect;

static int g_failures = 0;
static unsigned g_temp_counter = 0;

static void report(bool ok, const char* msg) {
    if (ok) {
        printf("[PASS] %s\n", msg);
    } else {
        printf("[FAIL] %s\n", msg);
        g_failures++;
    }
}

static uint16_t read_u16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int make_temp_dir(char* out, size_t out_size, const char* tag) {
    if (!out || out_size == 0) return -1;
#ifdef _WIN32
    char root[MAX_PATH];
    DWORD len = GetTempPathA((DWORD)sizeof(root), root);
    if (len == 0 || len >= sizeof(root)) {
        snprintf(root, sizeof(root), "C:\\Windows\\Temp");
    }
    snprintf(out, out_size, "%s\\casprix_%s_%d_%ld_%u",
             root, tag, _getpid(), (long)time(NULL), g_temp_counter++);
#else
    const char* root = getenv("TMPDIR");
    if (!root || !root[0]) root = getenv("TEMP");
    if (!root || !root[0]) root = "/tmp";
    snprintf(out, out_size, "%s/casprix_%s_%d_%ld_%u",
             root, tag, (int)getpid(), (long)time(NULL), g_temp_counter++);
#endif
    return MKDIR(out);
}

static void cleanup_temp_dir(const char* dir, const char* const* files, size_t file_count) {
    if (!dir) return;
    for (size_t i = 0; i < file_count; i++) {
        if (files && files[i]) remove(files[i]);
    }
    RMDIR(dir);
}

static int write_file(const char* path, const void* data, size_t size) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    if (size > 0 && fwrite(data, 1, size, f) != size) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int read_file(const char* path, uint8_t** out_data, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    uint8_t* buf;
    long sz;

    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out_data = buf;
    *out_size = (size_t)sz;
    return 0;
}

static int find_expected_index(const ZipExpect* expect, size_t expect_count, const char* name) {
    for (size_t i = 0; i < expect_count; i++) {
        if (expect[i].name && strcmp(expect[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static int verify_zip_archive(const char* path, const ZipExpect* expect, size_t expect_count) {
    uint8_t* data = NULL;
    size_t size = 0;

    if (read_file(path, &data, &size) != 0) {
        return -1;
    }

    if (size < 22) {
        free(data);
        return -1;
    }

    size_t eocd_pos = SIZE_MAX;
    for (size_t i = size - 22 + 1; i-- > 0;) {
        if (read_u32(data + i) == 0x06054b50u) {
            eocd_pos = i;
            break;
        }
        if (i == 0) break;
    }
    if (eocd_pos == SIZE_MAX) {
        free(data);
        return -1;
    }

    uint16_t total_entries = read_u16(data + eocd_pos + 10);
    uint32_t cd_size = read_u32(data + eocd_pos + 12);
    uint32_t cd_offset = read_u32(data + eocd_pos + 16);
    if (total_entries != expect_count || cd_offset + cd_size > size) {
        free(data);
        return -1;
    }

    bool* seen = (bool*)calloc(expect_count, sizeof(bool));
    if (!seen) {
        free(data);
        return -1;
    }

    size_t cd_pos = cd_offset;
    for (uint16_t entry = 0; entry < total_entries; entry++) {
        if (cd_pos + 46 > size || read_u32(data + cd_pos) != 0x02014b50u) {
            free(seen);
            free(data);
            return -1;
        }

        uint32_t crc = read_u32(data + cd_pos + 16);
        uint32_t comp_size = read_u32(data + cd_pos + 20);
        uint32_t uncomp_size = read_u32(data + cd_pos + 24);
        uint16_t name_len = read_u16(data + cd_pos + 28);
        uint16_t extra_len = read_u16(data + cd_pos + 30);
        uint16_t comment_len = read_u16(data + cd_pos + 32);
        uint32_t local_off = read_u32(data + cd_pos + 42);
        size_t entry_end = cd_pos + 46u + name_len + extra_len + comment_len;
        const char* name = (const char*)(data + cd_pos + 46);
        int idx = find_expected_index(expect, expect_count, name);

        if (idx < 0 || seen[idx] || entry_end > size || local_off + 30u > size) {
            free(seen);
            free(data);
            return -1;
        }

        if (read_u32(data + local_off) != 0x04034b50u) {
            free(seen);
            free(data);
            return -1;
        }

        uint16_t local_name_len = read_u16(data + local_off + 26);
        uint16_t local_extra_len = read_u16(data + local_off + 28);
        size_t payload_off = local_off + 30u + local_name_len + local_extra_len;

        if (comp_size != expect[idx].size ||
            uncomp_size != expect[idx].size ||
            payload_off + comp_size > size ||
            zip_crc32(data + payload_off, comp_size) != crc ||
            memcmp(data + payload_off, expect[idx].data, expect[idx].size) != 0) {
            free(seen);
            free(data);
            return -1;
        }

        seen[idx] = true;
        cd_pos = entry_end;
    }

    for (size_t i = 0; i < expect_count; i++) {
        if (!seen[i]) {
            free(seen);
            free(data);
            return -1;
        }
    }

    free(seen);
    free(data);
    return 0;
}

static bool manifest_parses_like_xml(const uint8_t* data, size_t size) {
    const char* xml = (const char*)data;
    return size > 0 &&
           strstr(xml, "<?xml") != NULL &&
           strstr(xml, "<manifest") != NULL &&
           strstr(xml, "</manifest>") != NULL &&
           strstr(xml, "android.app.NativeActivity") != NULL &&
           strstr(xml, "android:name=\"android.app.lib_name\"") != NULL;
}

static int build_minimal_apk(char* apk_path, size_t apk_path_size, const char* temp_dir,
                             const uint8_t* manifest, size_t manifest_size,
                             const uint8_t* so_data, size_t so_size) {
    char manifest_file[512];
    char so_file[512];
    ZipWriter* zw;

    snprintf(apk_path, apk_path_size, "%s%cminimal.apk", temp_dir, PATH_SEP);
    snprintf(manifest_file, sizeof(manifest_file), "%s%cAndroidManifest.xml", temp_dir, PATH_SEP);
    snprintf(so_file, sizeof(so_file), "%s%cMainActivity.so", temp_dir, PATH_SEP);

    if (write_file(manifest_file, manifest, manifest_size) != 0 ||
        write_file(so_file, so_data, so_size) != 0) {
        cleanup_temp_dir(temp_dir, (const char* const[]){ manifest_file, so_file, apk_path }, 3);
        return -1;
    }

    zw = zip_writer_open(apk_path);
    if (!zw) {
        cleanup_temp_dir(temp_dir, (const char* const[]){ manifest_file, so_file, apk_path }, 3);
        return -1;
    }

    if (zip_writer_add_file(zw, "AndroidManifest.xml", manifest_file, 0) != 0 ||
        zip_writer_add_file(zw, "lib/arm64-v8a/libMainActivity.so", so_file, 0) != 0) {
        zip_writer_close(zw);
        cleanup_temp_dir(temp_dir, (const char* const[]){ manifest_file, so_file, apk_path }, 3);
        return -1;
    }

    if (zip_writer_close(zw) != 0) {
        cleanup_temp_dir(temp_dir, (const char* const[]){ manifest_file, so_file, apk_path }, 3);
        return -1;
    }

    return 0;
}

static int test_minimal_apk_and_manifest(void) {
    static const uint8_t manifest[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
        "    package=\"com.casprix.tests\"\n"
        "    android:versionCode=\"1\"\n"
        "    android:versionName=\"1.0.0\">\n"
        "    <application android:label=\"Casprix Test\" android:hasCode=\"false\">\n"
        "        <activity android:name=\"android.app.NativeActivity\">\n"
        "            <meta-data android:name=\"android.app.lib_name\" android:value=\"MainActivity\" />\n"
        "            <intent-filter>\n"
        "                <action android:name=\"android.intent.action.MAIN\" />\n"
        "                <category android:name=\"android.intent.category.LAUNCHER\" />\n"
        "            </intent-filter>\n"
        "        </activity>\n"
        "    </application>\n"
        "</manifest>\n";
    static const uint8_t so_data[] = { 0x7f, 'E', 'L', 'F', 0, 1, 2, 3, 4, 5 };
    char temp_dir[256];
    char apk_path[512];
    char manifest_file[512];
    char so_file[512];
    ZipExpect expect[2];
    uint8_t* extracted = NULL;
    size_t extracted_size = 0;

    if (make_temp_dir(temp_dir, sizeof(temp_dir), "apk") != 0) {
        return -1;
    }

    snprintf(manifest_file, sizeof(manifest_file), "%s%cAndroidManifest.xml", temp_dir, PATH_SEP);
    snprintf(so_file, sizeof(so_file), "%s%cMainActivity.so", temp_dir, PATH_SEP);

    if (build_minimal_apk(apk_path, sizeof(apk_path), temp_dir,
                          manifest, sizeof(manifest) - 1,
                          so_data, sizeof(so_data)) != 0) {
        cleanup_temp_dir(temp_dir, NULL, 0);
        return -1;
    }

    expect[0].name = "AndroidManifest.xml";
    expect[0].data = manifest;
    expect[0].size = sizeof(manifest) - 1;
    expect[1].name = "lib/arm64-v8a/libMainActivity.so";
    expect[1].data = so_data;
    expect[1].size = sizeof(so_data);

    if (cpx_zip_verify(apk_path) != CPX_ZIP_OK) {
        cleanup_temp_dir(temp_dir, (const char* const[]){ manifest_file, so_file, apk_path }, 3);
        return -1;
    }

    if (read_file(apk_path, &extracted, &extracted_size) != 0) {
        cleanup_temp_dir(temp_dir, (const char* const[]){ manifest_file, so_file, apk_path }, 3);
        return -1;
    }

    bool manifest_found = false;
    const uint8_t* manifest_in_zip = NULL;
    for (size_t i = 0; i + sizeof(manifest) - 1 <= extracted_size; i++) {
        if (memcmp(extracted + i, manifest, sizeof(manifest) - 1) == 0) {
            manifest_found = true;
            manifest_in_zip = extracted + i;
            break;
        }
    }
    bool xml_ok = manifest_in_zip && manifest_parses_like_xml(manifest_in_zip, sizeof(manifest) - 1);

    free(extracted);
    {
        int rc = (manifest_found && xml_ok) ? 0 : -1;
        cleanup_temp_dir(temp_dir, (const char* const[]){ manifest_file, so_file, apk_path }, 3);
        return rc;
    }
}

static int test_missing_so_reports_failure(void) {
    static const uint8_t manifest[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
        "    package=\"com.casprix.tests\">\n"
        "</manifest>\n";
    char temp_dir[256];
    char apk_path[512];
    char manifest_file[512];
    ZipWriter* zw;
    int rc;

    if (make_temp_dir(temp_dir, sizeof(temp_dir), "apkmiss") != 0) {
        return -1;
    }

    snprintf(apk_path, sizeof(apk_path), "%s%cmissing.apk", temp_dir, PATH_SEP);
    snprintf(manifest_file, sizeof(manifest_file), "%s%cAndroidManifest.xml", temp_dir, PATH_SEP);
    if (write_file(manifest_file, manifest, sizeof(manifest) - 1) != 0) {
        cleanup_temp_dir(temp_dir, (const char* const[]){ manifest_file, apk_path }, 2);
        return -1;
    }

    zw = zip_writer_open(apk_path);
    if (!zw) {
        cleanup_temp_dir(temp_dir, (const char* const[]){ manifest_file, apk_path }, 2);
        return -1;
    }

    rc = zip_writer_add_file(zw, "AndroidManifest.xml", manifest_file, 0);
    if (rc != 0) {
        zip_writer_close(zw);
        cleanup_temp_dir(temp_dir, (const char* const[]){ manifest_file, apk_path }, 2);
        return -1;
    }

    rc = zip_writer_add_file(zw, "lib/arm64-v8a/libMainActivity.so",
                             "does/not/exist.so", 0);
    zip_writer_close(zw);
    cleanup_temp_dir(temp_dir, (const char* const[]){ manifest_file, apk_path }, 2);
    return rc != 0 ? 0 : -1;
}

int main(void) {
    printf("APK packaging regression tests\n");
    report(test_minimal_apk_and_manifest() == 0,
           "minimal APK builds, validates as ZIP, and contains AndroidManifest.xml");
    report(test_missing_so_reports_failure() == 0,
           "missing .so fails the manual packaging path");
    return g_failures ? 1 : 0;
}
