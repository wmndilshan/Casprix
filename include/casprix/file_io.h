// Casprix File I/O Library
// Comprehensive file operations with error handling

#ifndef NUWAN_FILE_IO_H
#define NUWAN_FILE_IO_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// File modes
typedef enum {
    NUWAN_FILE_READ = 0,
    NUWAN_FILE_WRITE = 1,
    NUWAN_FILE_APPEND = 2,
    NUWAN_FILE_READ_WRITE = 3
} nuwan_file_mode_t;

// File handle
typedef struct {
    void* handle;           // Platform file handle
    char* path;             // File path
    nuwan_file_mode_t mode; // Open mode
    bool is_open;           // File status
    size_t position;        // Current position
    size_t size;            // File size
} nuwan_file_t;

// Directory entry
typedef struct {
    char* name;
    bool is_directory;
    size_t size;
    int64_t modified_time;
} nuwan_dir_entry_t;

// File operations
nuwan_file_t* nuwan_file_open(const char* path, nuwan_file_mode_t mode);
void nuwan_file_close(nuwan_file_t* file);
bool nuwan_file_is_open(nuwan_file_t* file);

// Read operations
char* nuwan_file_read_all(nuwan_file_t* file);
char* nuwan_file_read_line(nuwan_file_t* file);
size_t nuwan_file_read(nuwan_file_t* file, void* buffer, size_t size);
char* nuwan_file_read_all_text(const char* path);
char** nuwan_file_read_all_lines(const char* path, size_t* line_count);

// Write operations
size_t nuwan_file_write(nuwan_file_t* file, const void* data, size_t size);
bool nuwan_file_write_text(nuwan_file_t* file, const char* text);
bool nuwan_file_write_line(nuwan_file_t* file, const char* line);
bool nuwan_file_write_all_text(const char* path, const char* text);
bool nuwan_file_write_all_lines(const char* path, const char** lines, size_t line_count);

// File positioning
bool nuwan_file_seek(nuwan_file_t* file, int64_t offset, int origin);
size_t nuwan_file_tell(nuwan_file_t* file);
bool nuwan_file_rewind(nuwan_file_t* file);
bool nuwan_file_eof(nuwan_file_t* file);

// File information
bool nuwan_file_exists(const char* path);
size_t nuwan_file_get_size(const char* path);
int64_t nuwan_file_get_modified_time(const char* path);
bool nuwan_file_is_directory(const char* path);

// File operations
bool nuwan_file_copy(const char* source, const char* destination);
bool nuwan_file_move(const char* source, const char* destination);
bool nuwan_file_delete(const char* path);
bool nuwan_file_rename(const char* old_path, const char* new_path);

// Directory operations
bool nuwan_dir_create(const char* path);
bool nuwan_dir_delete(const char* path);
bool nuwan_dir_exists(const char* path);
nuwan_dir_entry_t** nuwan_dir_list(const char* path, size_t* count);
void nuwan_dir_entry_free(nuwan_dir_entry_t* entry);

// Path operations
char* nuwan_path_combine(const char* path1, const char* path2);
char* nuwan_path_get_directory(const char* path);
char* nuwan_path_get_filename(const char* path);
char* nuwan_path_get_extension(const char* path);
char* nuwan_path_get_absolute(const char* path);
bool nuwan_path_is_absolute(const char* path);

// Stream operations (buffered I/O)
typedef struct nuwan_stream nuwan_stream_t;

nuwan_stream_t* nuwan_stream_create(nuwan_file_t* file, size_t buffer_size);
void nuwan_stream_destroy(nuwan_stream_t* stream);
size_t nuwan_stream_read(nuwan_stream_t* stream, void* buffer, size_t size);
size_t nuwan_stream_write(nuwan_stream_t* stream, const void* data, size_t size);
bool nuwan_stream_flush(nuwan_stream_t* stream);

#ifdef __cplusplus
}
#endif

#endif // NUWAN_FILE_IO_H
