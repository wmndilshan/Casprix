/*
 * LLM Runtime - Fundamental Operations Header
 * 
 * Core operations for LLM training and inference
 */

#ifndef LLM_OPS_H
#define LLM_OPS_H

#include "tensor.h"

// ===== ELEMENTWISE OPERATIONS =====

// Vector addition: out[i] = a[i] + b[i]
void vec_add_f32(const f32* a, const f32* b, f32* out, i32 n);

// Vector multiplication: out[i] = a[i] * b[i]
void vec_mul_f32(const f32* a, const f32* b, f32* out, i32 n);

// FMA: out[i] = a[i] * b[i] + c[i]
void vec_fma_f32(const f32* a, const f32* b, const f32* c, f32* out, i32 n);

// Scalar multiply: out[i] = a[i] * scalar
void vec_scale_f32(const f32* a, f32 scalar, f32* out, i32 n);

// Scalar add: out[i] = a[i] + scalar
void vec_add_scalar_f32(const f32* a, f32 scalar, f32* out, i32 n);

// ===== ACTIVATION FUNCTIONS =====

// ReLU: out[i] = max(0, x[i])
void relu_f32(const f32* x, f32* out, i32 n);

// GELU approximation: x * 0.5 * (1 + tanh(...))
void gelu_f32(const f32* x, f32* out, i32 n);

// ===== REDUCTIONS =====

// Sum of all elements
f32 vec_sum_f32(const f32* x, i32 n);

// Mean of all elements
f32 vec_mean_f32(const f32* x, i32 n);

// Max element
f32 vec_max_f32(const f32* x, i32 n);

// ===== MATRIX OPERATIONS =====

// GEMM: C = A * B
// A: [M, K], B: [K, N], C: [M, N]
// All matrices row-major
void gemm_f32(const f32* A, const f32* B, f32* C, i32 M, i32 K, i32 N);

// GEMV: y = A * x
// A: [M, N], x: [N], y: [M]
void gemv_f32(const f32* A, const f32* x, f32* y, i32 M, i32 N);

// Matrix transpose (out-of-place)
void transpose_f32(const f32* A, f32* A_T, i32 rows, i32 cols);

// ===== NORMALIZATION =====

// LayerNorm: y = gamma * (x - mean) / sqrt(var + eps) + beta
// Apply per row
// x: [batch, hidden_dim]
// gamma, beta: [hidden_dim]
void layernorm_f32(const f32* x, const f32* gamma, const f32* beta, 
                   f32* out, i32 batch, i32 hidden_dim, f32 eps);

// RMSNorm: y = x / sqrt(mean(x^2) + eps) * gamma
void rmsnorm_f32(const f32* x, const f32* gamma, f32* out, 
                 i32 batch, i32 hidden_dim, f32 eps);

// Softmax (numerically stable)
// Apply per row
// x: [batch, dim]
void softmax_f32(const f32* x, f32* out, i32 batch, i32 dim);

// ===== EMBEDDING =====

// Embedding lookup
// table: [vocab_size, hidden_dim]
// token_ids: [batch * seq_len]
// out: [batch, seq_len, hidden_dim]
void embedding_lookup(const f32* table, const i32* token_ids, f32* out,
                      i32 batch, i32 seq_len, i32 vocab_size, i32 hidden_dim);

// ===== LOSS =====

// Cross-entropy loss with softmax
// logits: [batch, vocab_size]
// targets: [batch]
// Returns average loss
f32 cross_entropy_loss(const f32* logits, const i32* targets, 
                       i32 batch, i32 vocab_size);

// ===== SiLU ACTIVATION =====

// SiLU/Swish: out[i] = x[i] * sigmoid(x[i])
void silu_f32(const f32* x, f32* out, i32 n);

// ===== DOT PRODUCT =====

// Dot product: return sum(a[i] * b[i])
f32 vec_dot_f32(const f32* a, const f32* b, i32 n);

// ===== EXTERNAL NASM KERNELS =====

