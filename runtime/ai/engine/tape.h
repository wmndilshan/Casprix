/*
 * Casprix AI optimized execution tape
 *
 * Reverse-mode AD with dynamic computation graph (tape).
 * Records forward operations, then replays backward using
 * existing backward kernels from backward.h.
 *
 * All tape nodes are arena-allocated for zero GC in training loops.
 * Gradients are accumulated (+=) to handle parameter sharing.
 */

#ifndef CPX_AI_ENGINE_TAPE_H
#define CPX_AI_ENGINE_TAPE_H

#include "../core/tensor.h"
#include "../kernels/cpu_ops.h"
#include "../kernels/grad_kernels.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Operation Types
 * ==========================================================================*/

typedef enum {
    AI_TAPE_OP_GEMM,            /* Y = X * W                     */
    AI_TAPE_OP_GEMM_BIAS,       /* Y = X * W + bias              */
    AI_TAPE_OP_LAYERNORM,       /* Y = LN(X, gamma, beta)        */
    AI_TAPE_OP_GELU,            /* Y = GELU(X)                   */
    AI_TAPE_OP_RELU,            /* Y = ReLU(X)                   */
    AI_TAPE_OP_SOFTMAX,         /* Y = softmax(X)                */
    AI_TAPE_OP_ADD,             /* Y = A + B                     */
    AI_TAPE_OP_SCALE,           /* Y = X * scalar                */
    AI_TAPE_OP_EMBEDDING,       /* Y = embedding_lookup(table, ids) */
    AI_TAPE_OP_CROSS_ENTROPY,   /* loss = CE(logits, targets)    */
    AI_TAPE_OP_TRANSPOSE,       /* Y = X^T                       */
    AI_TAPE_OP_ATTENTION,       /* Y = softmax(Q*K^T/sqrt(d))*V  */
    AI_TAPE_OP_VEC_ADD_BIAS,    /* Y[i] = X[i] + bias[i % dim]  */
} AiTapeOp;

/* ============================================================================
 * AiTapeValue - Tensor with gradient tracking
 * ==========================================================================*/

typedef struct AiTapeNode AiTapeNode;

typedef struct AiTapeValue {
    Tensor* data;               /* Forward value                          */
    Tensor* grad;               /* Accumulated gradient (NULL until needed) */
    bool requires_grad;         /* Whether to track gradients             */
    AiTapeNode* creator;         /* The op that created this (NULL=leaf)   */
    i32 id;                     /* Unique ID for debugging                */
} AiTapeValue;

/* ============================================================================
 * AiTapeNode - Records a single forward operation
 * ==========================================================================*/

#define TAPE_MAX_INPUTS 4
#define TAPE_MAX_SAVED  4

struct AiTapeNode {
    AiTapeOp op;

    /* Input/output tensors */
    AiTapeValue* inputs[TAPE_MAX_INPUTS];
    i32 num_inputs;
    AiTapeValue* output;

    /* Saved tensors for backward (copies of forward values needed by grad) */
    Tensor* saved[TAPE_MAX_SAVED];
    i32 num_saved;

    /* Shape metadata for backward dispatch */
    i32 M, K, N;               /* For GEMM: X=[M,K], W=[K,N]            */
    i32 batch, dim;             /* For layernorm/softmax                  */
    f32 eps;                    /* For layernorm epsilon                  */
    f32 scalar;                 /* For AI_TAPE_OP_SCALE                           */

    /* Attention-specific */
    i32 seq_len;
    i32 num_heads;
    i32 head_dim;

    /* Embedding-specific */
    const i32* token_ids;       /* Pointer to token ID array              */
    i32 vocab_size;

    /* Cross-entropy-specific */
    const i32* targets;         /* Pointer to target array                */
};

/* ============================================================================
 * AiTape - Dynamic computation graph
 * ==========================================================================*/

#define TAPE_INITIAL_CAPACITY 256
#define TAPE_INITIAL_TENSORS  128

typedef struct {
    /* AiTape entries (recorded ops) */
    AiTapeNode* entries;
    i32 count;
    i32 capacity;

    /* Tracked GradTensors (for gradient allocation) */
    AiTapeValue** tensors;
    i32 tensor_count;
    i32 tensor_capacity;

    /* TensorArena for all allocations (zero GC) */
    TensorArena* arena;

    /* State */
    bool recording;
    i32 next_tensor_id;
} AiTape;

/* ============================================================================
 * AiTape Lifecycle API
 * ==========================================================================*/

