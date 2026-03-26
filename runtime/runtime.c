#include "runtime.h"

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



