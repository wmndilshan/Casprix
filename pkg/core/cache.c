/**
 * Local Package Cache Implementation
 */

#include "cache.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <unistd.h>
#include <dirent.h>
#endif

static char cache_dir_path[512] = {0};

const char* pkg_cache_dir(void) {
    if (cache_dir_path[0] != '\0') {
        return cache_dir_path;
    }
    
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    const char* home = getenv("HOME");
#endif
    
    if (!home) home = ".";
    
    snprintf(cache_dir_path, sizeof(cache_dir_path), "%s/.cpkg/cache", home);
    
    return cache_dir_path;
}

static bool dir_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void mkdir_recursive(const char* path) {
    char tmp[512];
    char* p = NULL;
    size_t len;
    
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/' || tmp[len - 1] == '\\')
        tmp[len - 1] = 0;
    
    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = 0;
            if (!dir_exists(tmp)) {
                mkdir(tmp, 0755);
            }
            *p = '/';
        }
    }
    
    if (!dir_exists(tmp)) {
        mkdir(tmp, 0755);
    }
}

bool pkg_cache_init(void) {
    const char* dir = pkg_cache_dir();
    
    if (!dir_exists(dir)) {
        mkdir_recursive(dir);
    }
    
    return dir_exists(dir);
}

bool pkg_cache_has(const char* name, const char* version) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/%s", pkg_cache_dir(), name, version);
    
    return dir_exists(path);
}

char* pkg_cache_get_path(const char* name, const char* version) {
    if (!pkg_cache_has(name, version)) return NULL;
    
    char* path = (char*)malloc(1024);
    snprintf(path, 1024, "%s/%s/%s", pkg_cache_dir(), name, version);
    
    return path;
}

char* pkg_cache_put(const char* name, const char* version, const char* tarball) {
    char pkg_dir[1024];
    snprintf(pkg_dir, sizeof(pkg_dir), "%s/%s/%s", pkg_cache_dir(), name, version);
    
    /* Create package directory */
    mkdir_recursive(pkg_dir);
    
    /* For now, just return the directory path */
    /* In a full implementation, we'd extract the tarball here */
    
    return strdup(pkg_dir);
}

bool pkg_cache_remove(const char* name, const char* version) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/%s", pkg_cache_dir(), name, version);
    
    /* Platform-specific directory removal */
#ifdef _WIN32
    char cmd[1280];
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", path);
    return system(cmd) == 0;
#else
    char cmd[1280];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", path);
    return system(cmd) == 0;
#endif
}

bool pkg_cache_clean(void) {
    const char* cache = pkg_cache_dir();
    
#ifdef _WIN32
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", cache);
    return system(cmd) == 0;
#else
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"/*", cache);
    return system(cmd) == 0;
#endif
}

char** pkg_cache_list(int* count) {
    *count = 0;
    
    const char* cache = pkg_cache_dir();
    if (!dir_exists(cache)) return NULL;
    
    /* Platform-specific directory listing */
#ifdef _WIN32
    WIN32_FIND_DATAA ffd;
    HANDLE hFind;
    
    char search_path[1024];
    snprintf(search_path, sizeof(search_path), "%s/*", cache);
    
    hFind = FindFirstFileA(search_path, &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return NULL;
    
    /* Count entries */
    int capacity = 100;
    char** list = (char**)malloc(capacity * sizeof(char*));
    
    do {
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (strcmp(ffd.cFileName, ".") != 0 && strcmp(ffd.cFileName, "..") != 0) {
                if (*count >= capacity) {
                    capacity *= 2;
                    list = (char**)realloc(list, capacity * sizeof(char*));
                }
                list[*count] = strdup(ffd.cFileName);
                (*count)++;
            }
        }
    } while (FindNextFileA(hFind, &ffd) != 0);
    
    FindClose(hFind);
    return list;
#else
    DIR* dir = opendir(cache);
    if (!dir) return NULL;
    
    int capacity = 100;
    char** list = (char**)malloc(capacity * sizeof(char*));
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Use stat to check for directory (portable, avoids DT_DIR dependency) */
        char entry_path[512];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", pkg_cache_dir(), entry->d_name);
        struct stat entry_st;
        bool is_dir = stat(entry_path, &entry_st) == 0 && S_ISDIR(entry_st.st_mode);
        if (is_dir &&
            strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0) {
            if (*count >= capacity) {
                capacity *= 2;
                list = (char**)realloc(list, capacity * sizeof(char*));
            }
            list[*count] = strdup(entry->d_name);
            (*count)++;
        }
    }
    
    closedir(dir);
    return list;
#endif
}

long long pkg_cache_size(void) {
    /* Simplified - return 0 for now */
    /* In full implementation, recursively calculate directory size */
    return 0;
}
