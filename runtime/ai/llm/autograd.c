/*
 * LLM Runtime - Automatic Differentiation Engine
 *
 * Reverse-mode AD implementation with dynamic tape recording.
 * All allocations go through the arena — zero malloc in the hot path.
 */

#include "autograd.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ============================================================================
 * Internal Helpers
 * ==========================================================================*/

static GradTensor* tape_alloc_grad_tensor(Tape* tape) {
    GradTensor* gt = (GradTensor*)arena_alloc(tape->arena, sizeof(GradTensor), 8);
    if (!gt) return NULL;
    memset(gt, 0, sizeof(GradTensor));
    gt->id = tape->next_tensor_id++;

    /* Track it */
    if (tape->tensor_count >= tape->tensor_capacity) {
        i32 new_cap = tape->tensor_capacity * 2;
        GradTensor** new_arr = (GradTensor**)arena_alloc(
            tape->arena, new_cap * sizeof(GradTensor*), 8);
        if (!new_arr) return NULL;
        memcpy(new_arr, tape->tensors, tape->tensor_count * sizeof(GradTensor*));
        tape->tensors = new_arr;
        tape->tensor_capacity = new_cap;
    }
    tape->tensors[tape->tensor_count++] = gt;
    return gt;
}

static TapeEntry* tape_alloc_entry(Tape* tape) {
    if (tape->count >= tape->capacity) {
        i32 new_cap = tape->capacity * 2;
        TapeEntry* new_arr = (TapeEntry*)arena_alloc(
            tape->arena, new_cap * sizeof(TapeEntry), 8);
        if (!new_arr) return NULL;
        memcpy(new_arr, tape->entries, tape->count * sizeof(TapeEntry));
        tape->entries = new_arr;
        tape->capacity = new_cap;
    }
    TapeEntry* entry = &tape->entries[tape->count++];
    memset(entry, 0, sizeof(TapeEntry));
    return entry;
}

/* Allocate a tensor from the tape's arena */
static Tensor* tape_alloc_tensor(Tape* tape, i32 ndim, const i32* shape) {
    return arena_alloc_tensor(tape->arena, ndim, shape);
}

/* Clone tensor data into arena (for saving forward values) */
static Tensor* tape_save_tensor(Tape* tape, const Tensor* src) {
    Tensor* dst = tape_alloc_tensor(tape, src->ndim, src->shape);
    if (dst && src->data) {
        memcpy(dst->data, src->data, src->size * sizeof(f32));
    }
    return dst;
}

/* Ensure a GradTensor has a gradient buffer allocated */
static void ensure_grad(Tape* tape, GradTensor* gt) {
    if (!gt->grad && gt->requires_grad) {
        gt->grad = tape_alloc_tensor(tape, gt->data->ndim, gt->data->shape);
        if (gt->grad) {
            tensor_zeros(gt->grad);
        }
    }
}

/* Accumulate gradient: dst += src */
static void grad_accumulate(f32* dst, const f32* src, i32 n) {
#ifdef HAS_AVX2
    if (n >= 8) {
        vec_acc_f32_avx2(dst, src, n);
        return;
    }
#endif
    for (i32 i = 0; i < n; i++) {
        dst[i] += src[i];
    }
}

/* ============================================================================
 * Tape Lifecycle
 * ==========================================================================*/

Tape* tape_create(Arena* arena) {
    Tape* tape = (Tape*)arena_alloc(arena, sizeof(Tape), 8);
    if (!tape) return NULL;
    memset(tape, 0, sizeof(Tape));

    tape->arena = arena;
    tape->recording = false;
    tape->next_tensor_id = 0;

    /* Pre-allocate entry and tensor arrays */
    tape->capacity = TAPE_INITIAL_CAPACITY;
    tape->entries = (TapeEntry*)arena_alloc(arena,
        tape->capacity * sizeof(TapeEntry), 8);

    tape->tensor_capacity = TAPE_INITIAL_TENSORS;
    tape->tensors = (GradTensor**)arena_alloc(arena,
        tape->tensor_capacity * sizeof(GradTensor*), 8);

    if (!tape->entries || !tape->tensors) return NULL;

    tape->count = 0;
    tape->tensor_count = 0;

    return tape;
}

void tape_begin(Tape* tape) {
    if (!tape) return;
    tape->recording = true;
}

void tape_end(Tape* tape) {
    if (!tape) return;
    tape->recording = false;
}

void tape_zero_grads(Tape* tape) {
    if (!tape) return;
    for (i32 i = 0; i < tape->tensor_count; i++) {
        GradTensor* gt = tape->tensors[i];
        if (gt->grad) {
            tensor_zeros(gt->grad);
        }
    }
}

void tape_reset(Tape* tape) {
    if (!tape) return;
    tape->count = 0;
    tape->tensor_count = 0;
    tape->next_tensor_id = 0;
    tape->recording = false;
    /* Arena is reset externally by the caller (arena_reset) */
}

/* ============================================================================
 * Backward Pass Dispatch
 * ==========================================================================*/

static void backward_gemm(TapeEntry* entry, Tape* tape) {
    /* Forward: Y = X * W, X:[M,K], W:[K,N], Y:[M,N] */
    GradTensor* X_gt = entry->inputs[0];
    GradTensor* W_gt = entry->inputs[1];
    GradTensor* Y_gt = entry->output;

    if (!Y_gt->grad) return;

    /* Saved: X data, W data */
    Tensor* X_saved = entry->saved[0];
    Tensor* W_saved = entry->saved[1];
    i32 M = entry->M, K = entry->K, N = entry->N;

    /* grad_X = grad_Y * W^T */
    f32* grad_X_buf = NULL;
    if (X_gt->requires_grad) {
        ensure_grad(tape, X_gt);
        Tensor* tmp = tape_alloc_tensor(tape, 2, (i32[]){M, K});
        gemm_backward(X_saved->data, W_saved->data, Y_gt->grad->data,
                       tmp->data, NULL, M, K, N);
        grad_accumulate(X_gt->grad->data, tmp->data, M * K);
    }

    /* grad_W = X^T * grad_Y */
    if (W_gt->requires_grad) {
        ensure_grad(tape, W_gt);
        Tensor* tmp = tape_alloc_tensor(tape, 2, (i32[]){K, N});
        gemm_backward(X_saved->data, W_saved->data, Y_gt->grad->data,
                       NULL, tmp->data, M, K, N);
        grad_accumulate(W_gt->grad->data, tmp->data, K * N);
    }
}

