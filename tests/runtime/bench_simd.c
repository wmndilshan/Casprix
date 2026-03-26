/*
 * SIMD Kernel Micro-Benchmarks
 * Measures throughput in GFLOPS for AVX2 kernels
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../runtime/math/linalg_runtime.h"

#define ITERATIONS 1000
#define VECTOR_SIZE 10000

double get_time() {
    return (double)clock() / CLOCKS_PER_SEC;
}

void benchmark_dot_product() {
    printf("\n=== Dot Product Benchmark ===\n");
    
    NuwanVector* a = nuwan_vector_create(VECTOR_SIZE);
    NuwanVector* b = nuwan_vector_create(VECTOR_SIZE);
    
    for (int i = 0; i < VECTOR_SIZE; i++) {
        a->data[i] = (double)i * 0.1;
        b->data[i] = (double)(VECTOR_SIZE - i) * 0.2;
    }
    
    // Warm-up
    for (int i = 0; i < 10; i++) {
        nuwan_vector_dot(a, b);
    }
    
    // Benchmark
    double start = get_time();
    double sum = 0.0;
    for (int i = 0; i < ITERATIONS; i++) {
        sum += nuwan_vector_dot(a, b);
    }
    double elapsed = get_time() - start;
    
    double flops = 2.0 * VECTOR_SIZE * ITERATIONS;
    double gflops = flops / (elapsed * 1e9);
    
    printf("Size: %d elements\n", VECTOR_SIZE);
    printf("Iterations: %d\n", ITERATIONS);
    printf("Time: %.3f seconds\n", elapsed);
    printf("Throughput: %.2f GFLOPS\n", gflops);
    
    nuwan_vector_free(a);
    nuwan_vector_free(b);
}

int main() {
    printf("=== AVX2 SIMD Kernel Benchmarks ===\n");
    
#ifdef HAS_AVX2
    printf("AVX2 SIMD: ENABLED\n");
#else
    printf("AVX2 SIMD: DISABLED\n");
#endif
    
    benchmark_dot_product();
    
    printf("\n=== Complete ===\n");
    return 0;
}
