/**
 * Casperix Runtime - Async File I/O Implementation
 */

#include "file_async.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <fcntl.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#endif

/* On Windows, store HANDLE in fd field via intptr_t to avoid truncation */
#ifdef _WIN32
#define FD_TO_HANDLE(fd) ((HANDLE)(intptr_t)(fd))
#define HANDLE_TO_FD(h)  ((int)(intptr_t)(h))
#endif

Future* async_file_open(EventLoop* loop, const char* path, int flags) {
    Future* future = future_create();
    
    AsyncFile* file = (AsyncFile*)calloc(1, sizeof(AsyncFile));
    file->path = strdup(path);
    file->event_loop = loop;
    
#ifdef _WIN32
    DWORD access = GENERIC_READ;
    DWORD creation = OPEN_EXISTING;
    
    if (flags & O_WRONLY) access = GENERIC_WRITE;
    if (flags & O_RDWR) access = GENERIC_READ | GENERIC_WRITE;
    if (flags & O_CREAT) creation = CREATE_ALWAYS;
    
    HANDLE handle = CreateFileA(
        path,
        access,
        FILE_SHARE_READ,
        NULL,
        creation,
        FILE_FLAG_OVERLAPPED,  // Async I/O
        NULL
    );
    
    if (handle != INVALID_HANDLE_VALUE) {
        file->fd = HANDLE_TO_FD(handle);
        file->is_open = true;
        file->is_readable = (flags & O_RDONLY) || (flags & O_RDWR);
        file->is_writable = (flags & O_WRONLY) || (flags & O_RDWR);
        future_complete(future, file);
    } else {
        free(file->path);
        free(file);
        future_fail(future, (void*)(intptr_t)GetLastError());
    }
#else
    int fd = open(path, flags | O_NONBLOCK, 0644);
    if (fd >= 0) {
        file->fd = fd;
        file->is_open = true;
        file->is_readable = (flags & O_RDONLY) || (flags & O_RDWR);
        file->is_writable = (flags & O_WRONLY) || (flags & O_RDWR);
        future_complete(future, file);
    } else {
        free(file->path);
        free(file);
        future_fail(future, (void*)(intptr_t)errno);
    }
#endif
    
    return future;
}

void async_file_close(AsyncFile* file) {
    if (!file || !file->is_open) return;
    
#ifdef _WIN32
    CloseHandle(FD_TO_HANDLE(file->fd));
#else
    close(file->fd);
#endif
    
    file->is_open = false;
    free(file->path);
    free(file);
}

Future* async_file_read(AsyncFile* file, void* buffer, size_t size) {
    Future* future = future_create();
    
    if (!file || !file->is_readable) {
        future_fail(future, "File not readable");
        return future;
    }
    
#ifdef _WIN32
    OVERLAPPED overlapped = {0};
    DWORD bytes_read;
    
    if (ReadFile(FD_TO_HANDLE(file->fd), buffer, (DWORD)size, &bytes_read, &overlapped)) {
        future_complete(future, (void*)(intptr_t)bytes_read);
    } else if (GetLastError() == ERROR_IO_PENDING) {
        // Async operation pending - will complete later
        future_complete(future, buffer);
    } else {
        future_fail(future, (void*)(intptr_t)GetLastError());
    }
#else
    ssize_t bytes = read(file->fd, buffer, size);
    if (bytes >= 0) {
        future_complete(future, (void*)(intptr_t)bytes);
    } else {
        future_fail(future, (void*)(intptr_t)errno);
    }
#endif
    
    return future;
}

Future* async_file_write(AsyncFile* file, const void* data, size_t size) {
    Future* future = future_create();
    
    if (!file || !file->is_writable) {
        future_fail(future, "File not writable");
        return future;
    }
    
#ifdef _WIN32
    OVERLAPPED overlapped = {0};
    DWORD bytes_written;
    
    if (WriteFile(FD_TO_HANDLE(file->fd), data, (DWORD)size, &bytes_written, &overlapped)) {
        future_complete(future, (void*)(intptr_t)bytes_written);
    } else if (GetLastError() == ERROR_IO_PENDING) {
        future_complete(future, (void*)data);
    } else {
        future_fail(future, (void*)(intptr_t)GetLastError());
    }
#else
    ssize_t bytes = write(file->fd, data, size);
    if (bytes >= 0) {
        future_complete(future, (void*)(intptr_t)bytes);
    } else {
        future_fail(future, (void*)(intptr_t)errno);
    }
#endif
    
    return future;
}