#ifdef __cplusplus
extern "C" {
#endif

// ---- Elementwise Operations (AVX2, 2x unrolled) ----
extern void vec_add_f32_avx2(const f32* a, const f32* b, f32* out, i32 n);
extern void vec_mul_f32_avx2(const f32* a, const f32* b, f32* out, i32 n);
extern void vec_fma_f32_avx2(const f32* a, const f32* b, const f32* c, f32* out, i32 n);
extern void vec_scale_f32_avx2(const f32* a, f32 scalar, f32* out, i32 n);
extern void vec_add_scalar_f32_avx2(const f32* a, f32 scalar, f32* out, i32 n);

// ---- Activation Functions (AVX2) ----
extern void relu_f32_avx2(const f32* x, f32* out, i32 n);
extern void gelu_f32_avx2(const f32* x, f32* out, i32 n);
extern void silu_f32_avx2(const f32* x, f32* out, i32 n);

// ---- Reductions (AVX2, dual accumulator) ----
extern f32 vec_sum_f32_avx2(const f32* x, i32 n);
extern f32 vec_mean_f32_avx2(const f32* x, i32 n);
extern f32 vec_max_f32_avx2(const f32* x, i32 n);

// ---- Dot Product (AVX2, dual accumulator + FMA) ----
extern f32 vec_dot_f32_avx2(const f32* a, const f32* b, i32 n);

// ---- Matrix Operations (AVX2) ----

// GEMM micro-kernel (4x4 tile): C[4,4] += A[4,K] * B[K,4]
// Uses XMM registers, suitable for small remainder tiles.
extern void gemm_kernel_4x4_avx2(const f32* A, const f32* B, f32* C,
                                  i32 K, i32 lda, i32 ldb, i32 ldc);

// GEMM micro-kernel (6x16 tile): C[6,16] += A[6,K] * B[K,16]
// Uses ALL 16 YMM registers (12 FMA accumulators + 2 B-loads + 1 A-broadcast + 1 temp).
// 12 FMAs per K iteration = 96 FLOPs/iter. Primary kernel for large GEMM.
extern void gemm_kernel_6x16_avx2(const f32* A, const f32* B, f32* C,
                                   i32 K, i32 lda, i32 ldb, i32 ldc);

// GEMV: y = A * x, where A: [M, N], x: [N], y: [M]
// Uses dual-accumulator dot product per row.
extern void gemv_f32_avx2(const f32* A, const f32* x, f32* y, i32 M, i32 N);

// ---- Normalization (AVX2, 2x unrolled) ----
// Single-row softmax (numerically stable, dual accumulator max/sum)
extern void softmax_row_avx2(const f32* x, f32* out, i32 dim);

// Single-row LayerNorm: y = gamma * (x - mean) / sqrt(var + eps) + beta
extern void layernorm_row_avx2(const f32* x, const f32* gamma, const f32* beta,
                                f32* out, i32 dim, f32 eps);

// Single-row RMSNorm: y = x / sqrt(mean(x^2) + eps) * gamma
extern void rmsnorm_row_avx2(const f32* x, const f32* gamma, f32* out,
                              i32 dim, f32 eps);

// ---- Optimizer (AVX2) ----
// Fused Adam step: param -= lr * m_hat / (sqrt(v_hat) + eps)
// beta1_t = (1 - beta1^t), beta2_t = (1 - beta2^t) for bias correction
extern void adam_step_avx2(f32* param, const f32* grad, f32* m, f32* v,
                            f32 lr, f32 beta1, f32 beta2, f32 eps,
                            f32 beta1_t, f32 beta2_t, i32 n);

// ---- Backward Pass Kernels (AVX2, 2x unrolled where applicable) ----

// ReLU backward: grad_in = grad_out * (x > 0)
extern void relu_backward_avx2(const f32* grad_out, const f32* x, f32* grad_in, i32 n);

// GELU backward (for fast GELU approximation)
extern void gelu_backward_avx2(const f32* grad_out, const f32* x, f32* grad_in, i32 n);

// SiLU/Swish backward
extern void silu_backward_avx2(const f32* grad_out, const f32* x, f32* grad_in, i32 n);

// Softmax backward (Jacobian-vector product for single row)
extern void softmax_backward_avx2(const f32* grad_out, const f32* softmax_out,
                                   f32* grad_in, i32 dim);

// Cross-entropy backward (fused softmax + CE gradient)
// grad = softmax_out, then grad[target] -= 1, scaled by 1/batch
extern void cross_entropy_backward_avx2(const f32* softmax_out, const i32* targets,
                                         f32* grad, i32 batch, i32 vocab_size);

// ---- Gradient Utilities (AVX2, 2x unrolled) ----

// Vector accumulate: acc[i] += src[i]
extern void vec_acc_f32_avx2(f32* acc, const f32* src, i32 n);

// Gradient clipping by L2 norm (dual accumulator for norm computation)
// If ||grad|| > max_norm, scale grad by max_norm / ||grad||
extern void grad_clip_norm_avx2(f32* grad, i32 n, f32 max_norm);

#ifdef __cplusplus
}
#endif

#endif // LLM_OPS_H
