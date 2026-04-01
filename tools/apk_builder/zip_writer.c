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
static uint16_t read_u16_buf(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t read_u32_buf(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int read_exact_at(FILE* f, long offset, void* buf, size_t size) {
    if (fseek(f, offset, SEEK_SET) != 0) return 0;
    return fread(buf, 1, size, f) == size;
}

static int read_u16_at(FILE* f, long offset, uint16_t* out) {
    uint8_t b[2];
    if (!read_exact_at(f, offset, b, sizeof(b))) return 0;
    *out = (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
    return 1;
}

static int read_u32_at(FILE* f, long offset, uint32_t* out) {
    uint8_t b[4];
    if (!read_exact_at(f, offset, b, sizeof(b))) return 0;
    *out = (uint32_t)b[0] |
           ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
    return 1;
}

static int read_file_size(FILE* f, long* size_out) {
    long pos = ftell(f);
    if (pos < 0) return 0;
    if (fseek(f, 0, SEEK_END) != 0) return 0;
    long size = ftell(f);
    if (size < 0) return 0;
    if (fseek(f, pos, SEEK_SET) != 0) return 0;
    *size_out = size;
    return 1;
}

static int file_has_suffix_entry_end(long start, uint32_t comp_size, long file_size) {
    long remaining;

    if (start < 0 || file_size < 0) return 0;
    if (start > file_size) return 0;
    remaining = file_size - start;
    return remaining >= 0 && (uint32_t)remaining >= comp_size;
}

static int find_eocd(FILE* f, long file_size, long* eocd_offset,
                     uint16_t* entry_count, uint32_t* cd_size,
                     uint32_t* cd_offset, uint16_t* comment_len) {
    const uint32_t sig = 0x06054b50u;
    const long min_eocd = 22;
    const long max_comment = 65535;
    long scan_start;
    long scan_size;
    uint8_t tail[22 + 65535];

    if (file_size < min_eocd) return 0;
    scan_start = file_size - (min_eocd + max_comment);
    if (scan_start < 0) scan_start = 0;
    scan_size = file_size - scan_start;
    if (scan_size > (long)sizeof(tail)) return 0;
    if (!read_exact_at(f, scan_start, tail, (size_t)scan_size)) return 0;

    for (long i = scan_size - min_eocd; i >= 0; --i) {
        uint32_t maybe_sig = (uint32_t)tail[i] |
                             ((uint32_t)tail[i + 1] << 8) |
                             ((uint32_t)tail[i + 2] << 16) |
                             ((uint32_t)tail[i + 3] << 24);
        if (maybe_sig != sig) continue;

        uint16_t disk_no = (uint16_t)(tail[i + 4] | ((uint16_t)tail[i + 5] << 8));
        uint16_t cd_disk = (uint16_t)(tail[i + 6] | ((uint16_t)tail[i + 7] << 8));
        uint16_t entries_on_disk = (uint16_t)(tail[i + 8] | ((uint16_t)tail[i + 9] << 8));
        uint16_t total_entries   = (uint16_t)(tail[i + 10] | ((uint16_t)tail[i + 11] << 8));
        uint32_t size_cd = (uint32_t)tail[i + 12] |
                           ((uint32_t)tail[i + 13] << 8) |
                           ((uint32_t)tail[i + 14] << 16) |
                           ((uint32_t)tail[i + 15] << 24);
        uint32_t off_cd = (uint32_t)tail[i + 16] |
                          ((uint32_t)tail[i + 17] << 8) |
                          ((uint32_t)tail[i + 18] << 16) |
                          ((uint32_t)tail[i + 19] << 24);
        uint16_t c_len = (uint16_t)(tail[i + 20] | ((uint16_t)tail[i + 21] << 8));

        if (disk_no != 0 || cd_disk != 0 || entries_on_disk != total_entries) return 0;
        if ((long)i + min_eocd + (long)c_len != scan_size) return 0;
        if ((long)off_cd + (long)size_cd > file_size) return 0;

        *eocd_offset = scan_start + i;
        *entry_count = total_entries;
        *cd_size = size_cd;
        *cd_offset = off_cd;
        *comment_len = c_len;
        return 1;
    }

    return 0;
}

#ifdef HAVE_ZLIB
static CpxZipError verify_deflated_entry(FILE* f, long data_offset,
                                        uint32_t comp_size, uint32_t uncomp_size,
                                        uint32_t expected_crc) {
    CpxZipError err = CPX_ZIP_OK;
    uint8_t* comp_buf = NULL;
    uint8_t* out_buf = NULL;
    z_stream zs;
    int zret;

    memset(&zs, 0, sizeof(zs));

    if (uncomp_size == 0) {
        if (comp_size != 0) return CPX_ZIP_ERR_CRC_MISMATCH;
        return expected_crc == 0 ? CPX_ZIP_OK : CPX_ZIP_ERR_CRC_MISMATCH;
    }

    if (comp_size > 0) {
        comp_buf = (uint8_t*)malloc(comp_size);
        if (!comp_buf) return CPX_ZIP_ERR_IO;
        if (!read_exact_at(f, data_offset, comp_buf, comp_size)) {
            free(comp_buf);
            return CPX_ZIP_ERR_IO;
        }
    }

    out_buf = (uint8_t*)malloc(uncomp_size ? uncomp_size : 1);
    if (!out_buf) {
        free(comp_buf);
        return CPX_ZIP_ERR_IO;
    }

    zs.next_in = comp_buf;
    zs.avail_in = comp_size;
    zs.next_out = out_buf;
    zs.avail_out = uncomp_size;

    zret = inflateInit2(&zs, -MAX_WBITS);
    if (zret != Z_OK) {
        free(comp_buf);
        free(out_buf);
        return CPX_ZIP_ERR_IO;
    }

    zret = inflate(&zs, Z_FINISH);
    if (zret != Z_STREAM_END || zs.total_out != uncomp_size || zs.total_in != comp_size) {
        inflateEnd(&zs);
        free(comp_buf);
        free(out_buf);
        return CPX_ZIP_ERR_CRC_MISMATCH;
    }

    err = (zip_crc32(out_buf, uncomp_size) == expected_crc) ? CPX_ZIP_OK
                                                             : CPX_ZIP_ERR_CRC_MISMATCH;
    inflateEnd(&zs);
    free(comp_buf);
    free(out_buf);
    return err;
}
#endif

static CpxZipError verify_stored_entry(FILE* f, long data_offset,
                                       uint32_t comp_size, uint32_t expected_crc) {
    uint8_t* buf = NULL;
    CpxZipError err = CPX_ZIP_OK;

    if (comp_size == 0) {
        return expected_crc == 0 ? CPX_ZIP_OK : CPX_ZIP_ERR_CRC_MISMATCH;
    }

    buf = (uint8_t*)malloc(comp_size);
    if (!buf) return CPX_ZIP_ERR_IO;
    if (!read_exact_at(f, data_offset, buf, comp_size)) {
        free(buf);
        return CPX_ZIP_ERR_IO;
    }

    err = (zip_crc32(buf, comp_size) == expected_crc) ? CPX_ZIP_OK : CPX_ZIP_ERR_CRC_MISMATCH;
    free(buf);
    return err;
}

/* ========================================================================
 * ZipWriter API
 * ======================================================================== */

ZipWriter* zip_writer_open(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return NULL;
    ZipWriter* zw = (ZipWriter*)calloc(1, sizeof(ZipWriter));
    if (!zw) {
        fclose(f);
        return NULL;
    }
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

static int zip_read_all(const char* path, uint8_t** out_data, size_t* out_size) {
    FILE* f;
    uint8_t* data;
    long size;

    if (!path || !out_data || !out_size) return -1;
    *out_data = NULL;
    *out_size = 0;

    f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    data = (uint8_t*)malloc((size_t)(size > 0 ? size : 1));
    if (!data) {
        fclose(f);
        return -1;
    }

    if (size > 0 && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return -1;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return 0;
}

static CpxZipError zip_verify_payload_crc(const uint8_t* payload,
                                          size_t file_size,
                                          size_t payload_offset,
                                          uint16_t method,
                                          uint32_t expected_crc,
                                          uint32_t compressed_size,
                                          uint32_t uncompressed_size) {
    if (payload_offset + compressed_size > file_size) {
        return CPX_ZIP_ERR_ENTRY_OUT_OF_BOUNDS;
    }

    if (method == 0) {
        if (zip_crc32(payload + payload_offset, compressed_size) != expected_crc) {
            return CPX_ZIP_ERR_CRC_MISMATCH;
        }
        return CPX_ZIP_OK;
    }

#ifdef HAVE_ZLIB
    if (method == 8) {
        z_stream stream;
        uint8_t* out;
        int zerr;
        uint32_t crc;

        out = (uint8_t*)malloc((size_t)(uncompressed_size > 0 ? uncompressed_size : 1));
        if (!out) return CPX_ZIP_ERR_IO;

        memset(&stream, 0, sizeof(stream));
        stream.next_in = (Bytef*)(payload + payload_offset);
        stream.avail_in = compressed_size;
        stream.next_out = out;
        stream.avail_out = uncompressed_size;

        zerr = inflateInit2(&stream, -MAX_WBITS);
        if (zerr != Z_OK) {
            free(out);
            return CPX_ZIP_ERR_IO;
        }

        zerr = inflate(&stream, Z_FINISH);
        if (zerr != Z_STREAM_END || stream.total_out != uncompressed_size) {
            inflateEnd(&stream);
            free(out);
            return CPX_ZIP_ERR_IO;
        }

        crc = zip_crc32(out, uncompressed_size);
        inflateEnd(&stream);
        free(out);

        return crc == expected_crc ? CPX_ZIP_OK : CPX_ZIP_ERR_CRC_MISMATCH;
    }
#endif

    return CPX_ZIP_ERR_UNSUPPORTED_METHOD;
}

CpxZipError cpx_zip_verify(const char* apk_path) {
    uint8_t* data = NULL;
    size_t size = 0;
    size_t eocd_pos = SIZE_MAX;
    size_t cd_pos;
    size_t local_count = 0;
    uint16_t total_entries;
    uint32_t cd_size;
    uint32_t cd_offset;

    if (zip_read_all(apk_path, &data, &size) != 0) {
        return CPX_ZIP_ERR_OPEN_FAILED;
    }
    if (size < 22) {
        free(data);
        return CPX_ZIP_ERR_BAD_EOCD;
    }

    for (size_t i = size - 22;; --i) {
        if (read_u32_buf(data + i) == 0x06054b50u) {
            eocd_pos = i;
            break;
        }
        if (i == 0) break;
    }
    if (eocd_pos == SIZE_MAX) {
        free(data);
        return CPX_ZIP_ERR_BAD_EOCD;
    }

    total_entries = read_u16_buf(data + eocd_pos + 10);
    cd_size = read_u32_buf(data + eocd_pos + 12);
    cd_offset = read_u32_buf(data + eocd_pos + 16);
    if ((size_t)cd_offset + (size_t)cd_size > size) {
        free(data);
        return CPX_ZIP_ERR_BAD_CENTRAL_DIRECTORY;
    }

    {
        size_t pos = 0;
        while (pos < cd_offset) {
            uint32_t sig;
            uint16_t name_len;
            uint16_t extra_len;
            uint32_t comp_size;
            size_t payload_offset;

            if (pos + 30 > cd_offset) {
                free(data);
                return CPX_ZIP_ERR_BAD_LOCAL_HEADER;
            }
            sig = read_u32_buf(data + pos);
            if (sig != 0x04034b50u) {
                free(data);
                return CPX_ZIP_ERR_BAD_LOCAL_HEADER;
            }

            comp_size = read_u32_buf(data + pos + 18);
            name_len = read_u16_buf(data + pos + 26);
            extra_len = read_u16_buf(data + pos + 28);
            payload_offset = pos + 30u + name_len + extra_len;
            if (payload_offset + comp_size > size || payload_offset > cd_offset) {
                free(data);
                return CPX_ZIP_ERR_ENTRY_OUT_OF_BOUNDS;
            }

            local_count++;
            pos = payload_offset + comp_size;
        }
    }

    cd_pos = cd_offset;
    for (uint16_t entry = 0; entry < total_entries; ++entry) {
        uint16_t method;
        uint16_t name_len;
        uint16_t extra_len;
        uint16_t comment_len;
        uint32_t crc;
        uint32_t comp_size;
        uint32_t uncomp_size;
        uint32_t local_off;
        uint16_t local_name_len;
        uint16_t local_extra_len;
        size_t payload_offset;
        CpxZipError verify_err;

        if (cd_pos + 46 > size || read_u32_buf(data + cd_pos) != 0x02014b50u) {
            free(data);
            return CPX_ZIP_ERR_BAD_CENTRAL_DIRECTORY;
        }

        method = read_u16_buf(data + cd_pos + 10);
        crc = read_u32_buf(data + cd_pos + 16);
        comp_size = read_u32_buf(data + cd_pos + 20);
        uncomp_size = read_u32_buf(data + cd_pos + 24);
        name_len = read_u16_buf(data + cd_pos + 28);
        extra_len = read_u16_buf(data + cd_pos + 30);
        comment_len = read_u16_buf(data + cd_pos + 32);
        local_off = read_u32_buf(data + cd_pos + 42);

        if (cd_pos + 46u + name_len + extra_len + comment_len > size) {
            free(data);
            return CPX_ZIP_ERR_BAD_CENTRAL_DIRECTORY;
        }
        if ((size_t)local_off + 30 > cd_offset ||
            read_u32_buf(data + local_off) != 0x04034b50u) {
            free(data);
            return CPX_ZIP_ERR_BAD_LOCAL_HEADER;
        }

        local_name_len = read_u16_buf(data + local_off + 26);
        local_extra_len = read_u16_buf(data + local_off + 28);
        payload_offset = (size_t)local_off + 30u + local_name_len + local_extra_len;

        if (payload_offset + comp_size > size) {
            free(data);
            return CPX_ZIP_ERR_ENTRY_OUT_OF_BOUNDS;
        }
        if (read_u32_buf(data + local_off + 14) != crc ||
            read_u32_buf(data + local_off + 18) != comp_size ||
            read_u32_buf(data + local_off + 22) != uncomp_size) {
            free(data);
            return CPX_ZIP_ERR_BAD_LOCAL_HEADER;
        }

        verify_err = zip_verify_payload_crc(data, size, payload_offset, method,
                                            crc, comp_size, uncomp_size);
        if (verify_err != CPX_ZIP_OK) {
            free(data);
            return verify_err;
        }

        cd_pos += 46u + name_len + extra_len + comment_len;
    }

    free(data);
    if (local_count != total_entries) {
        return CPX_ZIP_ERR_BAD_CENTRAL_DIRECTORY;
    }
    return CPX_ZIP_OK;
}

const char* cpx_zip_error_string(CpxZipError err) {
    switch (err) {
        case CPX_ZIP_OK:                    return "OK";
        case CPX_ZIP_ERR_OPEN_FAILED:       return "Could not open ZIP file";
        case CPX_ZIP_ERR_BAD_EOCD:          return "Invalid ZIP end-of-central-directory";
        case CPX_ZIP_ERR_BAD_CENTRAL_DIRECTORY:return "Invalid ZIP central directory";
        case CPX_ZIP_ERR_BAD_LOCAL_HEADER:  return "Invalid ZIP local file header";
        case CPX_ZIP_ERR_ENTRY_OUT_OF_BOUNDS:return "ZIP entry extends past end of file";
        case CPX_ZIP_ERR_CRC_MISMATCH:      return "ZIP CRC mismatch";
        case CPX_ZIP_ERR_UNSUPPORTED_METHOD:return "Unsupported ZIP compression method";
        case CPX_ZIP_ERR_IO:                return "ZIP I/O or inflate error";
        default:                            return "Unknown ZIP error";
    }
}
