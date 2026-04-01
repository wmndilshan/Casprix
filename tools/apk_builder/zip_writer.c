/*
 * zip_writer.c — Minimal ZIP implementation for APK packaging
 *
 * Supports:
 *   - Stored  (method 0): used for .so, .dex, resources.arsc
 *   - Deflated (method 8): used for XML / small text files (via zlib if available)
 *   - Standard ZIP64 not required for typical APKs < 4 GB
 */

#include "zip_writer.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Try to use zlib for deflate; fall back to stored if not available */
#ifdef HAVE_ZLIB
#  include <zlib.h>
#else
   /* Without zlib, always use stored mode */
#  define compress2(dest, destLen, src, srcLen, level) \
       (*(destLen) = (srcLen), memcpy(dest, src, srcLen), Z_OK)
#  define Z_OK 0
#  define Z_DEFAULT_COMPRESSION 6
#endif

/* ========================================================================
 * CRC-32
 * ======================================================================== */

static uint32_t crc32_table[256];
static int crc32_init_done = 0;

static void crc32_init(void) {
    if (crc32_init_done) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_init_done = 1;
}

uint32_t zip_crc32_update(uint32_t crc, const void* data, size_t size) {
    crc32_init();
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    while (size--) crc = crc32_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t zip_crc32(const void* data, size_t size) {
    return zip_crc32_update(0, data, size);
}

/* ========================================================================
 * DOS Date/Time helpers
 * ======================================================================== */

static void get_dos_datetime(uint16_t* time_out, uint16_t* date_out) {
    time_t t   = time(NULL);
    struct tm* tm = localtime(&t);
    *time_out = (uint16_t)(((tm->tm_hour) << 11) |
                            ((tm->tm_min)  << 5)  |
                            (tm->tm_sec  / 2));
    *date_out = (uint16_t)((((tm->tm_year - 80)) << 9) |
                             ((tm->tm_mon + 1)     << 5)  |
                              (tm->tm_mday));
}

/* ========================================================================
 * Write helpers
 * ======================================================================== */

static void write_u16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }
static void write_u32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }

/* ========================================================================
 * ZipWriter API
 * ======================================================================== */

ZipWriter* zip_writer_open(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return NULL;
    ZipWriter* zw = (ZipWriter*)calloc(1, sizeof(ZipWriter));
    zw->file = f;
    zw->current_offset = 0;
    zw->entry_count = 0;
    return zw;
}

static int _add_entry(ZipWriter* zw, const char* archive_name,
                       const uint8_t* data, size_t uncomp_size, int compress) {
    if (zw->entry_count >= ZIP_MAX_ENTRIES) return -1;

    uint16_t t, d;
    get_dos_datetime(&t, &d);

    uint32_t crc       = zip_crc32(data, uncomp_size);
    uint16_t method    = 0;
    size_t   comp_size = uncomp_size;
    uint8_t* comp_data = (uint8_t*)data;
    uint8_t* tmp       = NULL;

#ifdef HAVE_ZLIB
    if (compress && uncomp_size > 0) {
        uLongf bound = compressBound((uLong)uncomp_size);
        tmp = (uint8_t*)malloc(bound);
        uLongf actual = bound;
        if (compress2(tmp, &actual, data, (uLong)uncomp_size,
                      Z_DEFAULT_COMPRESSION) == Z_OK && actual < uncomp_size) {
            comp_data = tmp;
            comp_size = (size_t)actual;
            method    = 8;
        }
    }
#else
    (void)compress;
#endif

    uint16_t name_len = (uint16_t)strlen(archive_name);

    /* Local file header */
    long lhdr_off = zw->current_offset;
    write_u32(zw->file, 0x04034b50u);  /* Signature */
    write_u16(zw->file, 20);           /* Version needed (2.0) */
    write_u16(zw->file, 0);            /* Flags */
    write_u16(zw->file, method);
    write_u16(zw->file, t);
    write_u16(zw->file, d);
    write_u32(zw->file, crc);
    write_u32(zw->file, (uint32_t)comp_size);
    write_u32(zw->file, (uint32_t)uncomp_size);
    write_u16(zw->file, name_len);
    write_u16(zw->file, 0);            /* Extra field length */
    fwrite(archive_name, 1, name_len, zw->file);
    fwrite(comp_data, 1, comp_size, zw->file);

    zw->current_offset = ftell(zw->file);

    /* Record central directory entry info */
    ZipEntry* e = &zw->entries[zw->entry_count++];
    snprintf(e->name, sizeof(e->name), "%s", archive_name);
    e->crc32                = crc;
    e->compressed_size      = (uint32_t)comp_size;
    e->uncompressed_size    = (uint32_t)uncomp_size;
    e->local_header_offset  = (uint32_t)lhdr_off;
    e->method               = method;
    e->last_mod_time        = t;
    e->last_mod_date        = d;

    if (tmp) free(tmp);
    return 0;
}

int zip_writer_add_bytes(ZipWriter* zw, const char* archive_name,
                          const void* data, size_t size, int compress) {
    return _add_entry(zw, archive_name, (const uint8_t*)data, size, compress);
}

int zip_writer_add_file(ZipWriter* zw, const char* archive_name,
                         const char* file_path, int compress) {
    FILE* f = fopen(file_path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    size_t read_bytes = fread(buf, 1, (size_t)sz, f);
    (void)read_bytes;
    fclose(f);

    int r = _add_entry(zw, archive_name, buf, (size_t)sz, compress);
    free(buf);
    return r;
}

int zip_writer_close(ZipWriter* zw) {
    if (!zw) return -1;

    long cd_start = ftell(zw->file);

    /* Central directory entries */
    for (int i = 0; i < zw->entry_count; i++) {
        ZipEntry* e       = &zw->entries[i];
        uint16_t name_len = (uint16_t)strlen(e->name);

        write_u32(zw->file, 0x02014b50u);  /* CD signature */
        write_u16(zw->file, 20);           /* Version made by */
        write_u16(zw->file, 20);           /* Version needed */
        write_u16(zw->file, 0);            /* Flags */
        write_u16(zw->file, e->method);
        write_u16(zw->file, e->last_mod_time);
        write_u16(zw->file, e->last_mod_date);
        write_u32(zw->file, e->crc32);
        write_u32(zw->file, e->compressed_size);
        write_u32(zw->file, e->uncompressed_size);
        write_u16(zw->file, name_len);
        write_u16(zw->file, 0);            /* Extra length */
        write_u16(zw->file, 0);            /* Comment length */
        write_u16(zw->file, 0);            /* Disk number start */
        write_u16(zw->file, 0);            /* Internal attrs */
        write_u32(zw->file, 0);            /* External attrs */
        write_u32(zw->file, e->local_header_offset);
        fwrite(e->name, 1, name_len, zw->file);
    }

    long cd_size = ftell(zw->file) - cd_start;

    /* End of central directory record */
    write_u32(zw->file, 0x06054b50u);      /* EOCD signature */
    write_u16(zw->file, 0);               /* Disk number */
    write_u16(zw->file, 0);               /* Start disk */
    write_u16(zw->file, (uint16_t)zw->entry_count);
    write_u16(zw->file, (uint16_t)zw->entry_count);
    write_u32(zw->file, (uint32_t)cd_size);
    write_u32(zw->file, (uint32_t)cd_start);
    write_u16(zw->file, 0);               /* Comment length */

    fclose(zw->file);
    free(zw);
    return 0;
}
