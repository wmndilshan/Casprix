#include "../runtime.h"
#include "../../include/casprix/stdlib.h"
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#ifdef _WIN32
    #include <direct.h>
    #include <io.h>
    #define mkdir(path, mode) _mkdir(path)
    #define rmdir(path) _rmdir(path)
    #define access(path, mode) _access(path, mode)
    #define F_OK 0
    #define stat _stat
    #define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
    #define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#else
    #include <unistd.h>
    #include <dirent.h>
#endif

// ============================================================================
// File Handle Management
// ============================================================================

typedef struct {
    FILE* file;
    bool is_open;
    char path[512];
    char mode[8];
} FileHandle;

#define MAX_FILE_HANDLES 256
static FileHandle file_handles[MAX_FILE_HANDLES] = {0};

static int get_free_handle() {
    for (int i = 1; i < MAX_FILE_HANDLES; i++) {
        if (!file_handles[i].is_open) {
            return i;
        }
    }
    return -1;
}

// ============================================================================
// Basic File Operations
// ============================================================================

void* nuwan_fopen(const char* filename, const char* mode) {
    if (!filename || !mode) return NULL;

    FILE* file = fopen(filename, mode);
    if (!file) {
        return NULL;
    }

    int handle = get_free_handle();
    if (handle == -1) {
        fclose(file);
        return NULL;
    }

    file_handles[handle].file = file;
    file_handles[handle].is_open = true;
    strncpy(file_handles[handle].path, filename, sizeof(file_handles[handle].path) - 1);
    strncpy(file_handles[handle].mode, mode, sizeof(file_handles[handle].mode) - 1);

    return (void*)(intptr_t)handle;
}

int nuwan_fclose(void* file) {
    if (!file) return 0;

    int handle = (int)(intptr_t)file;
    if (handle < 1 || handle >= MAX_FILE_HANDLES || !file_handles[handle].is_open) {
        return -1;
    }

    int result = fclose(file_handles[handle].file);
    file_handles[handle].is_open = false;
    file_handles[handle].file = NULL;
    file_handles[handle].path[0] = '\0';
    file_handles[handle].mode[0] = '\0';

    return result;
}

int nuwan_fread(void* buffer, size_t size, size_t count, void* file) {
    if (!file || !buffer) return 0;

    int handle = (int)(intptr_t)file;
    if (handle < 1 || handle >= MAX_FILE_HANDLES || !file_handles[handle].is_open) {
        return 0;
    }

    return (int)fread(buffer, size, count, file_handles[handle].file);
}

int nuwan_fwrite(const void* buffer, size_t size, size_t count, void* file) {
    if (!file || !buffer) return 0;

    int handle = (int)(intptr_t)file;
    if (handle < 1 || handle >= MAX_FILE_HANDLES || !file_handles[handle].is_open) {
        return 0;
    }

    return (int)fwrite(buffer, size, count, file_handles[handle].file);
}

// ============================================================================
// High-Level File Operations
// ============================================================================

int nuwan_file_open_read(const char* filename) {
    void* handle = nuwan_fopen(filename, "r");
    return handle ? (int)(intptr_t)handle : -1;
}

int nuwan_file_open_write(const char* filename) {
    void* handle = nuwan_fopen(filename, "w");
    return handle ? (int)(intptr_t)handle : -1;
}

int nuwan_file_close(int handle) {
    return nuwan_fclose((void*)(intptr_t)handle);
}

char* nuwan_file_read_line(int handle) {
    if (handle < 1 || handle >= MAX_FILE_HANDLES || !file_handles[handle].is_open) {
        return NULL;
    }

    char* line = NULL;

#ifdef _WIN32
    char buffer[4096];
    if (fgets(buffer, sizeof(buffer), file_handles[handle].file)) {
        size_t read_len = strlen(buffer);
        line = (char*)malloc(read_len + 1);
        if (line) {
            memcpy(line, buffer, read_len + 1);
            if (read_len > 0 && line[read_len - 1] == '\n') {
                line[read_len - 1] = '\0';
            }
        }
    }
#else
    size_t len = 0;
    ssize_t read = getline(&line, &len, file_handles[handle].file);

    if (read == -1) {
        free(line);
        return NULL;
    }

    if (read > 0 && line[read - 1] == '\n') {
        line[read - 1] = '\0';
    }
#endif

    return line;
}

int nuwan_file_write(int handle, const char* content) {
    if (!content) return 0;

    void* file = (void*)(intptr_t)handle;
    size_t len = strlen(content);
    return nuwan_fwrite(content, sizeof(char), len, file);
}

bool nuwan_file_exists(const char* filename) {
    if (!filename) return false;
    return access(filename, F_OK) == 0;
}

long nuwan_file_size(const char* filename) {
    if (!filename) return -1;

    struct stat st;
    if (stat(filename, &st) == 0) {
        return (long)st.st_size;
    }
    return -1;
}

int nuwan_file_delete(const char* filename) {
    if (!filename) return -1;
    return remove(filename);
}

int nuwan_file_copy(const char* source, const char* destination) {
    if (!source || !destination) return -1;

    FILE* src = fopen(source, "rb");
    if (!src) return -1;

    FILE* dst = fopen(destination, "wb");
    if (!dst) {
        fclose(src);
        return -1;
    }

    char buffer[4096];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, bytes, dst);
    }

    fclose(src);
    fclose(dst);
    return 0;
}
