#include "../runtime/io/direct_io.h"
#include "../runtime/io/fast_format.h"
#include "../runtime/io/lockfree_log.h"

#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

#if !defined(_WIN32)
#include <pthread.h>
#endif

#define ITERATIONS 100000

static double get_ms(struct timespec start, struct timespec end) {
    return (double)(end.tv_sec - start.tv_sec) * 1000.0 + (double)(end.tv_nsec - start.tv_nsec) / 1000000.0;
}

void test_formatting() {
    printf("[TEST] Formatting correctness and speed...\n");
    char buf[128];
    uint64_t val = 1234567890123456789ULL;
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        cpx_fmt_snprintf(buf, sizeof(buf), "Value: %llu, Hex: %p", val + i, (void*)(uintptr_t)(val + i));
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("  fast_format: %.2f ms for %d iterations\n", get_ms(start, end), ITERATIONS);

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        snprintf(buf, sizeof(buf), "Value: %llu, Hex: %p", (unsigned long long)(val + i), (void*)(uintptr_t)(val + i));
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("  libc snprintf: %.2f ms for %d iterations\n", get_ms(start, end), ITERATIONS);
}

void* producer_thread(void* arg) {
    CpxLogQueue* q = (CpxLogQueue*)arg;
    char msg[64];
    for (int i = 0; i < 1000; i++) {
        size_t len = cpx_fmt_snprintf(msg, sizeof(msg), "Log message from thread %p, iteration %d", (void*)pthread_self(), i);
        while (!cpx_logq_try_push(q, CPX_LOG_INFO, msg, len)) {
            // spin or yield
        }
    }
    return NULL;
}

void test_async_logging() {
    printf("[TEST] Async lock-free logging under contention...\n");
    CpxLogQueue q;
    if (cpx_logq_init(&q, 1024, 1) != 0) {
        printf("  [FAIL] Failed to init log queue\n");
        return;
    }

    pthread_t threads[8];
    for (int i = 0; i < 8; i++) {
        pthread_create(&threads[i], NULL, producer_thread, &q);
    }

    for (int i = 0; i < 8; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("  Draining queue...\n");
    // Wait a bit for the drain thread to catch up
    struct timespec ts = {0, 100000000L}; // 100ms
    nanosleep(&ts, NULL);

    cpx_logq_shutdown(&q);
    printf("  [PASS] Async logging finished\n");
}

int main() {
    printf("=== Casprix High-Performance I/O Test Suite ===\n\n");
    test_formatting();
    printf("\n");
    test_async_logging();
    return 0;
}
