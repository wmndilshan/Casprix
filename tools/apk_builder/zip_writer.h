/*
 * Minimal ZIP writer for APK packaging
 *
 * APK files are standard ZIP archives (no deflate required for alignment,
 * but we support both stored and deflated entries).
 * Only implements what is needed for APK packaging:
 *   - Stored (method 0) entries for .so, .dex and pre-aligned resources
 *   - Deflated (method 8) entries for XML and small files
 *   - Central directory and end-of-central-directory record
 */

#ifndef ZIP_WRITER_H
#define ZIP_WRITER_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZIP_MAX_ENTRIES 512

typedef struct {
    char     name[512];
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t local_header_offset;
    uint16_t method;           /* 0=stored, 8=deflated */
    uint16_t last_mod_time;
    uint16_t last_mod_date;
} ZipEntry;

typedef struct {
    FILE*    file;
    ZipEntry entries[ZIP_MAX_ENTRIES];
    int      entry_count;
    long     current_offset;
} ZipWriter;

/* Open a new ZIP file for writing */
ZipWriter* zip_writer_open(const char* path);

/* Add a file from the filesystem */
int zip_writer_add_file(ZipWriter* zw, const char* archive_name,
                         const char* file_path, int compress);

/* Add raw bytes as an entry */
int zip_writer_add_bytes(ZipWriter* zw, const char* archive_name,
                          const void* data, size_t size, int compress);

/* Finalize (write central directory + EOCD) and close */
int zip_writer_close(ZipWriter* zw);

/* CRC-32 utility */
uint32_t zip_crc32(const void* data, size_t size);
uint32_t zip_crc32_update(uint32_t crc, const void* data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* ZIP_WRITER_H */
