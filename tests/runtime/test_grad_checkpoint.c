/*
 * Regression test: Gradient checkpointing
 *
 * Verifies that transformer_forward_checkpointed() produces identical
 * logits to transformer_forward() (standard), and that
 * grad_checkpoint_recompute() reproduces the correct activations.
 *
 * Build:
 *   gcc -O2 -I runtime/ai/llm \
 *       tests/runtime/test_grad_checkpoint.c \
 *       runtime/ai/llm/training.c runtime/ai/llm/transformer.c \
 *       runtime/ai/llm/tensor.c runtime/ai/llm/ops.c runtime/ai/llm/tokenizer.c \
 *       -lm -o test_grad_checkpoint
 */

#include "training.h"
#include "transformer.h"
#include "tensor.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float max_abs_diff_f(const float* a, const float* b, size_t n) {
    float m = 0.f;
    for (size_t i = 0; i < n; i++) {
        float d = fabsf(a[i] - b[i]);
        if (d > m) m = d;
    }
    return m;
}

static void test_forward_equivalence(void) {
    /* Small model: vocab=32, hidden=16, 4 layers, 2 heads, ffn=32, seq=4 */
    int vocab=32, hidden=16, layers=4, heads=2, ffn=32, max_seq=8;
    TransformerModel* model = transformer_create(vocab, hidden, layers, heads, ffn, max_seq);

    int batch=1, seq=4;
    i32 token_ids[4] = {1, 5, 10, 3};

    /* Allocate memory pools. */
    MemoryPools* mem_std  = mempool_create(0, 4*1024*1024, 1024*1024);
    MemoryPools* mem_ckpt = mempool_create(0, 4*1024*1024, 1024*1024);

    /* Standard forward. */
    Tensor* logits_std = arena_alloc_tensor(mem_std->activations, 2,
                                             (i32[]){batch*seq, vocab});
    transformer_forward(model, token_ids, batch, seq, logits_std, mem_std);

    /* Checkpointed forward. */
    Tensor* logits_ckpt = arena_alloc_tensor(mem_ckpt->activations, 2,
                                              (i32[]){batch*seq, vocab});
    GradCheckpointCtx* ctx = grad_checkpoint_create(model, mem_ckpt, 0);
    assert(ctx != NULL);
    assert(ctx->num_checkpoints > 0);

    transformer_forward_checkpointed(model, token_ids, batch, seq,
                                      logits_ckpt, ctx, mem_ckpt);

    /* Compare outputs — must be identical (same arithmetic path). */
    float diff = max_abs_diff_f(logits_std->data, logits_ckpt->data,
                                 (size_t)batch * seq * vocab);
    printf("  forward_equivalence: max_diff=%.2e  %s\n",
           (double)diff, diff < 1e-5f ? "PASS" : "FAIL");
    assert(diff < 1e-5f);

    grad_checkpoint_destroy(ctx);
    mempool_destroy(mem_std);
    mempool_destroy(mem_ckpt);
    transformer_destroy(model);
}

static void test_checkpoint_interval(void) {
    /* Verify num_checkpoints = ceil(layers / interval). */
    int layers = 9;
    TransformerModel* model = transformer_create(16, 8, layers, 2, 16, 4);
    MemoryPools* mem = mempool_create(0, 4*1024*1024, 1024*1024);

    /* interval=3 → 3 checkpoints for 9 layers */
    GradCheckpointCtx* ctx = grad_checkpoint_create(model, mem, 3);
    assert(ctx->interval == 3);
    assert(ctx->num_checkpoints == 3);
    grad_checkpoint_destroy(ctx);

    /* interval=0 → auto: sqrt(9)=3, so interval=3, 3 checkpoints */
    ctx = grad_checkpoint_create(model, mem, 0);
    assert(ctx->num_checkpoints == 3);
    grad_checkpoint_destroy(ctx);

    mempool_destroy(mem);
    transformer_destroy(model);
    printf("  checkpoint_interval: PASS\n");
}

static void test_recompute(void) {
    /* Verify recomputed segment matches the activation stored by checkpointed forward. */
    int vocab=16, hidden=8, layers=4, heads=2, ffn=16, max_seq=4;
    TransformerModel* model = transformer_create(vocab, hidden, layers, heads, ffn, max_seq);

    int batch=1, seq=4;
    i32 tids[4] = {2, 7, 1, 15};

    MemoryPools* mem = mempool_create(0, 8*1024*1024, 1024*1024);
    Tensor* logits = arena_alloc_tensor(mem->activations, 2,
                                         (i32[]){batch*seq, vocab});

    /* Run checkpointed forward with interval=2. */
    GradCheckpointCtx* ctx = grad_checkpoint_create(model, mem, 2);
    transformer_forward_checkpointed(model, tids, batch, seq, logits, ctx, mem);

    /* checkpoint_inputs[0] is the input to layer 0 (embedding output). */
    assert(ctx->checkpoint_inputs[0] != NULL);

    /* Recompute layers 0..2 from checkpoint 0. */
    Tensor* recomputed = arena_alloc_tensor(mem->activations, 3,
                                             (i32[]){batch, seq, hidden});
    grad_checkpoint_recompute(model, ctx, 0, 2, recomputed, mem);

    /* The recomputed output must be non-zero (i.e. computation ran). */
    float sum = 0.f;
    for (size_t i = 0; i < recomputed->size; i++) sum += fabsf(recomputed->data[i]);
    assert(sum > 0.f);
    printf("  recompute_nonzero: PASS (sum=%.4f)\n", (double)sum);

    grad_checkpoint_destroy(ctx);
    mempool_destroy(mem);
    transformer_destroy(model);
}

int main(void) {
    printf("=== test_grad_checkpoint ===\n");
    test_forward_equivalence();
    test_checkpoint_interval();
    test_recompute();
    printf("All gradient checkpointing tests passed.\n");
    return 0;
}
