/*
 * LLM Runtime - Backward Pass Implementation
 *
 * Dispatches to AVX2 NASM kernels when available for activation
 * and softmax backward passes. GEMM backward uses the optimized
 * gemm_f32() which itself dispatches to 6x16 micro-kernel.
 */

#include "backward.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#define PI 3.14159265358979323846f

// ===== LOSS GRADIENTS =====

void cross_entropy_backward(
    const f32* logits,
    const i32* targets,
    f32* grad_logits,
    i32 batch_seq,
    i32 vocab_size
) {
#ifdef HAS_AVX2
    // Use fused AVX2 kernel: computes softmax then subtracts 1 at target
    // First compute softmax per row
    for (i32 b = 0; b < batch_seq; b++) {
        const f32* logits_b = logits + b * vocab_size;
        f32* grad_b = grad_logits + b * vocab_size;

        // Softmax into grad buffer
        softmax_row_avx2(logits_b, grad_b, vocab_size);

        // Subtract 1 at target position
        grad_b[targets[b]] -= 1.0f;
    }
#else
    for (i32 b = 0; b < batch_seq; b++) {
        const f32* logits_b = logits + b * vocab_size;
        f32* grad_b = grad_logits + b * vocab_size;
        i32 target = targets[b];

        f32 max_logit = logits_b[0];
        for (i32 i = 1; i < vocab_size; i++) {
            if (logits_b[i] > max_logit) max_logit = logits_b[i];
        }

        f32 sum_exp = 0.0f;
        for (i32 i = 0; i < vocab_size; i++) {
            sum_exp += expf(logits_b[i] - max_logit);
        }

        for (i32 i = 0; i < vocab_size; i++) {
            f32 prob = expf(logits_b[i] - max_logit) / sum_exp;
            grad_b[i] = prob - (i == target ? 1.0f : 0.0f);
        }
    }
#endif
}

// ===== OPERATION GRADIENTS =====

void gemm_backward(
    const f32* X,
    const f32* W,
    const f32* grad_Y,
    f32* grad_X,
    f32* grad_W,
    i32 M, i32 K, i32 N
) {
    // Forward: Y = X * W
    // grad_X = grad_Y * W^T  -> [M,N] * [N,K] -> [M,K]
    // grad_W = X^T * grad_Y  -> [K,M] * [M,N] -> [K,N]

    // Both are GEMM operations but with transposed operands.
    // We implement them using the optimized gemm_f32 with explicit transposes
    // for large matrices, or direct loops for small ones.

    if (grad_X) {
        memset(grad_X, 0, M * K * sizeof(f32));
        // grad_X[i,k] = sum_j grad_Y[i,j] * W[k,j]  (W transposed)
        for (i32 i = 0; i < M; i++) {
            for (i32 k = 0; k < K; k++) {
                f32 sum = 0.0f;
#ifdef HAS_AVX2
                if (N >= 8) {
                    // Use dot product kernel for each (i,k) pair
                    sum = vec_dot_f32_avx2(
                        grad_Y + i * N,   // grad_Y[i, :]
                        W + k * N,         // W[k, :] (row k of W = column k of W^T)
                        N
                    );
                } else
#endif
                {
                    for (i32 j = 0; j < N; j++) {
                        sum += grad_Y[i * N + j] * W[k * N + j];
                    }
                }
                grad_X[i * K + k] = sum;
            }
        }
    }

    if (grad_W) {
        memset(grad_W, 0, K * N * sizeof(f32));
        // grad_W[k,j] = sum_i X[i,k] * grad_Y[i,j]  (X transposed)
        for (i32 k = 0; k < K; k++) {
            for (i32 j = 0; j < N; j++) {
                f32 sum = 0.0f;
                for (i32 i = 0; i < M; i++) {
                    sum += X[i * K + k] * grad_Y[i * N + j];
                }
                grad_W[k * N + j] = sum;
            }
        }
    }
}

