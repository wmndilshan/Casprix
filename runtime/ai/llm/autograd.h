/*
 * LLM Runtime - Automatic Differentiation Engine
 *
 * Reverse-mode AD with dynamic computation graph (tape).
 * Records forward operations, then replays backward using
 * existing backward kernels from backward.h.
 *
 * All tape nodes are arena-allocated for zero GC in training loops.
 * Gradients are accumulated (+=) to handle parameter sharing.
 */

#ifndef LLM_AUTOGRAD_H
#define LLM_AUTOGRAD_H

#include "tensor.h"
#include "ops.h"
#include "backward.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Operation Types
 * ==========================================================================*/

typedef enum {
    OP_GEMM,            /* Y = X * W                     */
    OP_GEMM_BIAS,       /* Y = X * W + bias              */
    OP_LAYERNORM,       /* Y = LN(X, gamma, beta)        */
    OP_GELU,            /* Y = GELU(X)                   */
    OP_RELU,            /* Y = ReLU(X)                   */
    OP_SOFTMAX,         /* Y = softmax(X)                */
    OP_ADD,             /* Y = A + B                     */
    OP_SCALE,           /* Y = X * scalar                */
    OP_EMBEDDING,       /* Y = embedding_lookup(table, ids) */
    OP_CROSS_ENTROPY,   /* loss = CE(logits, targets)    */
    OP_TRANSPOSE,       /* Y = X^T                       */
    OP_ATTENTION,       /* Y = softmax(Q*K^T/sqrt(d))*V  */
    OP_VEC_ADD_BIAS,    /* Y[i] = X[i] + bias[i % dim]  */
} OpType;

/* ============================================================================
 * GradTensor - Tensor with gradient tracking
 * ==========================================================================*/

typedef struct TapeEntry TapeEntry;

typedef struct GradTensor {
    Tensor* data;               /* Forward value                          */
    Tensor* grad;               /* Accumulated gradient (NULL until needed) */
    bool requires_grad;         /* Whether to track gradients             */
    TapeEntry* creator;         /* The op that created this (NULL=leaf)   */
    i32 id;                     /* Unique ID for debugging                */
} GradTensor;

/* ============================================================================
 * TapeEntry - Records a single forward operation
 * ==========================================================================*/

#define TAPE_MAX_INPUTS 4
#define TAPE_MAX_SAVED  4

struct TapeEntry {
    OpType op;

    /* Input/output tensors */
    GradTensor* inputs[TAPE_MAX_INPUTS];
    i32 num_inputs;
    GradTensor* output;

    /* Saved tensors for backward (copies of forward values needed by grad) */
    Tensor* saved[TAPE_MAX_SAVED];
    i32 num_saved;

    /* Shape metadata for backward dispatch */
    i32 M, K, N;               /* For GEMM: X=[M,K], W=[K,N]            */
    i32 batch, dim;             /* For layernorm/softmax                  */
    f32 eps;                    /* For layernorm epsilon                  */
    f32 scalar;                 /* For OP_SCALE                           */

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
 * Tape - Dynamic computation graph
 * ==========================================================================*/

#define TAPE_INITIAL_CAPACITY 256
#define TAPE_INITIAL_TENSORS  128

typedef struct {
    /* Tape entries (recorded ops) */
    TapeEntry* entries;
    i32 count;
    i32 capacity;

    /* Tracked GradTensors (for gradient allocation) */
    GradTensor** tensors;
    i32 tensor_count;
    i32 tensor_capacity;

    /* Arena for all allocations (zero GC) */
    Arena* arena;

    /* State */
    bool recording;
    i32 next_tensor_id;
} Tape;

/* ============================================================================
 * Tape Lifecycle API
 * ==========================================================================*/

/* Create a new tape using the given arena for all allocations */
Tape* tape_create(Arena* arena);

/* Begin recording operations */
void tape_begin(Tape* tape);

/* Stop recording operations */
void tape_end(Tape* tape);

/* Reverse-mode backward pass: walk tape in reverse, dispatch to backward kernels */
void tape_backward(Tape* tape);

/* Zero all accumulated gradients */
void tape_zero_grads(Tape* tape);

/* Reset tape for next training step (resets arena, clears entries) */
void tape_reset(Tape* tape);

/* ============================================================================
 * Leaf Tensor Creation
 * ==========================================================================*/

/* Create a parameter tensor (requires_grad=true, leaf node) */
GradTensor* ag_parameter(Tape* tape, Tensor* data);

/* Create an input tensor (requires_grad=false, leaf node) */
GradTensor* ag_input(Tape* tape, Tensor* data);

/* Create a constant tensor from arena (requires_grad=false) */
GradTensor* ag_constant(Tape* tape, Tensor* data);

/* ============================================================================
 * Tracked Operations (record on tape + execute forward kernel)
 * ==========================================================================*/

/* GEMM: Y = X * W, X:[M,K] W:[K,N] Y:[M,N] */
GradTensor* ag_gemm(Tape* tape, GradTensor* X, GradTensor* W,
                     i32 M, i32 K, i32 N);

/* GEMM + bias: Y = X * W + bias, bias:[N] broadcast over rows */
GradTensor* ag_gemm_bias(Tape* tape, GradTensor* X, GradTensor* W,
                          GradTensor* bias, i32 M, i32 K, i32 N);

/* LayerNorm: Y = gamma * (X - mean) / sqrt(var + eps) + beta */
GradTensor* ag_layernorm(Tape* tape, GradTensor* x, GradTensor* gamma,
                          GradTensor* beta, i32 batch, i32 dim, f32 eps);

/* GELU activation */
GradTensor* ag_gelu(Tape* tape, GradTensor* x);

/* ReLU activation */
GradTensor* ag_relu(Tape* tape, GradTensor* x);

/* Softmax per row: Y[b,:] = softmax(X[b,:]) */
GradTensor* ag_softmax(Tape* tape, GradTensor* x, i32 batch, i32 dim);

/* Element-wise addition: Y = A + B */
GradTensor* ag_add(Tape* tape, GradTensor* a, GradTensor* b);

/* Scalar multiply: Y = X * scalar */
GradTensor* ag_scale(Tape* tape, GradTensor* x, f32 scalar);

/* Embedding lookup: Y = table[ids], table:[vocab,dim] ids:[batch*seq] Y:[batch*seq,dim] */
GradTensor* ag_embedding(Tape* tape, GradTensor* table, const i32* token_ids,
                          i32 batch, i32 seq_len, i32 dim);

/* Cross-entropy loss: loss = CE(softmax(logits), targets) */
GradTensor* ag_cross_entropy(Tape* tape, GradTensor* logits,
                              const i32* targets, i32 batch_seq, i32 vocab_size);

/* Fused multi-head attention: Y = softmax(Q*K^T/sqrt(d)) * V */
GradTensor* ag_attention(Tape* tape, GradTensor* x,
                          GradTensor* Wq, GradTensor* Wk,
                          GradTensor* Wv, GradTensor* Wo,
                          i32 batch, i32 seq_len, i32 num_heads, i32 head_dim);

/* Bias addition (broadcast): Y[i] = X[i] + bias[i % dim] */
GradTensor* ag_bias_add(Tape* tape, GradTensor* x, GradTensor* bias,
                         i32 rows, i32 cols);

/* ============================================================================
 * Utility
 * ==========================================================================*/

/* Print tape contents for debugging */
void tape_print(Tape* tape);

/* Get the name string for an op type */
const char* op_type_name(OpType op);

#ifdef __cplusplus
}
#endif

#endif /* LLM_AUTOGRAD_H */
