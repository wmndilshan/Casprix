/*
 * Casprix Runtime - High-Performance String Operations Header
 * All hot paths delegate to SSE2/ASM kernels in string_asm.asm
 * Memory-safe: null guards, bounds checks on all public API
 */
#ifndef NUWAN_STRING_OPS_H
#define NUWAN_STRING_OPS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Concat ---- */
char*    nuwan_string_concat(const char* s1, const char* s2);
char*    nuwan_string_concat_many(const char** strings, size_t count);

/* ---- Substring ---- */
char*    nuwan_string_substring(const char* str, size_t start, size_t length);

/* ---- Length ---- */
size_t   nuwan_string_length(const char* str);

/* ---- Compare ---- */
int      nuwan_string_compare(const char* s1, const char* s2);
bool     nuwan_string_equals(const char* s1, const char* s2);

/* ---- Case (SSE2 batch 16 chars/iter) ---- */
char*    nuwan_string_to_upper(const char* str);
char*    nuwan_string_to_lower(const char* str);

/* ---- Search (BMH for len>=2, memchr for single char) ---- */
int      nuwan_string_index_of(const char* str, const char* substr);
int      nuwan_string_last_index_of(const char* str, const char* substr);
bool     nuwan_string_contains(const char* str, const char* substr);
bool     nuwan_string_starts_with(const char* str, const char* prefix);
bool     nuwan_string_ends_with(const char* str, const char* suffix);

/* ---- Trim ---- */
char*    nuwan_string_trim(const char* str);
char*    nuwan_string_trim_start(const char* str);
char*    nuwan_string_trim_end(const char* str);

/* ---- Split / Replace ---- */
char**   nuwan_string_split(const char* str, const char* delim, size_t* out_count);
char*    nuwan_string_replace(const char* str, const char* old_s, const char* new_s);
char*    nuwan_string_replace_all(const char* str, const char* old_s, const char* new_s);

/* ---- Char access (bounds-safe) ---- */
char     nuwan_string_char_at(const char* str, size_t index);
bool     nuwan_string_is_empty(const char* str);

/* ---- Formatting ---- */
char*    nuwan_string_format(const char* fmt, ...);

/* ---- Type conversions ---- */
int      nuwan_string_to_int(const char* s);
double   nuwan_string_to_float(const char* s);
bool     nuwan_string_to_bool(const char* s);
char*    nuwan_int_to_string(int64_t value);   /* fast: no snprintf */
char*    nuwan_float_to_string(double value);
char*    nuwan_bool_to_string(bool value);

/* ---- Validation (SSE2 for is_alpha) ---- */
bool     nuwan_string_is_numeric(const char* s);
bool     nuwan_string_is_alpha(const char* s);
bool     nuwan_string_is_alphanumeric(const char* s);

/* ---- Hash (FNV-1a 64-bit, from ASM kernel) ---- */
uint64_t nuwan_string_hash_public(const char* s);

/* ---- Memory management ---- */
void     nuwan_string_free(char* s);

/* ---- ASM kernel declarations ---- */
extern size_t   nuwan_strlen_asm(const char* str);
extern void*    nuwan_memcopy(void* dst, const void* src, size_t n);
extern void*    nuwan_memset_fast(void* ptr, int val, size_t n);
extern int      nuwan_strcmp_asm(const char* s1, const char* s2);
extern uint64_t nuwan_string_hash(const char* str);

#ifdef __cplusplus
}
#endif
#endif /* NUWAN_STRING_OPS_H */
