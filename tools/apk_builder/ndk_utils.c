/*
 * ndk_utils.c - Android NDK path detection.
 *
 * The detector prefers explicit environment variables and then scans the
 * default SDK ndk/ directories for the highest semantic version.
 */

#include "ndk_utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <direct.h>
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#define CPX_NDK_PATH_MAX 1024
#define CPX_NDK_VERSION_PARTS_MAX 16

static int cpx_is_dir(const char* path) {
    if (!path || !path[0]) return 0;
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int cpx_path_join(char* out, size_t out_sz, const char* a, const char* b) {
    const char sep =
#ifdef _WIN32
        '\\';
#else
        '/';
#endif

    size_t a_len;
    if (!out || out_sz == 0 || !a || !b) return 0;
    a_len = strlen(a);
    if (a_len + 1 + strlen(b) + 1 > out_sz) return 0;

    memcpy(out, a, a_len);
    if (a_len > 0 && out[a_len - 1] != '/' && out[a_len - 1] != '\\') {
        out[a_len++] = sep;
    }
    memcpy(out + a_len, b, strlen(b));
    out[a_len + strlen(b)] = '\0';
    return 1;
}

static int cpx_copy_path(char* out, size_t out_sz, const char* path) {
    size_t len;
    if (!out || out_sz == 0 || !path) return 0;
    len = strlen(path);
    if (len + 1 > out_sz) return 0;
    memcpy(out, path, len + 1);
    return 1;
}

static int cpx_parse_version(const char* s, int* parts, int max_parts) {
    int count = 0;
    const char* p = s;

    if (!s || !parts || max_parts <= 0) return 0;
    while (*p && count < max_parts) {
        char* end = NULL;
        long value;

        if (!isdigit((unsigned char)*p)) return 0;
        value = strtol(p, &end, 10);
        if (end == p || value < 0) return 0;
        parts[count++] = (int)value;

        if (*end == '\0') return count;
        if (*end != '.') return 0;
        p = end + 1;
    }

    return count;
}

static int cpx_compare_versions(const char* a, const char* b) {
    int ap[CPX_NDK_VERSION_PARTS_MAX];
    int bp[CPX_NDK_VERSION_PARTS_MAX];
    int ac = cpx_parse_version(a, ap, CPX_NDK_VERSION_PARTS_MAX);
    int bc = cpx_parse_version(b, bp, CPX_NDK_VERSION_PARTS_MAX);
    int n = ac > bc ? ac : bc;

    if (ac == 0 && bc == 0) return strcmp(a, b);
    if (ac == 0) return -1;
    if (bc == 0) return 1;

    for (int i = 0; i < n; ++i) {
        int av = (i < ac) ? ap[i] : 0;
        int bv = (i < bc) ? bp[i] : 0;
        if (av != bv) return (av > bv) ? 1 : -1;
    }
    return 0;
}

static int cpx_try_env_path(const char* name, char* out_buf, size_t buf_size) {
    const char* value = getenv(name);
    if (!value || !value[0]) return 0;
    if (!cpx_is_dir(value)) return 0;
    return cpx_copy_path(out_buf, buf_size, value);
}

static int cpx_scan_ndk_dir(const char* base_dir,
                            char* out_buf,
                            size_t buf_size,
                            char* version_buf,
                            size_t version_buf_size) {
    char best_dir[CPX_NDK_PATH_MAX];
    char best_name[256];
    int found = 0;

    if (!base_dir || !base_dir[0] || !cpx_is_dir(base_dir)) return 0;
    best_dir[0] = '\0';
    best_name[0] = '\0';

#ifdef _WIN32
    {
        char pattern[CPX_NDK_PATH_MAX];
        WIN32_FIND_DATAA fd;
        HANDLE h;

        if (!cpx_path_join(pattern, sizeof(pattern), base_dir, "*")) return 0;
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;

        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
                continue;
            }
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                continue;
            }
            if (!found || cpx_compare_versions(fd.cFileName, best_name) > 0) {
                found = 1;
                snprintf(best_name, sizeof(best_name), "%s", fd.cFileName);
                if (!cpx_path_join(best_dir, sizeof(best_dir), base_dir, best_name)) {
                    best_dir[0] = '\0';
                    found = 0;
                }
            }
        } while (FindNextFileA(h, &fd));

        FindClose(h);
    }