static void backward_gemm_bias(TapeEntry* entry, Tape* tape) {
    /* Forward: Y = X * W + bias */
    GradTensor* X_gt = entry->inputs[0];
    GradTensor* W_gt = entry->inputs[1];
    GradTensor* bias_gt = entry->inputs[2];
    GradTensor* Y_gt = entry->output;

    if (!Y_gt->grad) return;

    Tensor* X_saved = entry->saved[0];
    Tensor* W_saved = entry->saved[1];
    i32 M = entry->M, K = entry->K, N = entry->N;

    /* GEMM gradients (same as OP_GEMM) */
    if (X_gt->requires_grad) {
        ensure_grad(tape, X_gt);
        Tensor* tmp = tape_alloc_tensor(tape, 2, (i32[]){M, K});
        gemm_backward(X_saved->data, W_saved->data, Y_gt->grad->data,
                       tmp->data, NULL, M, K, N);
        grad_accumulate(X_gt->grad->data, tmp->data, M * K);
    }

    if (W_gt->requires_grad) {
        ensure_grad(tape, W_gt);
        Tensor* tmp = tape_alloc_tensor(tape, 2, (i32[]){K, N});
        gemm_backward(X_saved->data, W_saved->data, Y_gt->grad->data,
                       NULL, tmp->data, M, K, N);
        grad_accumulate(W_gt->grad->data, tmp->data, K * N);
    }

    /* Bias gradient: sum over rows */
    if (bias_gt->requires_grad) {
        ensure_grad(tape, bias_gt);
        for (i32 i = 0; i < M; i++) {
            grad_accumulate(bias_gt->grad->data,
                           Y_gt->grad->data + i * N, N);
        }
    }
}

static void backward_layernorm(TapeEntry* entry, Tape* tape) {
    /* Forward: Y = LN(X, gamma, beta) */
    GradTensor* X_gt = entry->inputs[0];
    GradTensor* gamma_gt = entry->inputs[1];
    GradTensor* beta_gt = entry->inputs[2];
    GradTensor* Y_gt = entry->output;

    if (!Y_gt->grad) return;

    Tensor* X_saved = entry->saved[0];
    i32 batch = entry->batch, dim = entry->dim;
    f32 eps = entry->eps;

    /* Allocate temp gradient buffers */
    Tensor* grad_x_tmp = tape_alloc_tensor(tape, 2, (i32[]){batch, dim});
    Tensor* grad_gamma_tmp = tape_alloc_tensor(tape, 1, (i32[]){dim});
    Tensor* grad_beta_tmp = tape_alloc_tensor(tape, 1, (i32[]){dim});
    tensor_zeros(grad_gamma_tmp);
    tensor_zeros(grad_beta_tmp);

    layernorm_backward(X_saved->data, gamma_gt->data->data,
                       Y_gt->grad->data, grad_x_tmp->data,
                       grad_gamma_tmp->data, grad_beta_tmp->data,
                       batch, dim, eps);

    if (X_gt->requires_grad) {
        ensure_grad(tape, X_gt);
        grad_accumulate(X_gt->grad->data, grad_x_tmp->data, batch * dim);
    }
    if (gamma_gt->requires_grad) {
        ensure_grad(tape, gamma_gt);
        grad_accumulate(gamma_gt->grad->data, grad_gamma_tmp->data, dim);
    }
    if (beta_gt->requires_grad) {
        ensure_grad(tape, beta_gt);
        grad_accumulate(beta_gt->grad->data, grad_beta_tmp->data, dim);
    }
}

static void backward_gelu(TapeEntry* entry, Tape* tape) {
    GradTensor* X_gt = entry->inputs[0];
    GradTensor* Y_gt = entry->output;

    if (!Y_gt->grad || !X_gt->requires_grad) return;

    Tensor* X_saved = entry->saved[0];
    i32 n = (i32)X_saved->size;

    ensure_grad(tape, X_gt);
    Tensor* tmp = tape_alloc_tensor(tape, 1, (i32[]){n});
    gelu_backward(X_saved->data, Y_gt->grad->data, tmp->data, n);
    grad_accumulate(X_gt->grad->data, tmp->data, n);
}

static void backward_relu(TapeEntry* entry, Tape* tape) {
    GradTensor* X_gt = entry->inputs[0];
    GradTensor* Y_gt = entry->output;

    if (!Y_gt->grad || !X_gt->requires_grad) return;

    Tensor* X_saved = entry->saved[0];
    i32 n = (i32)X_saved->size;

    ensure_grad(tape, X_gt);
    Tensor* tmp = tape_alloc_tensor(tape, 1, (i32[]){n});
    relu_backward(X_saved->data, Y_gt->grad->data, tmp->data, n);
    grad_accumulate(X_gt->grad->data, tmp->data, n);
}

static void backward_softmax(TapeEntry* entry, Tape* tape) {
    GradTensor* X_gt = entry->inputs[0];
    GradTensor* Y_gt = entry->output;

    if (!Y_gt->grad || !X_gt->requires_grad) return;

    /* saved[0] = softmax output */
    Tensor* softmax_out = entry->saved[0];
    i32 batch = entry->batch, dim = entry->dim;

    ensure_grad(tape, X_gt);
    Tensor* tmp = tape_alloc_tensor(tape, 2, (i32[]){batch, dim});
    softmax_backward(softmax_out->data, Y_gt->grad->data, tmp->data, batch, dim);
    grad_accumulate(X_gt->grad->data, tmp->data, batch * dim);
}

