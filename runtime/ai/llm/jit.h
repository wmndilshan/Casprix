/*
 * LLM Runtime - CPU JIT Kernel Compiler
 *
 * Emits native x86-64 AVX2 machine code for fused tensor operations.
 * Fused kernels avoid intermediate memory writes, improving cache
 * utilization for elementwise and bias+activation patterns.
 *
 * Target: x86-64, AVX2+FMA, Windows (VirtualAlloc) and Linux (mmap).
 * Calling convention: Win64 (RCX, RDX, R8, R9) or SysV (RDI, RSI, RDX, RCX).
 */

#ifndef LLM_JIT_H
#define LLM_JIT_H

#include "tensor.h"
#include "tensor_ir.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * JIT Types
 * ==========================================================================*/

/* Executable memory buffer */
typedef struct {
    u8* code;               /* Executable memory region                      */
    size_t size;            /* Bytes used                                    */
    size_t capacity;        /* Total allocated capacity                      */
} JitBuffer;

/* Function signature for elementwise fused kernels:
 *   fn(const float* in, float* out, int n)                                  */
typedef void (*JitElemKernelFn)(const f32* in, f32* out, i32 n);

/* Function signature for GEMM + bias + activation fused kernels:
 *   fn(const float* A, const float* B, const float* bias, float* out,
 *      int M, int K, int N)                                                 */
typedef void (*JitGemmFusedFn)(const f32* A, const f32* B, const f32* bias,
                                f32* out, i32 M, i32 K, i32 N);

/* Function signature for bias + residual add:
 *   fn(const float* x, const float* bias, const float* residual,
 *      float* out, int rows, int cols)                                      */
typedef void (*JitBiasResidualFn)(const f32* x, const f32* bias,
                                   const f32* residual, f32* out,
                                   i32 rows, i32 cols);

/* Compiled kernel entry */
typedef struct {
    IROpCode fused_op;      /* Which fused pattern this implements           */
    void* fn_ptr;           /* Cast to appropriate function signature        */
    u32 pattern_hash;       /* Hash for lookup cache                         */
} JitKernel;

/* JIT compiler state */
typedef struct {
    JitBuffer* buffer;      /* Active code buffer                            */
    JitKernel* cache;       /* Compiled kernel cache                         */
    i32 cache_count;
    i32 cache_capacity;
    u8* emit_ptr;           /* Current write position in buffer              */

    /* Statistics */
    i32 kernels_compiled;
    i32 cache_hits;
    size_t total_code_bytes;
} JitCompiler;

/* ============================================================================
 * JIT Compiler API
 * ==========================================================================*/

/* Create JIT compiler with given code buffer capacity (bytes) */
JitCompiler* jit_create(size_t code_capacity);

/* Destroy JIT compiler (frees executable memory) */
void jit_destroy(JitCompiler* jit);

/* Compile a fused IR node to native code. Returns function pointer. */
void* jit_compile_fused(JitCompiler* jit, IRNode* fused_node);

/* Look up cached kernel or compile new one */
void* jit_lookup_or_compile(JitCompiler* jit, IRNode* node);

/* ============================================================================
 * Pre-built Fused Kernels (C fallback for non-JIT path)
 * ==========================================================================*/

/* GEMM + bias + GELU: out = gelu(A*B + bias) */
void fused_gemm_bias_gelu(const f32* A, const f32* B, const f32* bias,
                           f32* out, i32 M, i32 K, i32 N);

/* GEMM + bias + ReLU: out = relu(A*B + bias) */
void fused_gemm_bias_relu(const f32* A, const f32* B, const f32* bias,
                           f32* out, i32 M, i32 K, i32 N);

/* Add + Scale: out = (a + b) * scalar */
void fused_add_scale(const f32* a, const f32* b, f32* out, i32 n, f32 scalar);

/* Bias + Residual: out[i] = x[i] + bias[i % cols] + residual[i] */
void fused_bias_residual(const f32* x, const f32* bias, const f32* residual,
                          f32* out, i32 rows, i32 cols);

/* ============================================================================
 * JIT Statistics
 * ==========================================================================*/

void jit_print_stats(JitCompiler* jit);

#ifdef __cplusplus
}
#endif

#endif /* LLM_JIT_H */