/* Create a new tape using the given arena for all allocations */
AiTape* ai_tape_create(TensorArena* arena);

/* Begin recording operations */
void ai_tape_begin(AiTape* tape);

/* Stop recording operations */
void ai_tape_end(AiTape* tape);

/* Reverse-mode backward pass: walk tape in reverse, dispatch to backward kernels */
void ai_tape_backward(AiTape* tape);

/* Zero all accumulated gradients */
void ai_tape_zero_grads(AiTape* tape);

/* Reset tape for next training step (resets arena, clears entries) */
void ai_tape_reset(AiTape* tape);

/* ============================================================================
 * Leaf Tensor Creation
 * ==========================================================================*/

/* Create a parameter tensor (requires_grad=true, leaf node) */
AiTapeValue* ai_tape_parameter(AiTape* tape, Tensor* data);

/* Create an input tensor (requires_grad=false, leaf node) */
AiTapeValue* ai_tape_input(AiTape* tape, Tensor* data);

/* Create a constant tensor from arena (requires_grad=false) */
AiTapeValue* ai_tape_constant(AiTape* tape, Tensor* data);

/* ============================================================================
 * Tracked Operations (record on tape + execute forward kernel)
 * ==========================================================================*/

/* GEMM: Y = X * W, X:[M,K] W:[K,N] Y:[M,N] */
AiTapeValue* ai_tape_gemm(AiTape* tape, AiTapeValue* X, AiTapeValue* W,
                     i32 M, i32 K, i32 N);

/* GEMM + bias: Y = X * W + bias, bias:[N] broadcast over rows */
AiTapeValue* ai_tape_gemm_bias(AiTape* tape, AiTapeValue* X, AiTapeValue* W,
                          AiTapeValue* bias, i32 M, i32 K, i32 N);

/* LayerNorm: Y = gamma * (X - mean) / sqrt(var + eps) + beta */
AiTapeValue* ai_tape_layernorm(AiTape* tape, AiTapeValue* x, AiTapeValue* gamma,
                          AiTapeValue* beta, i32 batch, i32 dim, f32 eps);

/* GELU activation */
AiTapeValue* ai_tape_gelu(AiTape* tape, AiTapeValue* x);

/* ReLU activation */
AiTapeValue* ai_tape_relu(AiTape* tape, AiTapeValue* x);

/* Softmax per row: Y[b,:] = softmax(X[b,:]) */
AiTapeValue* ai_tape_softmax(AiTape* tape, AiTapeValue* x, i32 batch, i32 dim);

/* Element-wise addition: Y = A + B */
AiTapeValue* ai_tape_add(AiTape* tape, AiTapeValue* a, AiTapeValue* b);

/* Scalar multiply: Y = X * scalar */
AiTapeValue* ai_tape_scale(AiTape* tape, AiTapeValue* x, f32 scalar);

/* Embedding lookup: Y = table[ids], table:[vocab,dim] ids:[batch*seq] Y:[batch*seq,dim] */
AiTapeValue* ai_tape_embedding(AiTape* tape, AiTapeValue* table, const i32* token_ids,
                          i32 batch, i32 seq_len, i32 dim);

/* Cross-entropy loss: loss = CE(softmax(logits), targets) */
AiTapeValue* ai_tape_cross_entropy(AiTape* tape, AiTapeValue* logits,
                              const i32* targets, i32 batch_seq, i32 vocab_size);

/* Fused multi-head attention: Y = softmax(Q*K^T/sqrt(d)) * V */
AiTapeValue* ai_tape_attention(AiTape* tape, AiTapeValue* x,
                          AiTapeValue* Wq, AiTapeValue* Wk,
                          AiTapeValue* Wv, AiTapeValue* Wo,
                          i32 batch, i32 seq_len, i32 num_heads, i32 head_dim);

/* Bias addition (broadcast): Y[i] = X[i] + bias[i % dim] */
AiTapeValue* ai_tape_bias_add(AiTape* tape, AiTapeValue* x, AiTapeValue* bias,
                         i32 rows, i32 cols);

/* ============================================================================
 * Utility
 * ==========================================================================*/

/* Print tape contents for debugging */
void ai_tape_print(AiTape* tape);

/* Get the name string for an op type */
const char* ai_tape_op_name(AiTapeOp op);

#ifdef __cplusplus
}
#endif

#endif /* CPX_AI_ENGINE_TAPE_H */