static void backward_add(TapeEntry* entry, Tape* tape) {
    GradTensor* A_gt = entry->inputs[0];
    GradTensor* B_gt = entry->inputs[1];
    GradTensor* Y_gt = entry->output;

    if (!Y_gt->grad) return;

    i32 n = (i32)Y_gt->data->size;

    if (A_gt->requires_grad) {
        ensure_grad(tape, A_gt);
        grad_accumulate(A_gt->grad->data, Y_gt->grad->data, n);
    }
    if (B_gt->requires_grad) {
        ensure_grad(tape, B_gt);
        grad_accumulate(B_gt->grad->data, Y_gt->grad->data, n);
    }
}

static void backward_scale(TapeEntry* entry, Tape* tape) {
    GradTensor* X_gt = entry->inputs[0];
    GradTensor* Y_gt = entry->output;

    if (!Y_gt->grad || !X_gt->requires_grad) return;

    i32 n = (i32)Y_gt->data->size;
    f32 scalar = entry->scalar;

    ensure_grad(tape, X_gt);
    /* grad_X += scalar * grad_Y */
    for (i32 i = 0; i < n; i++) {
        X_gt->grad->data[i] += scalar * Y_gt->grad->data[i];
    }
}

static void backward_embedding(TapeEntry* entry, Tape* tape) {
    GradTensor* table_gt = entry->inputs[0];
    GradTensor* Y_gt = entry->output;

    if (!Y_gt->grad || !table_gt->requires_grad) return;

    ensure_grad(tape, table_gt);

    /* entry->batch is batch*seq_len for embedding */
    i32 batch = entry->batch;
    i32 dim = entry->dim;

    /* Accumulate gradients back into embedding table */
    embedding_backward(Y_gt->grad->data, entry->token_ids,
                       table_gt->grad->data,
                       1, batch, entry->vocab_size, dim);
}

static void backward_cross_entropy(TapeEntry* entry, Tape* tape) {
    GradTensor* logits_gt = entry->inputs[0];
    GradTensor* Y_gt = entry->output;

    if (!logits_gt->requires_grad) return;

    ensure_grad(tape, logits_gt);

    i32 batch_seq = entry->batch;
    i32 vocab_size = entry->vocab_size;

    /* Cross-entropy backward computes softmax - one_hot(target) */
    Tensor* tmp = tape_alloc_tensor(tape, 2, (i32[]){batch_seq, vocab_size});
    cross_entropy_backward(entry->saved[0]->data, entry->targets,
                           tmp->data, batch_seq, vocab_size);

    /* Scale by 1/batch_seq for mean loss */
    f32 scale = 1.0f / (f32)batch_seq;
    for (i32 i = 0; i < batch_seq * vocab_size; i++) {
        tmp->data[i] *= scale;
    }

    grad_accumulate(logits_gt->grad->data, tmp->data, batch_seq * vocab_size);
}

static void backward_bias_add(TapeEntry* entry, Tape* tape) {
    /* Forward: Y[i] = X[i] + bias[i % cols] */
    GradTensor* X_gt = entry->inputs[0];
    GradTensor* bias_gt = entry->inputs[1];
    GradTensor* Y_gt = entry->output;

    if (!Y_gt->grad) return;

    i32 rows = entry->M;
    i32 cols = entry->N;

    if (X_gt->requires_grad) {
        ensure_grad(tape, X_gt);
        grad_accumulate(X_gt->grad->data, Y_gt->grad->data, rows * cols);
    }
    if (bias_gt->requires_grad) {
        ensure_grad(tape, bias_gt);
        /* Sum over rows */
        for (i32 r = 0; r < rows; r++) {
            grad_accumulate(bias_gt->grad->data,
                           Y_gt->grad->data + r * cols, cols);
        }
    }
}

