/*
 * LLM Runtime - Backward Pass (Gradient Computation)
 * 
 * Manual backpropagation for training transformer models
 */

#ifndef LLM_BACKWARD_H
#define LLM_BACKWARD_H

#include "tensor.h"
#include "ops.h"

// ===== LOSS GRADIENTS =====

// Cross-entropy loss backward
// logits: [batch*seq, vocab_size]
// targets: [batch*seq] (token IDs)
// grad_logits: [batch*seq, vocab_size] (output)
void cross_entropy_backward(
    const f32* logits,
    const i32* targets,
    f32* grad_logits,
    i32 batch_seq,
    i32 vocab_size
);

// ===== OPERATION GRADIENTS =====

// GEMM backward: Y = X * W
// X: [M, K], W: [K, N], Y: [M, N]
// grad_Y: [M, N] (incoming gradient)
// grad_X: [M, K] (output, can be NULL)
// grad_W: [K, N] (output, can be NULL)
void gemm_backward(
    const f32* X,
    const f32* W,
    const f32* grad_Y,
    f32* grad_X,
    f32* grad_W,
    i32 M, i32 K, i32 N
);

// LayerNorm backward
// x: [batch, dim]
// gamma: [dim]
// grad_out: [batch, dim] (incoming gradient)
// grad_x: [batch, dim] (output)
// grad_gamma: [dim] (output)
// grad_beta: [dim] (output)
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
);

// GELU backward
// x: [n]
// grad_out: [n] (incoming gradient)
// grad_x: [n] (output)
void gelu_backward(
    const f32* x,
    const f32* grad_out,
    f32* grad_x,
    i32 n
);

// ReLU backward
// x: [n]
// grad_out: [n] (incoming gradient)
// grad_x: [n] (output)
void relu_backward(
    const f32* x,
    const f32* grad_out,
    f32* grad_x,
    i32 n
);

// Softmax backward
// NOTE: For attention, we need special handling
// softmax_out: [batch, dim] (output of softmax forward)
// grad_out: [batch, dim] (incoming gradient)
// grad_x: [batch, dim] (output)
void softmax_backward(
    const f32* softmax_out,
    const f32* grad_out,
    f32* grad_x,
    i32 batch,
    i32 dim
);

// Element-wise addition backward (trivial but included for completeness)
// grad_out: [n]
// grad_a: [n] (output)
// grad_b: [n] (output)
static inline void add_backward(const f32* grad_out, f32* grad_a, f32* grad_b, i32 n) {
    // grad_a = grad_out, grad_b = grad_out
    for (i32 i = 0; i < n; i++) {
        if (grad_a) grad_a[i] += grad_out[i];
        if (grad_b) grad_b[i] += grad_out[i];
    }
}

// ===== EMBEDDING GRADIENTS =====

// Embedding lookup backward
// grad_out: [batch, seq_len, hidden_dim] (incoming gradient)
// token_ids: [batch, seq_len]
// grad_embedding: [vocab_size, hidden_dim] (output, accumulated)
void embedding_backward(
    const f32* grad_out,
    const i32* token_ids,
    f32* grad_embedding,
    i32 batch,
    i32 seq_len,
    i32 vocab_size,
    i32 hidden_dim
);

#endif // LLM_BACKWARD_H
