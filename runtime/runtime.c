#include "runtime.h"
#include "io/direct_io.h"
#include "io/fast_format.h"
#include "io/lockfree_log.h"

static CpxLogQueue g_runtime_logq;
static bool g_runtime_logq_ready = false;

// Common runtime utilities
void* nuwan_malloc(size_t size) {
    return malloc(size);
}

void nuwan_free(void* ptr) {
    free(ptr);
}

char* nuwan_strdup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str) + 1;
    char* copy = (char*)malloc(len);
    if (copy) {
        memcpy(copy, str, len);
    }
    return copy;
}

// String operations
char* nuwan_strconcat(const char* s1, const char* s2) {
    if (!s1) s1 = "";
    if (!s2) s2 = "";
    
    size_t len1 = strlen(s1);
    size_t len2 = strlen(s2);
    char* result = (char*)malloc(len1 + len2 + 1);
    
    if (result) {
        memcpy(result, s1, len1);
        memcpy(result + len1, s2, len2 + 1);
    }
    
    return result;
}

int nuwan_strlen(const char* str) {
    return str ? (int)strlen(str) : 0;
}

// Math operations
long long nuwan_power(long long base, long long exp) {
    if (exp < 0) return 0;
    if (exp == 0) return 1;
    
    long long result = 1;
    for (long long i = 0; i < exp; i++) {
        result *= base;
    }
    return result;
}

long long nuwan_sqrt(long long n) {
    if (n <= 0) return 0;
    if (n == 1) return 1;
    
    long long x = n;
    long long y = 1;
    while (x > y) {
        x = (x + y) / 2;
        y = n / x;
    }
    return x;
}

long long nuwan_gcd(long long a, long long b) {
    while (b != 0) {
        long long temp = b;
        b = a % b;
        a = temp;
    }
    return a;
}

long long nuwan_lcm(long long a, long long b) {
    if (a == 0 || b == 0) return 0;
    return (a * b) / nuwan_gcd(a, b);
}



void nuwan_print_int(int64_t val) {
    char buf[48];
    CpxFmtBuffer out;
    cpx_fmt_init(&out, buf, sizeof(buf));
    cpx_fmt_append_i64(&out, val);
    cpx_fmt_putc(&out, '\n');
    (void)cpx_io_write_all_fd(1, buf, out.len);
}

void nuwan_print_float(double val) {
    char buf[64];
    CpxFmtBuffer out;
    cpx_fmt_init(&out, buf, sizeof(buf));
    cpx_fmt_append_f64_6(&out, val);
    cpx_fmt_putc(&out, '\n');
    (void)cpx_io_write_all_fd(1, buf, out.len);
}

void nuwan_print_bool(bool val) {
    static const char t[] = "true\n";
    static const char f[] = "false\n";
    const char* s = val ? t : f;
    size_t n = val ? (sizeof(t) - 1) : (sizeof(f) - 1);
    (void)cpx_io_write_all_fd(1, s, n);
}

void nuwan_print_str(const char* val) {
    if (!val) return;
    size_t n = strlen(val);
    (void)cpx_io_write_all_fd(1, val, n);
    (void)cpx_io_write_all_fd(1, "\n", 1);
}

int nuwan_log_init(uint32_t capacity_pow2, int out_fd) {
    if (g_runtime_logq_ready) return 0;
    if (cpx_logq_init(&g_runtime_logq, capacity_pow2, out_fd) != 0) return -1;
    g_runtime_logq_ready = true;
    return 0;
}

void nuwan_log_shutdown(void) {
    if (!g_runtime_logq_ready) return;
    cpx_logq_shutdown(&g_runtime_logq);
    g_runtime_logq_ready = false;
}

bool nuwan_log_msg(CpxLogLevel level, const char* msg) {
    if (!msg) return false;
    size_t n = strlen(msg);
    if (!g_runtime_logq_ready) {
        return cpx_io_write_all_fd(2, msg, n) == 0;
    }
    return cpx_logq_try_push(&g_runtime_logq, level, msg, n);
}