#else
    {
        DIR* dir = opendir(base_dir);
        if (!dir) return 0;

        for (;;) {
            struct dirent* ent = readdir(dir);
            if (!ent) break;
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }

            char candidate[CPX_NDK_PATH_MAX];
            if (!cpx_path_join(candidate, sizeof(candidate), base_dir, ent->d_name)) {
                continue;
            }
            if (!cpx_is_dir(candidate)) {
                continue;
            }
            if (!found || cpx_compare_versions(ent->d_name, best_name) > 0) {
                found = 1;
                snprintf(best_name, sizeof(best_name), "%s", ent->d_name);
                snprintf(best_dir, sizeof(best_dir), "%s", candidate);
            }
        }

        closedir(dir);
    }
#endif

    if (!found) return 0;
    if (version_buf && version_buf_size > 0) {
        if (!cpx_copy_path(version_buf, version_buf_size, best_name)) return 0;
    }
    return cpx_copy_path(out_buf, buf_size, best_dir);
}

const char* cpx_ndk_detect(char* out_buf, size_t buf_size) {
    const char* env_names[] = {
        "ANDROID_NDK_HOME",
        "ANDROID_NDK",
        "NDK_HOME",
    };
    size_t i;

    if (!out_buf || buf_size == 0) return NULL;
    out_buf[0] = '\0';

    for (i = 0; i < sizeof(env_names) / sizeof(env_names[0]); ++i) {
        if (cpx_try_env_path(env_names[i], out_buf, buf_size)) {
            return out_buf;
        }
    }

#ifdef _WIN32
    {
        const char* localapp = getenv("LOCALAPPDATA");
        char base[CPX_NDK_PATH_MAX];
        if (localapp && localapp[0]) {
            int n = snprintf(base, sizeof(base), "%s\\Android\\Sdk\\ndk", localapp);
            if (n > 0 && n < (int)sizeof(base) &&
                cpx_scan_ndk_dir(base, out_buf, buf_size, NULL, 0)) {
                return out_buf;
            }
        }
    }
#else
    {
        const char* home = getenv("HOME");
        char base[CPX_NDK_PATH_MAX];
        char best_path[CPX_NDK_PATH_MAX];
        char best_version[CPX_NDK_PATH_MAX];

        if (home && home[0]) {
            best_version[0] = '\0';
            best_path[0] = '\0';

            {
                int n = snprintf(base, sizeof(base), "%s/Android/Sdk/ndk", home);
                if (n > 0 && n < (int)sizeof(base) &&
                    cpx_scan_ndk_dir(base, best_path, sizeof(best_path),
                                     best_version, sizeof(best_version))) {
                }
            }
            {
                char other_path[CPX_NDK_PATH_MAX];
                char other_version[CPX_NDK_PATH_MAX];
                int n = snprintf(base, sizeof(base), "%s/Library/Android/sdk/ndk", home);
                if (n > 0 && n < (int)sizeof(base) &&
                    cpx_scan_ndk_dir(base, other_path, sizeof(other_path), other_version, sizeof(other_version))) {
                    if (!best_path[0] || cpx_compare_versions(other_version, best_version) > 0) {
                        snprintf(best_path, sizeof(best_path), "%s", other_path);
                        snprintf(best_version, sizeof(best_version), "%s", other_version);
                    }
                }
            }

            if (best_path[0] && cpx_copy_path(out_buf, buf_size, best_path)) {
                return out_buf;
            }
        }
    }
#endif

    return NULL;
}
