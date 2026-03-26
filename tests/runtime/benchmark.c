/**
 * Casperix Performance Benchmarks
 * Validate runtime performance targets
 */

#include "runtime/async/task.h"
#include "runtime/async/future.h"
#include "runtime/concurrent/channel.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
static inline uint64_t get_nanoseconds() {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (counter.QuadPart * 1000000000) / freq.QuadPart;
}
#else
#include <sys/time.h>
static inline uint64_t get_nanoseconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
#endif

void benchmark_task_spawn() {
    printf("\n=== Task Spawn Benchmark ===\n");
    
    const int iterations = 100000;
    
    uint64_t start = get_nanoseconds();
    
    for (int i = 0; i < iterations; i++) {
        Task* task = task_create(NULL, NULL, "bench");
        task_destroy(task);
    }
    
    uint64_t end = get_nanoseconds();
    uint64_t elapsed_ns = end - start;
    double avg_ns = (double)elapsed_ns / iterations;
    
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.2f ms\n", elapsed_ns / 1000000.0);
    printf("Average: %.0f ns per spawn\n", avg_ns);
    printf("Target: < 1000 ns (1 μs)\n");
    printf("Status: %s\n", avg_ns < 1000 ? "PASS" : "FAIL");
}

void noop_callback(void* result, void* user_data) {
    (void)result;
    (void)user_data;
}

void benchmark_future_callback() {
    printf("\n=== Future Callback Benchmark ===\n");
    
    const int iterations = 100000;
    
    uint64_t start = get_nanoseconds();
    
    for (int i = 0; i < iterations; i++) {
        Future* f = future_create();
        future_then(f, noop_callback, NULL);
        future_complete(f, NULL);
        future_destroy(f);
    }
    
    uint64_t end = get_nanoseconds();
    uint64_t elapsed_ns = end - start;
    double avg_ns = (double)elapsed_ns / iterations;
    
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.2f ms\n", elapsed_ns / 1000000.0);
    printf("Average: %.0f ns per callback\n", avg_ns);
    printf("Target: < 500 ns\n");
    printf("Status: %s\n", avg_ns < 500 ? "PASS" : "FAIL");
}

void benchmark_channel_throughput() {
    printf("\n=== Channel Throughput Benchmark ===\n");

    const int iterations = 100000;
    /* Buffer must be >= iterations since send/recv are sequential (same thread) */
    Channel* ch = channel_create(iterations);

    uint64_t start = get_nanoseconds();

    for (int i = 0; i < iterations; i++) {
        int* val = malloc(sizeof(int));
        *val = i;
        channel_send(ch, val);
    }

    for (int i = 0; i < iterations; i++) {
        int* val = (int*)channel_recv(ch);
        free(val);
    }

    uint64_t end = get_nanoseconds();
    uint64_t elapsed_ns = end - start;
    double avg_ns = (double)elapsed_ns / (iterations * 2);  // send + recv

    printf("Iterations: %d\n", iterations);
    printf("Total time: %.2f ms\n", elapsed_ns / 1000000.0);
    printf("Average: %.0f ns per operation\n", avg_ns);
    printf("Throughput: %.2f Mops/sec\n", 1000.0 / avg_ns);
    printf("Target: < 1000 ns\n");
    printf("Status: %s\n", avg_ns < 1000 ? "PASS" : "FAIL");

    channel_destroy(ch);
}

void benchmark_memory_overhead() {
    printf("\n=== Memory Overhead Benchmark ===\n");
    
    const int count = 10000;
    Task** tasks = malloc(sizeof(Task*) * count);
    
    size_t baseline = sizeof(Task) * count;
    
    for (int i = 0; i < count; i++) {
        tasks[i] = task_create(NULL, NULL, "mem_test");
    }
    
    printf("Tasks created: %d\n", count);
    printf("Baseline size: %llu bytes\n", (unsigned long long)baseline);
    printf("Per-task size: %llu bytes\n", (unsigned long long)sizeof(Task));
    printf("Status: %s\n", sizeof(Task) < 1024 ? "PASS (< 1KB)" : "FAIL");
    
    for (int i = 0; i < count; i++) {
        task_destroy(tasks[i]);
    }
    free(tasks);
}

int main(void) {
    printf("╔════════════════════════════════════════╗\n");
    printf("║  Casperix Performance Benchmarks      ║\n");
    printf("╚════════════════════════════════════════╝\n");
    
    benchmark_task_spawn();
    benchmark_future_callback();
    benchmark_channel_throughput();
    benchmark_memory_overhead();
    
    printf("\n=== Benchmark Suite Complete ===\n\n");
    
    return 0;
}