Future* async_file_read_all(EventLoop* loop, const char* path) {
    Future* future = future_create();
    
    // Get file size first
#ifdef _WIN32
    HANDLE handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        future_fail(future, (void*)(intptr_t)GetLastError());
        return future;
    }
    
    LARGE_INTEGER file_size;
    GetFileSizeEx(handle, &file_size);
    
    char* buffer = (char*)malloc(file_size.QuadPart + 1);
    DWORD bytes_read;
    ReadFile(handle, buffer, (DWORD)file_size.QuadPart, &bytes_read, NULL);
    buffer[bytes_read] = '\0';
    
    CloseHandle(handle);
    future_complete(future, buffer);
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        future_fail(future, (void*)(intptr_t)errno);
        return future;
    }
    
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        future_fail(future, (void*)(intptr_t)errno);
        return future;
    }
    
    char* buffer = (char*)malloc(st.st_size + 1);
    ssize_t bytes = read(fd, buffer, st.st_size);
    buffer[bytes] = '\0';
    
    close(fd);
    future_complete(future, buffer);
#endif
    
    return future;
}

Future* async_file_write_all(EventLoop* loop, const char* path, const void* data, size_t size) {
    Future* future = future_create();
    
#ifdef _WIN32
    HANDLE handle = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        future_fail(future, (void*)(intptr_t)GetLastError());
        return future;
    }
    
    DWORD bytes_written;
    WriteFile(handle, data, (DWORD)size, &bytes_written, NULL);
    CloseHandle(handle);
    
    future_complete(future, (void*)(intptr_t)bytes_written);
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        future_fail(future, (void*)(intptr_t)errno);
        return future;
    }
    
    ssize_t bytes = write(fd, data, size);
    close(fd);
    
    if (bytes >= 0) {
        future_complete(future, (void*)(intptr_t)bytes);
    } else {
        future_fail(future, (void*)(intptr_t)errno);
    }
#endif
    
    return future;
}

MemoryMappedFile* mmap_file(const char* path, bool writable) {
    MemoryMappedFile* mmap = (MemoryMappedFile*)calloc(1, sizeof(MemoryMappedFile));
    
#ifdef _WIN32
    DWORD access = writable ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    DWORD protect = writable ? PAGE_READWRITE : PAGE_READONLY;
    DWORD map_access = writable ? FILE_MAP_WRITE : FILE_MAP_READ;
    
    HANDLE file_handle = CreateFileA(path, access, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file_handle == INVALID_HANDLE_VALUE) {
        free(mmap);
        return NULL;
    }
    
    LARGE_INTEGER file_size;
    GetFileSizeEx(file_handle, &file_size);
    
    HANDLE map_handle = CreateFileMappingA(file_handle, NULL, protect, 0, 0, NULL);
    if (!map_handle) {
        CloseHandle(file_handle);
        free(mmap);
        return NULL;
    }
    
    void* data = MapViewOfFile(map_handle, map_access, 0, 0, 0);
    if (!data) {
        CloseHandle(map_handle);
        CloseHandle(file_handle);
        free(mmap);
        return NULL;
    }
    
    mmap->data = data;
    mmap->size = file_size.QuadPart;
    mmap->handle = map_handle;
    
    CloseHandle(file_handle);
#else
    int fd = open(path, writable ? O_RDWR : O_RDONLY);
    if (fd < 0) {
        free(mmap);
        return NULL;
    }
    
    struct stat st;
    fstat(fd, &st);
    
    int prot = writable ? (PROT_READ | PROT_WRITE) : PROT_READ;
    void* data = mmap(NULL, st.st_size, prot, MAP_PRIVATE, fd, 0);
    
    close(fd);
    
    if (data == MAP_FAILED) {
        free(mmap);
        return NULL;
    }
    
    mmap->data = data;
    mmap->size = st.st_size;
    mmap->handle = NULL;
#endif
    
    return mmap;
}

void munmap_file(MemoryMappedFile* mmap) {
    if (!mmap) return;
    
#ifdef _WIN32
    UnmapViewOfFile(mmap->data);
    CloseHandle(mmap->handle);
#else
    munmap(mmap->data, mmap->size);
#endif
    
    free(mmap);
}
