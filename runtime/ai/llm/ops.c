/*
 * LLM Runtime - Fundamental Operations Implementation
 *
 * All operations dispatch to AVX2 NASM kernels when available and n >= 8.
 * C fallbacks are provided for small arrays and non-AVX2 builds.
 */

#include "ops.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <float.h>

// ===== ELEMENTWISE OPERATIONS =====

void vec_add_f32(const f32* a, const f32* b, f32* out, i32 n) {
#ifdef HAS_AVX2
    if (n >= 8) {
        vec_add_f32_avx2(a, b, out, n);
        return;
    }
#endif
    for (i32 i = 0; i < n; i++) {
        out[i] = a[i] + b[i];
    }
}

void vec_mul_f32(const f32* a, const f32* b, f32* out, i32 n) {
#ifdef HAS_AVX2
    if (n >= 8) {
        vec_mul_f32_avx2(a, b, out, n);
        return;
    }
#endif
    for (i32 i = 0; i < n; i++) {
        out[i] = a[i] * b[i];
    }
}

void vec_fma_f32(const f32* a, const f32* b, const f32* c, f32* out, i32 n) {
#ifdef HAS_AVX2
    if (n >= 8) {
        vec_fma_f32_avx2(a, b, c, out, n);
        return;
    }
#endif
    for (i32 i = 0; i < n; i++) {
        out[i] = a[i] * b[i] + c[i];
    }
}

void vec_scale_f32(const f32* a, f32 scalar, f32* out, i32 n) {
#ifdef HAS_AVX2
    if (n >= 8) {
        vec_scale_f32_avx2(a, scalar, out, n);
        return;
    }
#endif
    for (i32 i = 0; i < n; i++) {
        out[i] = a[i] * scalar;
    }
}

void vec_add_scalar_f32(const f32* a, f32 scalar, f32* out, i32 n) {
#ifdef HAS_AVX2
    if (n >= 8) {
        vec_add_scalar_f32_avx2(a, scalar, out, n);
        return;
    }
#endif
    for (i32 i = 0; i < n; i++) {
        out[i] = a[i] + scalar;
    }
}

// ===== ACTIVATION FUNCTIONS =====

void relu_f32(const f32* x, f32* out, i32 n) {
#ifdef HAS_AVX2
    if (n >= 8) {
        relu_f32_avx2(x, out, n);
        return;
    }
#endif
    for (i32 i = 0; i < n; i++) {
        out[i] = (x[i] > 0.0f) ? x[i] : 0.0f;
    }
}

void gelu_f32(const f32* x, f32* out, i32 n) {
#ifdef HAS_AVX2
    if (n >= 8) {
        gelu_f32_avx2(x, out, n);
        return;
    }
#endif
    const f32 coef = 0.044715f;
    const f32 sqrt_2_pi = 0.7978845608f;

    for (i32 i = 0; i < n; i++) {
        f32 x_val = x[i];
        f32 x3 = x_val * x_val * x_val;
        f32 inner = sqrt_2_pi * (x_val + coef * x3);
        f32 tanh_val = tanhf(inner);
        out[i] = 0.5f * x_val * (1.0f + tanh_val);
    }
}

void silu_f32(const f32* x, f32* out, i32 n) {
#ifdef HAS_AVX2
    if (n >= 8) {
        silu_f32_avx2(x, out, n);
        return;
    }
#endif
    for (i32 i = 0; i < n; i++) {
        f32 sigmoid = 1.0f / (1.0f + expf(-x[i]));
        out[i] = x[i] * sigmoid;
    }
}

// ===== REDUCTIONS =====

f32 vec_sum_f32(const f32* x, i32 n) {
#ifdef HAS_AVX2
    if (n >= 8) {
        return vec_sum_f32_avx2(x, n);
    }
#endif
    f32 sum = 0.0f;
    for (i32 i = 0; i < n; i++) {
        sum += x[i];
    }
    return sum;
}

f32 vec_mean_f32(const f32* x, i32 n) {
#ifdef HAS_AVX2
    if (n >= 8) {
        return vec_mean_f32_avx2(x, n);
    }
#endif
    return vec_sum_f32(x, n) / (f32)n;
}

f32 vec_max_f32(const f32* x, i32 n) {
#ifdef HAS_AVX2
    if (n >= 8) {
        return vec_max_f32_avx2(x, n);
    }
#endif
    f32 max_val = -FLT_MAX;
    for (i32 i = 0; i < n; i++) {
        if (x[i] > max_val) max_val = x[i];
    }
    return max_val;
}

// ===== DOT PRODUCT =====

