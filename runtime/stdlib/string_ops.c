/*
 * Casprix Runtime - String Operations (pure-C implementation)
 * ASM-optimised variants are declared as extern but fall back to these
 * when no assembly object is linked.
 */

#ifndef _WIN32
#  define _POSIX_C_SOURCE 200809L
#endif

#include "../../include/casprix/string_ops.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>

/* ── ASM kernel fallbacks (pure-C) ─────────────────────────────────────── */

size_t nuwan_strlen_asm(const char* s)        { return s ? strlen(s) : 0; }
void*  nuwan_memcopy(void* d, const void* s, size_t n) { return memcpy(d, s, n); }
void*  nuwan_memset_fast(void* p, int v, size_t n)     { return memset(p, v, n); }
int    nuwan_strcmp_asm(const char* a, const char* b)  { return strcmp(a, b); }

/* FNV-1a 64-bit hash */
uint64_t nuwan_string_hash(const char* s) {
    uint64_t h = 14695981039346656037ULL;
    if (!s) return h;
    while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ULL; }
    return h;
}

/* ── String API ─────────────────────────────────────────────────────────── */

char* nuwan_string_concat(const char* a, const char* b) {
    size_t la = a ? strlen(a) : 0, lb = b ? strlen(b) : 0;
    char* r = (char*)malloc(la + lb + 1);
    if (!r) return NULL;
    if (la) memcpy(r, a, la);
    if (lb) memcpy(r + la, b, lb);
    r[la + lb] = '\0';
    return r;
}

char* nuwan_string_concat_many(const char** strings, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++) if (strings[i]) total += strlen(strings[i]);
    char* r = (char*)malloc(total + 1);
    if (!r) return NULL;
    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        if (strings[i]) { size_t l = strlen(strings[i]); memcpy(r + off, strings[i], l); off += l; }
    }
    r[off] = '\0';
    return r;
}

char* nuwan_string_substring(const char* s, size_t start, size_t length) {
    if (!s) return NULL;
    size_t slen = strlen(s);
    if (start >= slen) { char* r = (char*)malloc(1); if (r) r[0] = '\0'; return r; }
    if (start + length > slen) length = slen - start;
    char* r = (char*)malloc(length + 1);
    if (!r) return NULL;
    memcpy(r, s + start, length);
    r[length] = '\0';
    return r;
}

size_t nuwan_string_length(const char* s) { return s ? strlen(s) : 0; }

int  nuwan_string_compare(const char* a, const char* b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return  1;
    return strcmp(a, b);
}
bool nuwan_string_equals(const char* a, const char* b) { return nuwan_string_compare(a, b) == 0; }

char* nuwan_string_to_upper(const char* s) {
    if (!s) return NULL;
    size_t l = strlen(s);
    char* r = (char*)malloc(l + 1);
    if (!r) return NULL;
    for (size_t i = 0; i <= l; i++) r[i] = (char)toupper((unsigned char)s[i]);
    return r;
}

char* nuwan_string_to_lower(const char* s) {
    if (!s) return NULL;
    size_t l = strlen(s);
    char* r = (char*)malloc(l + 1);
    if (!r) return NULL;
    for (size_t i = 0; i <= l; i++) r[i] = (char)tolower((unsigned char)s[i]);
    return r;
}

int nuwan_string_index_of(const char* s, const char* sub) {
    if (!s || !sub) return -1;
    const char* p = strstr(s, sub);
    return p ? (int)(p - s) : -1;
}

int nuwan_string_last_index_of(const char* s, const char* sub) {
    if (!s || !sub) return -1;
    size_t sl = strlen(s), subl = strlen(sub);
    if (subl > sl) return -1;
    for (int i = (int)(sl - subl); i >= 0; i--) {
        if (memcmp(s + i, sub, subl) == 0) return i;
    }
    return -1;
}

bool nuwan_string_contains(const char* s, const char* sub) {
    return nuwan_string_index_of(s, sub) >= 0;
}

