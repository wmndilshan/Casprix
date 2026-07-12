#include "dataset_stream.h"

#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <fileapi.h>
#include <handleapi.h>
#include <memoryapi.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static bool cpx_add_overflow(size_t a, size_t b, size_t* out) {
    if (a > SIZE_MAX - b) return true;
    *out = a + b;
    return false;
}

int cpx_dataset_map_readonly(const char* path, CpxMappedDataset* out) {
    if (!path || !out) return -1;
    memset(out, 0, sizeof(*out));
#if defined(_WIN32) || defined(_WIN64)
    HANDLE fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(fh, &sz) || sz.QuadPart < 0) {
        CloseHandle(fh);
        return -1;
    }
    HANDLE mh = CreateFileMappingA(fh, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mh) {
        CloseHandle(fh);
        return -1;
    }
    const uint8_t* base = (const uint8_t*)MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);
    if (!base) {
        CloseHandle(mh);
        CloseHandle(fh);
        return -1;
    }
    out->base = base;
    out->len = (size_t)sz.QuadPart;
    out->file_handle = fh;
    out->mapping_handle = mh;
#else
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        close(fd);
        return -1;
    }
    const uint8_t* base = (const uint8_t*)mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return -1;
    }
    out->base = base;
    out->len = (size_t)st.st_size;
    out->fd = fd;
#endif
    out->cursor = 0;
    return 0;
}

void cpx_dataset_unmap(CpxMappedDataset* ds) {
    if (!ds || !ds->base) return;
#if defined(_WIN32) || defined(_WIN64)
    UnmapViewOfFile(ds->base);
    if (ds->mapping_handle) CloseHandle(ds->mapping_handle);
    if (ds->file_handle && ds->file_handle != INVALID_HANDLE_VALUE) CloseHandle(ds->file_handle);
#else
    munmap((void*)ds->base, ds->len);
    if (ds->fd >= 0) close(ds->fd);
#endif
    memset(ds, 0, sizeof(*ds));
}

bool cpx_dataset_view(const CpxMappedDataset* ds, size_t offset, size_t nbytes, const void** out_ptr) {
    if (!ds || !ds->base || !out_ptr) return false;
    size_t end = 0;
    if (cpx_add_overflow(offset, nbytes, &end)) return false;
    if (end > ds->len) return false;
    *out_ptr = ds->base + offset;
    return true;
}

bool cpx_dataset_copy_to_arena(CpxMappedDataset* ds, size_t offset, size_t nbytes,
                               CpxArena* arena, size_t align, void** out_ptr) {
    if (!ds || !arena || !out_ptr || align == 0 || (align & (align - 1u)) != 0u) return false;
    const void* src = NULL;
    if (!cpx_dataset_view(ds, offset, nbytes, &src)) return false;
    void* dst = cpx_arena_alloc(arena, nbytes, align);
    if (!dst) return false;
    memcpy(dst, src, nbytes);
    *out_ptr = dst;
    return true;
}

int cpx_dataset_advise(CpxMappedDataset* ds, size_t offset, size_t nbytes) {
    if (!ds || !ds->base) return -1;
    if (offset + nbytes > ds->len) return -1;
#if defined(_WIN32) || defined(_WIN64)
    WIN32_MEMORY_RANGE_ENTRY entry = {.VirtualAddress = (void*)(ds->base + offset), .NumberOfBytes = nbytes};
    if (PrefetchVirtualMemory(GetCurrentProcess(), 1, &entry, 0)) return 0;
    return -1;
#else
    return posix_madvise((void*)(ds->base + offset), nbytes, POSIX_MADV_SEQUENTIAL);
#endif
}