f32 vec_dot_f32(const f32* a, const f32* b, i32 n) {
#ifdef HAS_AVX2
    if (n >= 8) {
        return vec_dot_f32_avx2(a, b, n);
    }
#endif
    f32 sum = 0.0f;
    for (i32 i = 0; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}

// ===== MATRIX OPERATIONS =====

// Tiled GEMM using 6x16 AVX2 micro-kernel with L2/L3 cache blocking
void gemm_f32(const f32* A, const f32* B, f32* C, i32 M, i32 K, i32 N) {
    memset(C, 0, M * N * sizeof(f32));

#ifdef HAS_AVX2
    // Cache-blocking parameters tuned for typical L2 cache (256KB-1MB)
    const i32 BLOCK_M = 48;   // Multiple of 6 (micro-kernel rows)
    const i32 BLOCK_N = 128;  // Multiple of 16 (micro-kernel cols)
    const i32 BLOCK_K = 256;

    for (i32 jj = 0; jj < N; jj += BLOCK_N) {
        i32 n_block = (jj + BLOCK_N > N) ? (N - jj) : BLOCK_N;

        for (i32 kk = 0; kk < K; kk += BLOCK_K) {
            i32 k_block = (kk + BLOCK_K > K) ? (K - kk) : BLOCK_K;

            for (i32 ii = 0; ii < M; ii += BLOCK_M) {
                i32 m_block = (ii + BLOCK_M > M) ? (M - ii) : BLOCK_M;

                // Dispatch to 6x16 micro-kernel for full tiles
                i32 i = 0;
                for (; i + 6 <= m_block; i += 6) {
                    i32 j = 0;
                    for (; j + 16 <= n_block; j += 16) {
                        gemm_kernel_6x16_avx2(
                            A + (ii + i) * K + kk,
                            B + kk * N + (jj + j),
                            C + (ii + i) * N + (jj + j),
                            k_block, K, N, N
                        );
                    }
                    // Remainder columns: use 4x4 micro-kernel
                    for (; j + 4 <= n_block; j += 4) {
                        gemm_kernel_4x4_avx2(
                            A + (ii + i) * K + kk,
                            B + kk * N + (jj + j),
                            C + (ii + i) * N + (jj + j),
                            k_block, K, N, N
                        );
                    }
                    // Scalar remainder for last few columns
                    if (j < n_block) {
                        for (i32 ri = 0; ri < 6 && (ii + i + ri) < M; ri++) {
                            for (i32 rj = j; rj < n_block; rj++) {
                                f32 sum = 0.0f;
                                for (i32 rk = 0; rk < k_block; rk++) {
                                    sum += A[(ii + i + ri) * K + (kk + rk)] *
                                           B[(kk + rk) * N + (jj + rj)];
                                }
                                C[(ii + i + ri) * N + (jj + rj)] += sum;
                            }
                        }
                    }
                }
                // Remainder rows: scalar fallback
                for (; i < m_block; i++) {
                    for (i32 j = 0; j < n_block; j++) {
                        f32 sum = 0.0f;
                        for (i32 rk = 0; rk < k_block; rk++) {
                            sum += A[(ii + i) * K + (kk + rk)] *
                                   B[(kk + rk) * N + (jj + j)];
                        }
                        C[(ii + i) * N + (jj + j)] += sum;
                    }
                }
            }
        }
    }
#else
    // Scalar fallback with cache blocking
    const i32 BLOCK_M = 64;
    const i32 BLOCK_K = 256;
    const i32 BLOCK_N = 64;

    for (i32 ii = 0; ii < M; ii += BLOCK_M) {
        i32 m_block = (ii + BLOCK_M > M) ? (M - ii) : BLOCK_M;
        for (i32 jj = 0; jj < N; jj += BLOCK_N) {
            i32 n_block = (jj + BLOCK_N > N) ? (N - jj) : BLOCK_N;
            for (i32 kk = 0; kk < K; kk += BLOCK_K) {
                i32 k_block = (kk + BLOCK_K > K) ? (K - kk) : BLOCK_K;
                for (i32 i = 0; i < m_block; i++) {
                    for (i32 j = 0; j < n_block; j++) {
                        f32 sum = C[(ii + i) * N + (jj + j)];
                        for (i32 k = 0; k < k_block; k++) {
                            sum += A[(ii + i) * K + (kk + k)] *
                                   B[(kk + k) * N + (jj + j)];
                        }
                        C[(ii + i) * N + (jj + j)] = sum;
                    }
                }
            }
        }
    }
#endif
}

void gemv_f32(const f32* A, const f32* x, f32* y, i32 M, i32 N) {
#ifdef HAS_AVX2
    if (N >= 8) {
        gemv_f32_avx2(A, x, y, M, N);
        return;
    }
#endif
    for (i32 i = 0; i < M; i++) {
        f32 sum = 0.0f;
        for (i32 j = 0; j < N; j++) {
            sum += A[i * N + j] * x[j];
        }
        y[i] = sum;
    }
}

void transpose_f32(const f32* A, f32* A_T, i32 rows, i32 cols) {
    const i32 BLOCK = 32;

    for (i32 i = 0; i < rows; i += BLOCK) {
        for (i32 j = 0; j < cols; j += BLOCK) {
            for (i32 ii = i; ii < i + BLOCK && ii < rows; ii++) {
                for (i32 jj = j; jj < j + BLOCK && jj < cols; jj++) {
                    A_T[jj * rows + ii] = A[ii * cols + jj];
                }
            }
        }
    }
}

// ===== NORMALIZATION =====

void layernorm_f32(const f32* x, const f32* gamma, const f32* beta,
                   f32* out, i32 batch, i32 hidden_dim, f32 eps) {
    for (i32 b = 0; b < batch; b++) {
        const f32* x_row = x + b * hidden_dim;
        f32* out_row = out + b * hidden_dim;

#ifdef HAS_AVX2
        if (hidden_dim >= 8) {
            layernorm_row_avx2(x_row, gamma, beta, out_row, hidden_dim, eps);
            continue;
        }
#endif
        f32 mean = vec_mean_f32(x_row, hidden_dim);

        f32 var_sum = 0.0f;
        for (i32 i = 0; i < hidden_dim; i++) {
            f32 diff = x_row[i] - mean;
            var_sum += diff * diff;
        }
        f32 var = var_sum / (f32)hidden_dim;
        f32 inv_std = 1.0f / sqrtf(var + eps);

        for (i32 i = 0; i < hidden_dim; i++) {
            f32 normalized = (x_row[i] - mean) * inv_std;
            out_row[i] = gamma[i] * normalized + beta[i];
        }
    }
}

void rmsnorm_f32(const f32* x, const f32* gamma, f32* out,
                 i32 batch, i32 hidden_dim, f32 eps) {
    for (i32 b = 0; b < batch; b++) {
        const f32* x_row = x + b * hidden_dim;
        f32* out_row = out + b * hidden_dim;

#ifdef HAS_AVX2
        if (hidden_dim >= 8) {
            rmsnorm_row_avx2(x_row, gamma, out_row, hidden_dim, eps);
            continue;
        }
#endif
        f32 sq_sum = 0.0f;
        for (i32 i = 0; i < hidden_dim; i++) {
            sq_sum += x_row[i] * x_row[i];
        }
        f32 rms = sqrtf(sq_sum / (f32)hidden_dim + eps);

        for (i32 i = 0; i < hidden_dim; i++) {
            out_row[i] = (x_row[i] / rms) * gamma[i];
        }
    }
}

void softmax_f32(const f32* x, f32* out, i32 batch, i32 dim) {
    for (i32 b = 0; b < batch; b++) {
        const f32* x_row = x + b * dim;
        f32* out_row = out + b * dim;

#ifdef HAS_AVX2
        if (dim >= 8) {
            softmax_row_avx2(x_row, out_row, dim);
            continue;
        }
#endif
        f32 max_val = vec_max_f32(x_row, dim);

        f32 sum = 0.0f;
        for (i32 i = 0; i < dim; i++) {
            f32 exp_val = expf(x_row[i] - max_val);
            out_row[i] = exp_val;
            sum += exp_val;
        }

        f32 inv_sum = 1.0f / sum;
        for (i32 i = 0; i < dim; i++) {
            out_row[i] *= inv_sum;
        }
    }
}

// ===== EMBEDDING =====

void embedding_lookup(const f32* table, const i32* token_ids, f32* out,
                      i32 batch, i32 seq_len, i32 vocab_size, i32 hidden_dim) {
    for (i32 b = 0; b < batch; b++) {
        for (i32 s = 0; s < seq_len; s++) {
            i32 token_id = token_ids[b * seq_len + s];
            const f32* embedding = table + token_id * hidden_dim;
            f32* out_ptr = out + (b * seq_len + s) * hidden_dim;
            memcpy(out_ptr, embedding, hidden_dim * sizeof(f32));
        }
    }
}

// ===== LOSS =====

f32 cross_entropy_loss(const f32* logits, const i32* targets,
                       i32 batch, i32 vocab_size) {
    f32 total_loss = 0.0f;

    for (i32 b = 0; b < batch; b++) {
        const f32* logit_row = logits + b * vocab_size;
        i32 target = targets[b];

        f32 max_val = vec_max_f32(logit_row, vocab_size);

        f32 sum = 0.0f;
        for (i32 i = 0; i < vocab_size; i++) {
            sum += expf(logit_row[i] - max_val);
        }

        f32 log_softmax_target = (logit_row[target] - max_val) - logf(sum);
        total_loss -= log_softmax_target;
    }

    return total_loss / (f32)batch;
}
