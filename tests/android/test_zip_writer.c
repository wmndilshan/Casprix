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
        if (files[i]) remove(files[i]);
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

static bool entry_matches(const char* got, const char* expected) {
    return got && expected && strcmp(got, expected) == 0;
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
    size_t eocd_pos = 0;
    size_t i;

    if (read_file(path, &data, &size) != 0) {
        printf("Could not read zip: %s\n", path);
        return -1;
    }

    if (size < 22) {
        free(data);
        return -1;
    }

    for (i = size - 22 + 1; i-- > 0;) {
        if (read_u32(data + i) == 0x06054b50u) {
            eocd_pos = i;
            break;
        }
        if (i == 0) break;
    }

    if (read_u32(data + eocd_pos) != 0x06054b50u) {
        free(data);
        return -1;
    }

    uint16_t total_entries = read_u16(data + eocd_pos + 10);
    uint32_t cd_size = read_u32(data + eocd_pos + 12);
    uint32_t cd_offset = read_u32(data + eocd_pos + 16);

    if (cd_offset + cd_size > size) {
        free(data);
        return -1;
    }

    if (total_entries != expect_count) {
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

        uint16_t method = read_u16(data + cd_pos + 10);
        uint32_t crc = read_u32(data + cd_pos + 16);
        uint32_t comp_size = read_u32(data + cd_pos + 20);
        uint32_t uncomp_size = read_u32(data + cd_pos + 24);
        uint16_t name_len = read_u16(data + cd_pos + 28);
        uint16_t extra_len = read_u16(data + cd_pos + 30);
        uint16_t comment_len = read_u16(data + cd_pos + 32);
        uint32_t local_off = read_u32(data + cd_pos + 42);

        size_t entry_end = cd_pos + 46u + name_len + extra_len + comment_len;
        if (entry_end > size || local_off + 30u > size) {
            free(seen);
            free(data);
            return -1;
        }

        const char* name = (const char*)(data + cd_pos + 46);
        int idx = find_expected_index(expect, expect_count, name);
        if (idx < 0) {
            free(seen);
            free(data);
            return -1;
        }

        if (seen[idx]) {
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

        if (!entry_matches(name, expect[idx].name) ||
            local_name_len != name_len ||
            comp_size > expect[idx].size ||
            uncomp_size > expect[idx].size ||
            payload_off + comp_size > size) {
            free(seen);
            free(data);
            return -1;
        }

        if (method != 0) {
            free(seen);
            free(data);
            return -1;
        }

        if (zip_crc32(data + payload_off, comp_size) != crc) {
            free(seen);
            free(data);
            return -1;
        }

        if (comp_size != expect[idx].size || uncomp_size != expect[idx].size) {
            free(seen);
            free(data);
            return -1;
        }

        if (memcmp(data + payload_off, expect[idx].data, expect[idx].size) != 0) {
            free(seen);
            free(data);
            return -1;
        }

        seen[idx] = true;
        cd_pos = entry_end;
    }

    for (i = 0; i < expect_count; i++) {
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

static int test_zip_round_trip(void) {
    char temp_dir[256];
    char zip_path[512];
    char txt_path[512];
    char blob_path[512];
    static const uint8_t txt[] = "hello zip";
    static const uint8_t blob[] = { 0x7f, 'E', 'L', 'F', 0x00, 0x01, 0x02, 0x03 };
    ZipExpect expect[2];
    ZipWriter* zw;

    if (make_temp_dir(temp_dir, sizeof(temp_dir), "zip") != 0) {
        printf("Could not create temp dir\n");
        return 1;
    }

    snprintf(zip_path, sizeof(zip_path), "%s%croundtrip.apk", temp_dir, PATH_SEP);
    snprintf(txt_path, sizeof(txt_path), "%s%chello.txt", temp_dir, PATH_SEP);
    snprintf(blob_path, sizeof(blob_path), "%s%clibMainActivity.so", temp_dir, PATH_SEP);

    if (write_file(txt_path, txt, sizeof(txt) - 1) != 0 ||
        write_file(blob_path, blob, sizeof(blob)) != 0) {
        printf("Could not create fixtures\n");
        cleanup_temp_dir(temp_dir, (const char* const[]){ txt_path, blob_path, zip_path }, 3);
        return 1;
    }

    zw = zip_writer_open(zip_path);
    if (!zw) {
        printf("zip_writer_open failed\n");
        cleanup_temp_dir(temp_dir, (const char* const[]){ txt_path, blob_path, zip_path }, 3);
        return 1;
    }

    if (zip_writer_add_file(zw, "assets/hello.txt", txt_path, 0) != 0 ||
        zip_writer_add_file(zw, "lib/arm64-v8a/libMainActivity.so", blob_path, 0) != 0 ||
        zip_writer_close(zw) != 0) {
        printf("zip_writer round trip failed\n");
        cleanup_temp_dir(temp_dir, (const char* const[]){ txt_path, blob_path, zip_path }, 3);
        return 1;
    }

    expect[0].name = "assets/hello.txt";
    expect[0].data = txt;
    expect[0].size = sizeof(txt) - 1;
    expect[1].name = "lib/arm64-v8a/libMainActivity.so";
    expect[1].data = blob;
    expect[1].size = sizeof(blob);

    {
        int ok = cpx_zip_verify(zip_path) == CPX_ZIP_OK ? 0 : 1;
        cleanup_temp_dir(temp_dir, (const char* const[]){ txt_path, blob_path, zip_path }, 3);
        return ok;
    }
}

static int test_missing_file_fails(void) {
    char temp_dir[256];
    char zip_path[512];
    ZipWriter* zw;
    int rc;

    if (make_temp_dir(temp_dir, sizeof(temp_dir), "zipmiss") != 0) {
        printf("Could not create temp dir\n");
        return 1;
    }

    snprintf(zip_path, sizeof(zip_path), "%s%cmissing.apk", temp_dir, PATH_SEP);
    zw = zip_writer_open(zip_path);
    if (!zw) {
        printf("zip_writer_open failed\n");
        cleanup_temp_dir(temp_dir, (const char* const[]){ zip_path }, 1);
        return 1;
    }

    rc = zip_writer_add_file(zw, "lib/arm64-v8a/libMainActivity.so",
                             "does/not/exist.so", 0);
    zip_writer_close(zw);
    cleanup_temp_dir(temp_dir, (const char* const[]){ zip_path }, 1);
    return rc != 0 ? 0 : 1;
}

int main(void) {
    printf("APK ZIP writer tests\n");
    report(test_zip_round_trip() == 0, "zip_writer round trip and CRC verification");
    report(test_missing_file_fails() == 0, "missing file fails the packaging path");
    return g_failures ? 1 : 0;
}