static void backward_attention(TapeEntry* entry, Tape* tape) {
    /*
     * Fused attention forward:
     *   Q = X * Wq, K = X * Wk, V = X * Wv
     *   scores = softmax(Q * K^T / sqrt(head_dim))
     *   attn_out = scores * V
     *   Y = attn_out * Wo
     *
     * saved[0] = X (input)
     * saved[1] = scores (post-softmax attention weights)
     * saved[2] = V (value projection)
     * saved[3] = attn_out (before Wo projection)
     */
    GradTensor* X_gt = entry->inputs[0];
    GradTensor* Wq_gt = entry->inputs[1];
    GradTensor* Wk_gt = entry->inputs[2];
    GradTensor* Wv_gt = entry->inputs[3];
    GradTensor* Y_gt = entry->output;

    if (!Y_gt->grad) return;

    Tensor* X_saved = entry->saved[0];
    Tensor* scores_saved = entry->saved[1];
    Tensor* V_saved = entry->saved[2];
    Tensor* attn_out_saved = entry->saved[3];

    i32 batch = entry->batch;
    i32 seq_len = entry->seq_len;
    i32 hidden_dim = entry->num_heads * entry->head_dim;
    i32 BS = batch * seq_len;

    /*
     * We need Wo from the GradTensor that was passed as the 5th input
     * in ag_attention. But we only have 4 input slots. The Wo weight
     * is stored as a separate GradTensor referenced via the entry.
     * We work around this by noting that the output Y_gt->creator
     * stores all the info we need. For Wo, we stored it in inputs
     * at index handled by the caller.
     *
     * Actually, ag_attention creates a OP_GEMM entry for the final Wo
     * projection separately. So this OP_ATTENTION only covers
     * Q/K/V projections + attention + output projection as one fused op.
     *
     * Let's implement the full backward:
     */

    /* We have 5 weight inputs but only 4 slots.
     * The Wo projection is handled as a separate entry.
     * See ag_attention implementation below.
     *
     * For OP_ATTENTION, we compute backward through:
     *   1. Q = X * Wq (gemm backward)
     *   2. K = X * Wk (gemm backward)
     *   3. V = X * Wv (gemm backward)
     *   4. scores = softmax(Q * K^T / sqrt(d))
     *   5. attn_out = scores * V
     *
     * The Wo projection grad is handled by a separate OP_GEMM entry.
     */

    /* Allocate temporary buffers */
    Tensor* grad_attn_out = tape_alloc_tensor(tape, 2, (i32[]){BS, hidden_dim});

    /* grad_attn_out is Y_gt->grad (since Wo is separate) */
    memcpy(grad_attn_out->data, Y_gt->grad->data, BS * hidden_dim * sizeof(f32));

    /* Per batch: backward through attention */
    for (i32 b = 0; b < batch; b++) {
        f32* grad_ao_b = grad_attn_out->data + b * seq_len * hidden_dim;
        f32* scores_b = scores_saved->data + b * seq_len * seq_len;
        f32* V_b = V_saved->data + b * seq_len * hidden_dim;

        /* 5. attn_out = scores * V -> [seq,seq] x [seq,hid] = [seq,hid] */
        /* grad_scores = grad_ao * V^T -> [seq,hid] x [hid,seq] = [seq,seq] */
        /* grad_V = scores^T * grad_ao -> [seq,seq] x [seq,hid] = [seq,hid] */
        Tensor* grad_scores = tape_alloc_tensor(tape, 2, (i32[]){seq_len, seq_len});
        Tensor* grad_V_b = tape_alloc_tensor(tape, 2, (i32[]){seq_len, hidden_dim});

        gemm_backward(scores_b, V_b, grad_ao_b,
                       grad_scores->data, grad_V_b->data,
                       seq_len, seq_len, hidden_dim);

        /* 4. softmax backward */
        Tensor* grad_pre_softmax = tape_alloc_tensor(tape, 2, (i32[]){seq_len, seq_len});
        softmax_backward(scores_b, grad_scores->data, grad_pre_softmax->data,
                         seq_len, seq_len);

        /* Scale backward (1/sqrt(head_dim)) */
        f32 scale = 1.0f / sqrtf((f32)entry->head_dim);
        for (i32 i = 0; i < seq_len * seq_len; i++) {
            grad_pre_softmax->data[i] *= scale;
        }

        /* grad_pre_softmax is gradient of Q * K^T */
        /* Q_b = X_b * Wq, K_b = X_b * Wk */
        f32* X_b = X_saved->data + b * seq_len * hidden_dim;

        /* Need Q_b and K_b for backward - recompute */
        Tensor* Q_b = tape_alloc_tensor(tape, 2, (i32[]){seq_len, hidden_dim});
        Tensor* K_b = tape_alloc_tensor(tape, 2, (i32[]){seq_len, hidden_dim});
        gemm_f32(X_b, Wq_gt->data->data, Q_b->data, seq_len, hidden_dim, hidden_dim);
        gemm_f32(X_b, Wk_gt->data->data, K_b->data, seq_len, hidden_dim, hidden_dim);

        /* scores = Q * K^T: grad_Q = grad_scores * K, grad_K = grad_scores^T * Q */
        Tensor* grad_Q_b = tape_alloc_tensor(tape, 2, (i32[]){seq_len, hidden_dim});
        Tensor* grad_K_b = tape_alloc_tensor(tape, 2, (i32[]){seq_len, hidden_dim});

        /* grad_Q = grad_pre_softmax * K -> [seq,seq] x [seq,hid] = [seq,hid] */
        gemm_f32(grad_pre_softmax->data, K_b->data, grad_Q_b->data,
                 seq_len, seq_len, hidden_dim);

        /* grad_K = grad_pre_softmax^T * Q -> [seq,seq] x [seq,hid] = [seq,hid] */
        Tensor* grad_pre_T = tape_alloc_tensor(tape, 2, (i32[]){seq_len, seq_len});
        transpose_f32(grad_pre_softmax->data, grad_pre_T->data, seq_len, seq_len);
        gemm_f32(grad_pre_T->data, Q_b->data, grad_K_b->data,
                 seq_len, seq_len, hidden_dim);

        /* Backward through Q = X * Wq */
        if (X_gt->requires_grad) {
            ensure_grad(tape, X_gt);
            Tensor* grad_X_q = tape_alloc_tensor(tape, 2, (i32[]){seq_len, hidden_dim});
            gemm_backward(X_b, Wq_gt->data->data, grad_Q_b->data,
                          grad_X_q->data, NULL, seq_len, hidden_dim, hidden_dim);
            grad_accumulate(X_gt->grad->data + b * seq_len * hidden_dim,
                           grad_X_q->data, seq_len * hidden_dim);
        }
        if (Wq_gt->requires_grad) {
            ensure_grad(tape, Wq_gt);
            Tensor* grad_Wq_tmp = tape_alloc_tensor(tape, 2, (i32[]){hidden_dim, hidden_dim});
            gemm_backward(X_b, Wq_gt->data->data, grad_Q_b->data,
                          NULL, grad_Wq_tmp->data, seq_len, hidden_dim, hidden_dim);
            grad_accumulate(Wq_gt->grad->data, grad_Wq_tmp->data,
                           hidden_dim * hidden_dim);
        }

        /* Backward through K = X * Wk */
        if (X_gt->requires_grad) {
            Tensor* grad_X_k = tape_alloc_tensor(tape, 2, (i32[]){seq_len, hidden_dim});
            gemm_backward(X_b, Wk_gt->data->data, grad_K_b->data,
                          grad_X_k->data, NULL, seq_len, hidden_dim, hidden_dim);
            grad_accumulate(X_gt->grad->data + b * seq_len * hidden_dim,
                           grad_X_k->data, seq_len * hidden_dim);
        }
        if (Wk_gt->requires_grad) {
            ensure_grad(tape, Wk_gt);
            Tensor* grad_Wk_tmp = tape_alloc_tensor(tape, 2, (i32[]){hidden_dim, hidden_dim});
            gemm_backward(X_b, Wk_gt->data->data, grad_K_b->data,
                          NULL, grad_Wk_tmp->data, seq_len, hidden_dim, hidden_dim);
            grad_accumulate(Wk_gt->grad->data, grad_Wk_tmp->data,
                           hidden_dim * hidden_dim);
        }

        /* Backward through V = X * Wv */
        if (X_gt->requires_grad) {
            Tensor* grad_X_v = tape_alloc_tensor(tape, 2, (i32[]){seq_len, hidden_dim});
            gemm_backward(X_b, Wv_gt->data->data, grad_V_b->data,
                          grad_X_v->data, NULL, seq_len, hidden_dim, hidden_dim);
            grad_accumulate(X_gt->grad->data + b * seq_len * hidden_dim,
                           grad_X_v->data, seq_len * hidden_dim);
        }
        if (Wv_gt->requires_grad) {
            ensure_grad(tape, Wv_gt);
            Tensor* grad_Wv_tmp = tape_alloc_tensor(tape, 2, (i32[]){hidden_dim, hidden_dim});
            gemm_backward(X_b, Wv_gt->data->data, grad_V_b->data,
                          NULL, grad_Wv_tmp->data, seq_len, hidden_dim, hidden_dim);
            grad_accumulate(Wv_gt->grad->data, grad_Wv_tmp->data,
                           hidden_dim * hidden_dim);
        }
    }
}

