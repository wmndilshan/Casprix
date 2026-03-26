/**
 * Casperix Runtime - Async File I/O
 * Non-blocking file operations using event loop
 */

#ifndef CASPERIX_FILE_ASYNC_H
#define CASPERIX_FILE_ASYNC_H

#include "../async/future.h"
#include "event_loop.h"
#include <stdint.h>
#include <stdbool.h>

// File handle
typedef struct AsyncFile {
    int fd;
    char* path;
    bool is_open;
    bool is_readable;
    bool is_writable;
    EventLoop* event_loop;
} AsyncFile;

// Open file asynchronously
Future* async_file_open(EventLoop* loop, const char* path, int flags);

// Close file
void async_file_close(AsyncFile* file);

// Read from file asynchronously
Future* async_file_read(AsyncFile* file, void* buffer, size_t size);

// Write to file asynchronously  
Future* async_file_write(AsyncFile* file, const void* data, size_t size);

// Read entire file into buffer
Future* async_file_read_all(EventLoop* loop, const char* path);

// Write entire buffer to file
Future* async_file_write_all(EventLoop* loop, const char* path, const void* data, size_t size);

// Memory-mapped file
typedef struct {
    void* data;
    size_t size;
    void* handle;  // Platform-specific
} MemoryMappedFile;

// Memory map a file (for large files)
MemoryMappedFile* mmap_file(const char* path, bool writable);

// Unmap file
void munmap_file(MemoryMappedFile* mmap);

#endif // CASPERIX_FILE_ASYNC_H