bool nuwan_string_starts_with(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool nuwan_string_ends_with(const char* s, const char* suffix) {
    if (!s || !suffix) return false;
    size_t sl = strlen(s), xl = strlen(suffix);
    return sl >= xl && memcmp(s + sl - xl, suffix, xl) == 0;
}

char* nuwan_string_trim(const char* s) {
    if (!s) return NULL;
    while (*s && isspace((unsigned char)*s)) s++;
    size_t l = strlen(s);
    while (l > 0 && isspace((unsigned char)s[l-1])) l--;
    char* r = (char*)malloc(l + 1);
    if (!r) return NULL;
    memcpy(r, s, l);
    r[l] = '\0';
    return r;
}

char* nuwan_string_trim_start(const char* s) {
    if (!s) return NULL;
    while (*s && isspace((unsigned char)*s)) s++;
    return strdup(s);
}

char* nuwan_string_trim_end(const char* s) {
    if (!s) return NULL;
    size_t l = strlen(s);
    while (l > 0 && isspace((unsigned char)s[l-1])) l--;
    char* r = (char*)malloc(l + 1);
    if (!r) return NULL;
    memcpy(r, s, l);
    r[l] = '\0';
    return r;
}

char** nuwan_string_split(const char* s, const char* delim, size_t* out_count) {
    if (!s || !delim || !out_count) { if (out_count) *out_count = 0; return NULL; }
    size_t dlen = strlen(delim);
    /* count parts */
    size_t n = 1;
    const char* p = s;
    while ((p = strstr(p, delim))) { n++; p += dlen; }
    char** arr = (char**)malloc(n * sizeof(char*));
    if (!arr) { *out_count = 0; return NULL; }
    const char* cur = s;
    for (size_t i = 0; i < n; i++) {
        const char* end = strstr(cur, delim);
        size_t l = end ? (size_t)(end - cur) : strlen(cur);
        arr[i] = (char*)malloc(l + 1);
        if (arr[i]) { memcpy(arr[i], cur, l); arr[i][l] = '\0'; }
        cur = end ? end + dlen : cur + l;
    }
    *out_count = n;
    return arr;
}

char* nuwan_string_replace(const char* s, const char* old_s, const char* new_s) {
    /* replace first occurrence */
    if (!s || !old_s || !new_s) return s ? strdup(s) : NULL;
    const char* p = strstr(s, old_s);
    if (!p) return strdup(s);
    size_t pre = (size_t)(p - s), ol = strlen(old_s), nl = strlen(new_s), sl = strlen(s);
    char* r = (char*)malloc(sl - ol + nl + 1);
    if (!r) return NULL;
    memcpy(r, s, pre);
    memcpy(r + pre, new_s, nl);
    memcpy(r + pre + nl, p + ol, sl - pre - ol + 1);
    return r;
}

char* nuwan_string_replace_all(const char* s, const char* old_s, const char* new_s) {
    if (!s || !old_s || !new_s) return s ? strdup(s) : NULL;
    size_t ol = strlen(old_s);
    if (ol == 0) return strdup(s);
    /* build result iteratively */
    char* result = strdup(s);
    const char* src = result;
    while (strstr(src, old_s)) {
        char* tmp = nuwan_string_replace(result, old_s, new_s);
        free(result);
        result = tmp;
        if (!result) return NULL;
        src = result;
    }
    return result;
}

char nuwan_string_char_at(const char* s, size_t idx) {
    if (!s || idx >= strlen(s)) return '\0';
    return s[idx];
}

bool nuwan_string_is_empty(const char* s) { return !s || *s == '\0'; }

char* nuwan_string_format(const char* fmt, ...) {
    if (!fmt) return NULL;
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return NULL; }
    char* r = (char*)malloc((size_t)n + 1);
    if (r) vsnprintf(r, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return r;
}

int    nuwan_string_to_int(const char* s)   { return s ? atoi(s) : 0; }
double nuwan_string_to_float(const char* s) { return s ? atof(s) : 0.0; }
bool   nuwan_string_to_bool(const char* s)  {
    if (!s) return false;
    return strcmp(s, "true") == 0 || strcmp(s, "1") == 0;
}

char* nuwan_int_to_string(int64_t v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", (long long)v);
    return strdup(buf);
}

char* nuwan_float_to_string(double v) {
    char buf[64];
    snprintf(buf, sizeof buf, "%g", v);
    return strdup(buf);
}

char* nuwan_bool_to_string(bool v) { return strdup(v ? "true" : "false"); }

bool nuwan_string_is_numeric(const char* s) {
    if (!s || !*s) return false;
    if (*s == '-' || *s == '+') s++;
    if (!*s) return false;
    while (*s) { if (!isdigit((unsigned char)*s)) return false; s++; }
    return true;
}

bool nuwan_string_is_alpha(const char* s) {
    if (!s || !*s) return false;
    while (*s) { if (!isalpha((unsigned char)*s)) return false; s++; }
    return true;
}

bool nuwan_string_is_alphanumeric(const char* s) {
    if (!s || !*s) return false;
    while (*s) { if (!isalnum((unsigned char)*s)) return false; s++; }
    return true;
}

uint64_t nuwan_string_hash_public(const char* s) { return nuwan_string_hash(s); }

void nuwan_string_free(char* s) { free(s); }