/* ============================================================================
 * tape_backward - Reverse-mode AD
 * ==========================================================================*/

void tape_backward(Tape* tape) {
    if (!tape || tape->count == 0) return;

    /* Seed: set gradient of final output (loss) to 1.0 */
    TapeEntry* last = &tape->entries[tape->count - 1];
    if (last->output) {
        GradTensor* loss = last->output;
        loss->grad = tape_alloc_tensor(tape, loss->data->ndim, loss->data->shape);
        if (loss->grad) {
            tensor_fill(loss->grad, 1.0f);
        }
    }

    /* Walk tape in reverse order */
    for (i32 i = tape->count - 1; i >= 0; i--) {
        TapeEntry* entry = &tape->entries[i];

        /* Skip if output has no gradient */
        if (!entry->output || !entry->output->grad) continue;

        switch (entry->op) {
            case OP_GEMM:           backward_gemm(entry, tape); break;
            case OP_GEMM_BIAS:      backward_gemm_bias(entry, tape); break;
            case OP_LAYERNORM:      backward_layernorm(entry, tape); break;
            case OP_GELU:           backward_gelu(entry, tape); break;
            case OP_RELU:           backward_relu(entry, tape); break;
            case OP_SOFTMAX:        backward_softmax(entry, tape); break;
            case OP_ADD:            backward_add(entry, tape); break;
            case OP_SCALE:          backward_scale(entry, tape); break;
            case OP_EMBEDDING:      backward_embedding(entry, tape); break;
            case OP_CROSS_ENTROPY:  backward_cross_entropy(entry, tape); break;
            case OP_VEC_ADD_BIAS:   backward_bias_add(entry, tape); break;
            case OP_ATTENTION:      backward_attention(entry, tape); break;
            case OP_TRANSPOSE:      /* handled implicitly */ break;
        }
    }
}

/* ============================================================================
 * Leaf Tensor Creation
 * ==========================================================================*/

GradTensor* ag_parameter(Tape* tape, Tensor* data) {
    GradTensor* gt = tape_alloc_grad_tensor(tape);
    if (!gt) return NULL;
    gt->data = data;
    gt->requires_grad = true;
    gt->creator = NULL;
    return gt;
}

GradTensor* ag_input(Tape* tape, Tensor* data) {
    GradTensor* gt = tape_alloc_grad_tensor(tape);
    if (!gt) return NULL;
    gt->data = data;
    gt->requires_grad = false;
    gt->creator = NULL;
    return gt;
}

GradTensor* ag_constant(Tape* tape, Tensor* data) {
    return ag_input(tape, data);
}

/* ============================================================================
 * Tracked Operations
 * ==========================================================================*/

GradTensor* ag_gemm(Tape* tape, GradTensor* X, GradTensor* W,
                     i32 M, i32 K, i32 N) {
    /* Execute forward: Y = X * W */
    Tensor* Y = tape_alloc_tensor(tape, 2, (i32[]){M, N});
    if (!Y) return NULL;
    gemm_f32(X->data->data, W->data->data, Y->data, M, K, N);

    GradTensor* Y_gt = tape_alloc_grad_tensor(tape);
    if (!Y_gt) return NULL;
    Y_gt->data = Y;
    Y_gt->requires_grad = (X->requires_grad || W->requires_grad);

    if (tape->recording && Y_gt->requires_grad) {
        TapeEntry* entry = tape_alloc_entry(tape);
        if (!entry) return Y_gt;
        entry->op = OP_GEMM;
        entry->inputs[0] = X;
        entry->inputs[1] = W;
        entry->num_inputs = 2;
        entry->output = Y_gt;
        entry->M = M; entry->K = K; entry->N = N;

        /* Save X and W for backward */
        entry->saved[0] = tape_save_tensor(tape, X->data);
        entry->saved[1] = tape_save_tensor(tape, W->data);
        entry->num_saved = 2;

        Y_gt->creator = entry;
    }

    return Y_gt;
}

