#ifndef SIMD_KERNELS_H
#define SIMD_KERNELS_H

// AVX2-optimized SIMD kernels for high-performance linear algebra
// These are implemented in simd_kernels.asm

#include <stdint.h>

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

// ---------------------------------------------------------------------------
// Swiss-Table parallel control-byte match
//
//     uint32_t casprix_swiss_match_h2_x16(const uint8_t* ctrl16, uint8_t h2)
//
// Compares 16 control bytes at `ctrl16` against `h2` in parallel and returns
// a 16-bit mask (in the low 16 bits of the result): bit `i` is set iff
// `ctrl16[i] == h2`.
//
// Windows x64 ABI (as emitted by NASM -f win64):
//     RCX = ctrl16            (pointer, 16-byte accessible; need not be aligned)
//     DL  = h2                (low byte of RDX)
//     EAX = 16-bit match mask (upper 16 bits of EAX are zeroed)
//
// This is the ASM hook used by runtime/stdlib/collections.c's Swiss-Table
// lookup path when built with -DCASPRIX_SWISS_USE_ASM_HOOK.  Without the
// hook, collections.c inlines an SSE2/NEON intrinsic equivalent.  Either
// way, only 5 instructions are on the hot path (load / broadcast / cmpeq /
// movemask / ret) and no branches.
// ---------------------------------------------------------------------------
uint32_t casprix_swiss_match_h2_x16(const uint8_t* ctrl16, uint8_t h2);

#ifdef __cplusplus
}
#endif

#endif // SIMD_KERNELS_H
