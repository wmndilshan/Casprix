#ifndef SIMD_KERNELS_H
#define SIMD_KERNELS_H

// AVX2-optimized SIMD kernels for high-performance linear algebra
// These are implemented in simd_kernels.asm

#ifdef __cplusplus
extern "C" {
#endif

// Vector dot product: returns sum(a[i] * b[i])
// Requires: AVX2, FMA
// Performance: ~4-6x faster than scalar for n >= 32
double nuwan_simd_dot_product(const double* a, const double* b, int n);

// SAXPY: y[i] += alpha * x[i]
// Requires: AVX2, FMA
// Performance: ~6-8x faster than scalar for large n
void nuwan_simd_saxpy(double alpha, const double* x, double* y, int n);

// Vector addition: result[i] = a[i] + b[i]
// Requires: AVX2
// Performance: ~4x faster than scalar
void nuwan_simd_vector_add(const double* a, const double* b, double* result, int n);

// Vector scaling: result[i] = x[i] * scalar
// Requires: AVX2
// Performance: ~4x faster than scalar
void nuwan_simd_vector_scale(const double* x, double scalar, double* result, int n);

// Squared Euclidean distance: returns sum((a[i] - b[i])^2)
// Requires: AVX2, FMA
// Performance: ~4x faster than scalar
double nuwan_simd_euclidean_dist_sq(const double* a, const double* b, int n);

#ifdef __cplusplus
}
#endif

#endif // SIMD_KERNELS_H