GradTensor* ag_gemm_bias(Tape* tape, GradTensor* X, GradTensor* W,
                          GradTensor* bias, i32 M, i32 K, i32 N) {
    /* Execute forward: Y = X * W + bias */
    Tensor* Y = tape_alloc_tensor(tape, 2, (i32[]){M, N});
    if (!Y) return NULL;
    gemm_f32(X->data->data, W->data->data, Y->data, M, K, N);

    /* Add bias (broadcast over rows) */
    for (i32 i = 0; i < M; i++) {
        vec_add_f32(Y->data + i * N, bias->data->data, Y->data + i * N, N);
    }

    GradTensor* Y_gt = tape_alloc_grad_tensor(tape);
    if (!Y_gt) return NULL;
    Y_gt->data = Y;
    Y_gt->requires_grad = (X->requires_grad || W->requires_grad || bias->requires_grad);

    if (tape->recording && Y_gt->requires_grad) {
        TapeEntry* entry = tape_alloc_entry(tape);
        if (!entry) return Y_gt;
        entry->op = OP_GEMM_BIAS;
        entry->inputs[0] = X;
        entry->inputs[1] = W;
        entry->inputs[2] = bias;
        entry->num_inputs = 3;
        entry->output = Y_gt;
        entry->M = M; entry->K = K; entry->N = N;

        entry->saved[0] = tape_save_tensor(tape, X->data);
        entry->saved[1] = tape_save_tensor(tape, W->data);
        entry->num_saved = 2;

        Y_gt->creator = entry;
    }

    return Y_gt;
}

GradTensor* ag_layernorm(Tape* tape, GradTensor* x, GradTensor* gamma,
                          GradTensor* beta, i32 batch, i32 dim, f32 eps) {
    Tensor* Y = tape_alloc_tensor(tape, 2, (i32[]){batch, dim});
    if (!Y) return NULL;

    layernorm_f32(x->data->data, gamma->data->data, beta->data->data,
                  Y->data, batch, dim, eps);

    GradTensor* Y_gt = tape_alloc_grad_tensor(tape);
    if (!Y_gt) return NULL;
    Y_gt->data = Y;
    Y_gt->requires_grad = (x->requires_grad || gamma->requires_grad || beta->requires_grad);

    if (tape->recording && Y_gt->requires_grad) {
        TapeEntry* entry = tape_alloc_entry(tape);
        if (!entry) return Y_gt;
        entry->op = OP_LAYERNORM;
        entry->inputs[0] = x;
        entry->inputs[1] = gamma;
        entry->inputs[2] = beta;
        entry->num_inputs = 3;
        entry->output = Y_gt;
        entry->batch = batch; entry->dim = dim; entry->eps = eps;

        /* Save input X for backward */
        entry->saved[0] = tape_save_tensor(tape, x->data);
        entry->num_saved = 1;

        Y_gt->creator = entry;
    }

    return Y_gt;
}

GradTensor* ag_gelu(Tape* tape, GradTensor* x) {
    i32 n = (i32)x->data->size;
    Tensor* Y = tape_alloc_tensor(tape, x->data->ndim, x->data->shape);
    if (!Y) return NULL;

    gelu_f32(x->data->data, Y->data, n);

    GradTensor* Y_gt = tape_alloc_grad_tensor(tape);
    if (!Y_gt) return NULL;
    Y_gt->data = Y;
    Y_gt->requires_grad = x->requires_grad;

    if (tape->recording && Y_gt->requires_grad) {
        TapeEntry* entry = tape_alloc_entry(tape);
        if (!entry) return Y_gt;
        entry->op = OP_GELU;
        entry->inputs[0] = x;
        entry->num_inputs = 1;
        entry->output = Y_gt;

        entry->saved[0] = tape_save_tensor(tape, x->data);
        entry->num_saved = 1;

        Y_gt->creator = entry;
    }

    return Y_gt;
}

GradTensor* ag_relu(Tape* tape, GradTensor* x) {
    i32 n = (i32)x->data->size;
    Tensor* Y = tape_alloc_tensor(tape, x->data->ndim, x->data->shape);
    if (!Y) return NULL;

    relu_f32(x->data->data, Y->data, n);

    GradTensor* Y_gt = tape_alloc_grad_tensor(tape);
    if (!Y_gt) return NULL;
    Y_gt->data = Y;
    Y_gt->requires_grad = x->requires_grad;

    if (tape->recording && Y_gt->requires_grad) {
        TapeEntry* entry = tape_alloc_entry(tape);
        if (!entry) return Y_gt;
        entry->op = OP_RELU;
        entry->inputs[0] = x;
        entry->num_inputs = 1;
        entry->output = Y_gt;

        entry->saved[0] = tape_save_tensor(tape, x->data);
        entry->num_saved = 1;

        Y_gt->creator = entry;
    }

    return Y_gt;
}

GradTensor* ag_softmax(Tape* tape, GradTensor* x, i32 batch, i32 dim) {
    Tensor* Y = tape_alloc_tensor(tape, 2, (i32[]){batch, dim});
    if (!Y) return NULL;

    softmax_f32(x->data->data, Y->data, batch, dim);

    GradTensor* Y_gt = tape_alloc_grad_tensor(tape);
    if (!Y_gt) return NULL;
    Y_gt->data = Y;
    Y_gt->requires_grad = x->requires_grad;

    if (tape->recording && Y_gt->requires_grad) {
        TapeEntry* entry = tape_alloc_entry(tape);
        if (!entry) return Y_gt;
        entry->op = OP_SOFTMAX;
        entry->inputs[0] = x;
        entry->num_inputs = 1;
        entry->output = Y_gt;
        entry->batch = batch; entry->dim = dim;

        /* Save softmax output for backward */
        entry->saved[0] = tape_save_tensor(tape, Y);
        entry->num_saved = 1;

        Y_gt->creator = entry;
    }

    return Y_gt;
}

GradTensor* ag_add(Tape* tape, GradTensor* a, GradTensor* b) {
    i32 n = (i32)a->data->size;
    Tensor* Y = tape_alloc_tensor(tape, a->data->ndim, a->data->shape);
    if (!Y) return NULL;

    vec_add_f32(a->data->data, b->data->data, Y->data, n);

    GradTensor* Y_gt = tape_alloc_grad_tensor(tape);
    if (!Y_gt) return NULL;
    Y_gt->data = Y;
    Y_gt->requires_grad = (a->requires_grad || b->requires_grad);

    if (tape->recording && Y_gt->requires_grad) {
        TapeEntry* entry = tape_alloc_entry(tape);
        if (!entry) return Y_gt;
        entry->op = OP_ADD;
        entry->inputs[0] = a;
        entry->inputs[1] = b;
        entry->num_inputs = 2;
        entry->output = Y_gt;

        Y_gt->creator = entry;
    }

    return Y_gt;
}

