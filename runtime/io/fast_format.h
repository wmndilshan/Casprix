#ifndef CASPRIX_FAST_FORMAT_H
#define CASPRIX_FAST_FORMAT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char*  data;
    size_t cap;
    size_t len;
    bool   truncated;
} CpxFmtBuffer;

void cpx_fmt_init(CpxFmtBuffer* b, char* storage, size_t cap);
void cpx_fmt_putc(CpxFmtBuffer* b, char c);
void cpx_fmt_append_mem(CpxFmtBuffer* b, const char* s, size_t len);
void cpx_fmt_append_cstr(CpxFmtBuffer* b, const char* s);
void cpx_fmt_append_u64(CpxFmtBuffer* b, uint64_t v);
void cpx_fmt_append_i64(CpxFmtBuffer* b, int64_t v);
void cpx_fmt_append_hex_u64(CpxFmtBuffer* b, uint64_t v, bool with_prefix);
void cpx_fmt_append_f64_6(CpxFmtBuffer* b, double v);

/* Tiny formatter for diagnostics: supports %s %d %i %u %x %p %% */
size_t cpx_fmt_vsnprintf(char* out, size_t out_cap, const char* fmt, va_list ap);
size_t cpx_fmt_snprintf(char* out, size_t out_cap, const char* fmt, ...);

#endif /* CASPRIX_FAST_FORMAT_H */
