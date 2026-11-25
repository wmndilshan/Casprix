#include <stdint.h>
#include <stdio.h>

void nuwan_print_int(int64_t value) { printf("%lld\n", (long long)value); }
void nuwan_print_uint(uint64_t value) { printf("%llu\n", (unsigned long long)value); }
void nuwan_print_bool(_Bool value) { puts(value ? "true" : "false"); }
void nuwan_print_str(const char* value) { puts(value ? value : ""); }