GradTensor* ag_scale(Tape* tape, GradTensor* x, f32 scalar) {
    i32 n = (i32)x->data->size;
    Tensor* Y = tape_alloc_tensor(tape, x->data->ndim, x->data->shape);
    if (!Y) return NULL;

    vec_scale_f32(x->data->data, scalar, Y->data, n);

    GradTensor* Y_gt = tape_alloc_grad_tensor(tape);
    if (!Y_gt) return NULL;
    Y_gt->data = Y;
    Y_gt->requires_grad = x->requires_grad;

    if (tape->recording && Y_gt->requires_grad) {
        TapeEntry* entry = tape_alloc_entry(tape);
        if (!entry) return Y_gt;
        entry->op = OP_SCALE;
        entry->inputs[0] = x;
        entry->num_inputs = 1;
        entry->output = Y_gt;
        entry->scalar = scalar;

        Y_gt->creator = entry;
    }

    return Y_gt;
}

GradTensor* ag_embedding(Tape* tape, GradTensor* table, const i32* token_ids,
                          i32 batch, i32 seq_len, i32 dim) {
    i32 batch_seq = batch * seq_len;
    Tensor* Y = tape_alloc_tensor(tape, 2, (i32[]){batch_seq, dim});
    if (!Y) return NULL;

    embedding_lookup(table->data->data, token_ids, Y->data,
                     batch, seq_len, table->data->shape[0], dim);

    GradTensor* Y_gt = tape_alloc_grad_tensor(tape);
    if (!Y_gt) return NULL;
    Y_gt->data = Y;
    Y_gt->requires_grad = table->requires_grad;

    if (tape->recording && Y_gt->requires_grad) {
        TapeEntry* entry = tape_alloc_entry(tape);
        if (!entry) return Y_gt;
        entry->op = OP_EMBEDDING;
        entry->inputs[0] = table;
        entry->num_inputs = 1;
        entry->output = Y_gt;
        entry->token_ids = token_ids;
        entry->batch = batch_seq;
        entry->dim = dim;
        entry->vocab_size = table->data->shape[0];

        Y_gt->creator = entry;
    }

    return Y_gt;
}

GradTensor* ag_cross_entropy(Tape* tape, GradTensor* logits,
                              const i32* targets, i32 batch_seq, i32 vocab_size) {
    /* Compute scalar loss */
    f32 loss_val = cross_entropy_loss(logits->data->data, targets, batch_seq, vocab_size);

    Tensor* loss_tensor = tape_alloc_tensor(tape, 1, (i32[]){1});
    if (!loss_tensor) return NULL;
    loss_tensor->data[0] = loss_val;

    GradTensor* loss_gt = tape_alloc_grad_tensor(tape);
    if (!loss_gt) return NULL;
    loss_gt->data = loss_tensor;
    loss_gt->requires_grad = true; /* Loss is always the root */

    if (tape->recording) {
        TapeEntry* entry = tape_alloc_entry(tape);
        if (!entry) return loss_gt;
        entry->op = OP_CROSS_ENTROPY;
        entry->inputs[0] = logits;
        entry->num_inputs = 1;
        entry->output = loss_gt;
        entry->batch = batch_seq;
        entry->vocab_size = vocab_size;
        entry->targets = targets;

        /* Save logits for backward (needed for softmax computation) */
        entry->saved[0] = tape_save_tensor(tape, logits->data);
        entry->num_saved = 1;

        loss_gt->creator = entry;
    }

    return loss_gt;
}

