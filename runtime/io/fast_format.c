#include "fast_format.h"

#include <limits.h>
#include <string.h>

static const char kDigits2[201] =
    "00010203040506070809"
    "10111213141516171819"
    "20212223242526272829"
    "30313233343536373839"
    "40414243444546474849"
    "50515253545556575859"
    "60616263646566676869"
    "70717273747576777879"
    "80818283848586878889"
    "90919293949596979899";

static const char kHexDigits[17] = "0123456789abcdef";

void cpx_fmt_init(CpxFmtBuffer* b, char* storage, size_t cap) {
    b->data = storage;
    b->cap = cap;
    b->len = 0;
    b->truncated = false;
    if (cap > 0) b->data[0] = '\0';
}

void cpx_fmt_putc(CpxFmtBuffer* b, char c) {
    if (b->len + 1 < b->cap) {
        b->data[b->len++] = c;
        b->data[b->len] = '\0';
    } else {
        b->truncated = true;
    }
}

void cpx_fmt_append_mem(CpxFmtBuffer* b, const char* s, size_t len) {
    if (!s || len == 0) return;
    size_t avail = (b->cap > b->len + 1) ? (b->cap - b->len - 1) : 0;
    size_t n = (len < avail) ? len : avail;
    if (n > 0) {
        memcpy(b->data + b->len, s, n);
        b->len += n;
        b->data[b->len] = '\0';
    }
    if (n != len) b->truncated = true;
}

void cpx_fmt_append_cstr(CpxFmtBuffer* b, const char* s) {
    if (!s) {
        cpx_fmt_append_mem(b, "(null)", 6);
        return;
    }
    cpx_fmt_append_mem(b, s, strlen(s));
}

void cpx_fmt_append_u64(CpxFmtBuffer* b, uint64_t v) {
    char tmp[32];
    size_t p = sizeof(tmp);
    /* Ultra-fast path: use multiplicative inverse for division by 100 
     * where possible, avoiding expensive DIV instructions. */
    while (v >= 100) {
        uint64_t q;
        if (v <= 0xFFFFFFFFU) {
            /* 32-bit optimization */
            q = (uint64_t)(((uint32_t)v * (uint64_t)0x51EB851F) >> 37);
        } else {
            /* 64-bit multiplicative inverse for 100 */
            q = v / 100; /* Fallback: compilers usually optimize this well anyway */
        }
        uint32_t r = (uint32_t)(v - q * 100);
        tmp[--p] = kDigits2[r * 2 + 1];
        tmp[--p] = kDigits2[r * 2];
        v = q;
    }
    if (v < 10) {
        tmp[--p] = (char)('0' + v);
    } else {
        uint32_t r = (uint32_t)v;
        tmp[--p] = kDigits2[r * 2 + 1];
        tmp[--p] = kDigits2[r * 2];
    }
    cpx_fmt_append_mem(b, tmp + p, sizeof(tmp) - p);
}

void cpx_fmt_append_i64(CpxFmtBuffer* b, int64_t v) {
    if (v < 0) {
        cpx_fmt_putc(b, '-');
        if (v == INT64_MIN) {
            cpx_fmt_append_mem(b, "9223372036854775808", 19);
            return;
        }
        cpx_fmt_append_u64(b, (uint64_t)(-v));
    } else {
        cpx_fmt_append_u64(b, (uint64_t)v);
    }
}

void cpx_fmt_append_hex_u64(CpxFmtBuffer* b, uint64_t v, bool with_prefix) {
    char tmp[16];
    size_t p = sizeof(tmp);
    do {
        tmp[--p] = kHexDigits[v & 0xF];
        v >>= 4;
    } while (v);
    if (with_prefix) cpx_fmt_append_mem(b, "0x", 2);
    cpx_fmt_append_mem(b, tmp + p, sizeof(tmp) - p);
}

void cpx_fmt_append_f64_6(CpxFmtBuffer* b, double v) {
    if (v < 0.0) {
        cpx_fmt_putc(b, '-');
        v = -v;
    }
    uint64_t ip = (uint64_t)v;
    double frac = v - (double)ip;
    uint32_t scaled = (uint32_t)(frac * 1000000.0 + 0.5);
    if (scaled >= 1000000u) {
        scaled -= 1000000u;
        ip += 1u;
    }
    cpx_fmt_append_u64(b, ip);
    cpx_fmt_putc(b, '.');
    
    /* Optimized 6-digit fractional part conversion using digit-pair lookup. */
    char frac_buf[6];
    uint32_t d1 = scaled / 100;
    uint32_t r1 = scaled - d1 * 100;
    uint32_t d2 = d1 / 100;
    uint32_t r2 = d1 - d2 * 100;
    
    /* r1 = last 2 digits, r2 = middle 2, d2 = first 2 */
    memcpy(frac_buf + 4, kDigits2 + r1 * 2, 2);
    memcpy(frac_buf + 2, kDigits2 + r2 * 2, 2);
    memcpy(frac_buf + 0, kDigits2 + d2 * 2, 2);
    
    cpx_fmt_append_mem(b, frac_buf, 6);
}

size_t cpx_fmt_vsnprintf(char* out, size_t out_cap, const char* fmt, va_list ap) {
    CpxFmtBuffer b;
    cpx_fmt_init(&b, out, out_cap);
    if (!fmt) return 0;
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') {
            cpx_fmt_putc(&b, *p);
            continue;
        }
        ++p;
        if (*p == '\0') break;
        switch (*p) {
            case '%': cpx_fmt_putc(&b, '%'); break;
            case 's': cpx_fmt_append_cstr(&b, va_arg(ap, const char*)); break;
            case 'd':
            case 'i': cpx_fmt_append_i64(&b, (int64_t)va_arg(ap, int)); break;
            case 'u': cpx_fmt_append_u64(&b, (uint64_t)va_arg(ap, unsigned int)); break;
            case 'l': {
                const char next = *(p + 1);
                if (next == 'l') {
                    const char conv = *(p + 2);
                    if (conv == 'd' || conv == 'i') {
                        cpx_fmt_append_i64(&b, (int64_t)va_arg(ap, long long));
                        p += 2;
                    } else if (conv == 'u') {
                        cpx_fmt_append_u64(&b, (uint64_t)va_arg(ap, unsigned long long));
                        p += 2;
                    } else {
                        cpx_fmt_putc(&b, '%');
                        cpx_fmt_putc(&b, 'l');
                    }
                } else {
                    cpx_fmt_putc(&b, '%');
                    cpx_fmt_putc(&b, 'l');
                }
                break;
            }
            case 'x': cpx_fmt_append_hex_u64(&b, (uint64_t)va_arg(ap, unsigned int), false); break;
            case 'p': cpx_fmt_append_hex_u64(&b, (uint64_t)(uintptr_t)va_arg(ap, void*), true); break;
            default:
                cpx_fmt_putc(&b, '%');
                cpx_fmt_putc(&b, *p);
                break;
        }
    }
    return b.len;
}

size_t cpx_fmt_snprintf(char* out, size_t out_cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t n = cpx_fmt_vsnprintf(out, out_cap, fmt, ap);
    va_end(ap);
    return n;
}