void layernorm_backward(
    const f32* x,
    const f32* gamma,
    const f32* grad_out,
    f32* grad_x,
    f32* grad_gamma,
    f32* grad_beta,
    i32 batch,
    i32 dim,
    f32 eps
) {
    for (i32 b = 0; b < batch; b++) {
        const f32* x_b = x + b * dim;
        const f32* grad_out_b = grad_out + b * dim;
        f32* grad_x_b = grad_x + b * dim;

        // Recompute mean and variance
        f32 mean = 0.0f;
        for (i32 i = 0; i < dim; i++) {
            mean += x_b[i];
        }
        mean /= dim;

        f32 var = 0.0f;
        for (i32 i = 0; i < dim; i++) {
            f32 diff = x_b[i] - mean;
            var += diff * diff;
        }
        var /= dim;

        f32 std_inv = 1.0f / sqrtf(var + eps);

        // Gradient w.r.t. gamma and beta
        for (i32 i = 0; i < dim; i++) {
            f32 x_norm = (x_b[i] - mean) * std_inv;
            grad_gamma[i] += grad_out_b[i] * x_norm;
            grad_beta[i] += grad_out_b[i];
        }

        // Gradient w.r.t. input
        f32 grad_x_norm_sum = 0.0f;
        f32 grad_x_norm_dot_x_norm = 0.0f;

        for (i32 i = 0; i < dim; i++) {
            f32 x_norm = (x_b[i] - mean) * std_inv;
            f32 grad_x_norm = grad_out_b[i] * gamma[i];

            grad_x_norm_sum += grad_x_norm;
            grad_x_norm_dot_x_norm += grad_x_norm * x_norm;
        }

        for (i32 i = 0; i < dim; i++) {
            f32 x_norm = (x_b[i] - mean) * std_inv;
            f32 grad_x_norm = grad_out_b[i] * gamma[i];

            grad_x_b[i] = std_inv / dim * (
                dim * grad_x_norm - grad_x_norm_sum - x_norm * grad_x_norm_dot_x_norm
            );
        }
    }
}

void gelu_backward(
    const f32* x,
    const f32* grad_out,
    f32* grad_x,
    i32 n
) {
#ifdef HAS_AVX2
    if (n >= 8) {
        // Note: AVX2 kernel has reversed parameter order (grad_out, x, grad_in)
        gelu_backward_avx2(grad_out, x, grad_x, n);
        return;
    }
#endif
    const f32 sqrt_2_over_pi = sqrtf(2.0f / PI);
    const f32 coeff = 0.044715f;

    for (i32 i = 0; i < n; i++) {
        f32 x_val = x[i];
        f32 x_cubed = x_val * x_val * x_val;

        f32 tanh_arg = sqrt_2_over_pi * (x_val + coeff * x_cubed);
        f32 tanh_val = tanhf(tanh_arg);

        f32 sech_sq = 1.0f - tanh_val * tanh_val;
        f32 dtanh_dx = sqrt_2_over_pi * (1.0f + 3.0f * coeff * x_val * x_val) * sech_sq;

        f32 gelu_grad = 0.5f * (1.0f + tanh_val + x_val * dtanh_dx);

        grad_x[i] = grad_out[i] * gelu_grad;
    }
}

void relu_backward(
    const f32* x,
    const f32* grad_out,
    f32* grad_x,
    i32 n
) {
#ifdef HAS_AVX2
    if (n >= 8) {
        relu_backward_avx2(grad_out, x, grad_x, n);
        return;
    }
#endif
    for (i32 i = 0; i < n; i++) {
        grad_x[i] = (x[i] > 0.0f) ? grad_out[i] : 0.0f;
    }
}

void softmax_backward(
    const f32* softmax_out,
    const f32* grad_out,
    f32* grad_x,
    i32 batch,
    i32 dim
) {
    for (i32 b = 0; b < batch; b++) {
        const f32* y_b = softmax_out + b * dim;
        const f32* grad_b = grad_out + b * dim;
        f32* grad_x_b = grad_x + b * dim;

#ifdef HAS_AVX2
        if (dim >= 8) {
            softmax_backward_avx2(grad_b, y_b, grad_x_b, dim);
            continue;
        }
#endif
        f32 dot = 0.0f;
        for (i32 i = 0; i < dim; i++) {
            dot += y_b[i] * grad_b[i];
        }

        for (i32 i = 0; i < dim; i++) {
            grad_x_b[i] = y_b[i] * (grad_b[i] - dot);
        }
    }
}

// ===== EMBEDDING GRADIENTS =====

void embedding_backward(
    const f32* grad_out,
    const i32* token_ids,
    f32* grad_embedding,
    i32 batch,
    i32 seq_len,
    i32 vocab_size,
    i32 hidden_dim
) {
    for (i32 b = 0; b < batch; b++) {
        for (i32 s = 0; s < seq_len; s++) {
            i32 token_id = token_ids[b * seq_len + s];

            if (token_id < 0 || token_id >= vocab_size) {
                fprintf(stderr, "Warning: Invalid token ID %d at batch %d, seq %d\n", token_id, b, s);
                continue;
            }

            const f32* grad_b_s = grad_out + (b * seq_len + s) * hidden_dim;
            f32* grad_emb = grad_embedding + token_id * hidden_dim;

            // Accumulate gradient using AVX2 kernel
#ifdef HAS_AVX2
            if (hidden_dim >= 8) {
                vec_acc_f32_avx2(grad_emb, grad_b_s, hidden_dim);
                continue;
            }
#endif
            for (i32 d = 0; d < hidden_dim; d++) {
                grad_emb[d] += grad_b_s[d];
            }
        }
    }
}