GradTensor* ag_attention(Tape* tape, GradTensor* x,
                          GradTensor* Wq, GradTensor* Wk,
                          GradTensor* Wv, GradTensor* Wo,
                          i32 batch, i32 seq_len, i32 num_heads, i32 head_dim) {
    i32 hidden_dim = num_heads * head_dim;
    i32 BS = batch * seq_len;

    /* Q, K, V projections */
    Tensor* Q = tape_alloc_tensor(tape, 2, (i32[]){BS, hidden_dim});
    Tensor* K = tape_alloc_tensor(tape, 2, (i32[]){BS, hidden_dim});
    Tensor* V = tape_alloc_tensor(tape, 2, (i32[]){BS, hidden_dim});

    gemm_f32(x->data->data, Wq->data->data, Q->data, BS, hidden_dim, hidden_dim);
    gemm_f32(x->data->data, Wk->data->data, K->data, BS, hidden_dim, hidden_dim);
    gemm_f32(x->data->data, Wv->data->data, V->data, BS, hidden_dim, hidden_dim);

    /* Attention per batch */
    Tensor* scores_all = tape_alloc_tensor(tape, 3, (i32[]){batch, seq_len, seq_len});
    Tensor* attn_out = tape_alloc_tensor(tape, 2, (i32[]){BS, hidden_dim});
    f32 scale = 1.0f / sqrtf((f32)head_dim);

    for (i32 b = 0; b < batch; b++) {
        f32* Q_b = Q->data + b * seq_len * hidden_dim;
        f32* K_b = K->data + b * seq_len * hidden_dim;
        f32* V_b = V->data + b * seq_len * hidden_dim;

        /* K^T */
        Tensor* K_T = tape_alloc_tensor(tape, 2, (i32[]){hidden_dim, seq_len});
        transpose_f32(K_b, K_T->data, seq_len, hidden_dim);

        /* Scores = Q * K^T */
        f32* scores_b = scores_all->data + b * seq_len * seq_len;
        gemm_f32(Q_b, K_T->data, scores_b, seq_len, hidden_dim, seq_len);

        /* Scale */
        vec_scale_f32(scores_b, scale, scores_b, seq_len * seq_len);

        /* Softmax */
        softmax_f32(scores_b, scores_b, seq_len, seq_len);

        /* Attention output = scores * V */
        f32* ao_b = attn_out->data + b * seq_len * hidden_dim;
        gemm_f32(scores_b, V_b, ao_b, seq_len, seq_len, hidden_dim);
    }

    /* Output projection: Y = attn_out * Wo */
    Tensor* Y = tape_alloc_tensor(tape, 2, (i32[]){BS, hidden_dim});
    gemm_f32(attn_out->data, Wo->data->data, Y->data, BS, hidden_dim, hidden_dim);

    GradTensor* Y_gt = tape_alloc_grad_tensor(tape);
    if (!Y_gt) return NULL;
    Y_gt->data = Y;
    Y_gt->requires_grad = true;

    if (tape->recording) {
        /* Record the Wo projection as a separate GEMM entry */
        /* This simplifies backward since OP_ATTENTION handles Q/K/V */

        /* First: record the attention core (Q/K/V + softmax) */
        TapeEntry* attn_entry = tape_alloc_entry(tape);
        if (!attn_entry) return Y_gt;

        /* Wrap attn_out as a GradTensor for the GEMM chain */
        GradTensor* attn_out_gt = tape_alloc_grad_tensor(tape);
        attn_out_gt->data = attn_out;
        attn_out_gt->requires_grad = true;

        attn_entry->op = OP_ATTENTION;
        attn_entry->inputs[0] = x;
        attn_entry->inputs[1] = Wq;
        attn_entry->inputs[2] = Wk;
        attn_entry->inputs[3] = Wv;
        attn_entry->num_inputs = 4;
        attn_entry->output = attn_out_gt;
        attn_entry->batch = batch;
        attn_entry->seq_len = seq_len;
        attn_entry->num_heads = num_heads;
        attn_entry->head_dim = head_dim;

        /* Save tensors needed for backward */
        attn_entry->saved[0] = tape_save_tensor(tape, x->data);
        attn_entry->saved[1] = tape_save_tensor(tape, scores_all);
        attn_entry->saved[2] = tape_save_tensor(tape, V);
        attn_entry->saved[3] = tape_save_tensor(tape, attn_out);
        attn_entry->num_saved = 4;

        attn_out_gt->creator = attn_entry;

        /* Second: record the output projection GEMM */
        TapeEntry* wo_entry = tape_alloc_entry(tape);
        if (!wo_entry) return Y_gt;
        wo_entry->op = OP_GEMM;
        wo_entry->inputs[0] = attn_out_gt;
        wo_entry->inputs[1] = Wo;
        wo_entry->num_inputs = 2;
        wo_entry->output = Y_gt;
        wo_entry->M = BS; wo_entry->K = hidden_dim; wo_entry->N = hidden_dim;

        wo_entry->saved[0] = tape_save_tensor(tape, attn_out);
        wo_entry->saved[1] = tape_save_tensor(tape, Wo->data);
        wo_entry->num_saved = 2;

        Y_gt->creator = wo_entry;
    }

    return Y_gt;
}

GradTensor* ag_bias_add(Tape* tape, GradTensor* x, GradTensor* bias,
                         i32 rows, i32 cols) {
    Tensor* Y = tape_alloc_tensor(tape, x->data->ndim, x->data->shape);
    if (!Y) return NULL;

    /* Y[i] = X[i] + bias[i % cols] */
    for (i32 r = 0; r < rows; r++) {
        vec_add_f32(x->data->data + r * cols, bias->data->data,
                    Y->data + r * cols, cols);
    }

    GradTensor* Y_gt = tape_alloc_grad_tensor(tape);
    if (!Y_gt) return NULL;
    Y_gt->data = Y;
    Y_gt->requires_grad = (x->requires_grad || bias->requires_grad);

    if (tape->recording && Y_gt->requires_grad) {
        TapeEntry* entry = tape_alloc_entry(tape);
        if (!entry) return Y_gt;
        entry->op = OP_VEC_ADD_BIAS;
        entry->inputs[0] = x;
        entry->inputs[1] = bias;
        entry->num_inputs = 2;
        entry->output = Y_gt;
        entry->M = rows; entry->N = cols;

        Y_gt->creator = entry;
    }

    return Y_gt;
}

/* ============================================================================
 * Debug / Utility
 * ==========================================================================*/

const char* op_type_name(OpType op) {
    switch (op) {
        case OP_GEMM:           return "GEMM";
        case OP_GEMM_BIAS:      return "GEMM_BIAS";
        case OP_LAYERNORM:      return "LAYERNORM";
        case OP_GELU:           return "GELU";
        case OP_RELU:           return "RELU";
        case OP_SOFTMAX:        return "SOFTMAX";
        case OP_ADD:            return "ADD";
        case OP_SCALE:          return "SCALE";
        case OP_EMBEDDING:      return "EMBEDDING";
        case OP_CROSS_ENTROPY:  return "CROSS_ENTROPY";
        case OP_TRANSPOSE:      return "TRANSPOSE";
        case OP_ATTENTION:      return "ATTENTION";
        case OP_VEC_ADD_BIAS:   return "BIAS_ADD";
        default:                return "UNKNOWN";
    }
}

void tape_print(Tape* tape) {
    if (!tape) return;
    printf("=== Autograd Tape (%d entries, %d tensors) ===\n",
           tape->count, tape->tensor_count);

    for (i32 i = 0; i < tape->count; i++) {
        TapeEntry* e = &tape->entries[i];
        printf("  [%3d] %-14s  inputs=%d  saved=%d",
               i, op_type_name(e->op), e->num_inputs, e->num_saved);

        if (e->output && e->output->data) {
            printf("  out=[");
            for (i32 d = 0; d < e->output->data->ndim; d++) {
                printf("%d%s", e->output->data->shape[d],
                       d < e->output->data->ndim - 1 ? "," : "");
            }
            printf("]");
        }
        printf("\n");
    }
    printf("=======================================\n");
}
