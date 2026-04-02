/*
 * LLM Runtime — Inference Engine Implementation
 */

#include "inference.h"
#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>   /* rand() for top-k sampling */

/* ═══════════════════════════════════════════════════════════════════
 * Standard sampling
 * ═══════════════════════════════════════════════════════════════════ */

i32 cpx_argmax(const float* logits, i32 vocab_size) {
    assert(logits != NULL);
    assert(vocab_size > 0);
    i32   best_idx = 0;
    float best_val = logits[0];
    for (i32 i = 1; i < vocab_size; i++) {
        if (logits[i] > best_val) { best_val = logits[i]; best_idx = i; }
    }
    return best_idx;
}

i32 cpx_sample_topk(const float* logits, i32 vocab_size,
                     i32 k, float temperature, Arena* scratch) {
    assert(logits  != NULL);
    assert(scratch != NULL);
    assert(vocab_size > 0);
    if (k <= 0 || k > vocab_size) k = vocab_size;

    /* Allocate score copy from arena. */
    float* scores = (float*)arena_alloc(scratch, (size_t)vocab_size * sizeof(float),
                                        sizeof(float));
    assert(scores != NULL);
    memcpy(scores, logits, (size_t)vocab_size * sizeof(float));

    /* Apply temperature. */
    if (temperature != 1.0f && temperature > 0.f) {
        float inv_t = 1.f / temperature;
        for (i32 i = 0; i < vocab_size; i++) scores[i] *= inv_t;
    }

    /* Partial selection sort to find the k-th largest threshold.
     * O(vocab_size * k) — acceptable for small k at inference.
     * For large k, replace with nth_element equivalent. */
    int*  indices = (int*)arena_alloc(scratch, (size_t)k * sizeof(int), sizeof(int));
    assert(indices != NULL);

    /* Mask tracking which indices have been picked. */
    uint8_t* used = (uint8_t*)arena_alloc(scratch, (size_t)vocab_size, 1);
    assert(used != NULL);
    memset(used, 0, (size_t)vocab_size);

    for (i32 pick = 0; pick < k; pick++) {
        float best = -1e38f;
        i32   bidx = 0;
        for (i32 i = 0; i < vocab_size; i++) {
            if (!used[i] && scores[i] > best) { best = scores[i]; bidx = i; }
        }
        indices[pick] = bidx;
        used[bidx] = 1;
    }

    /* Zero out scores outside top-k. */
    memset(used, 0, (size_t)vocab_size);
    for (i32 p = 0; p < k; p++) used[indices[p]] = 1;
    for (i32 i = 0; i < vocab_size; i++) if (!used[i]) scores[i] = -1e38f;

    /* Softmax over remaining top-k. */
    float max_s = scores[indices[0]];
    for (i32 p = 1; p < k; p++) if (scores[indices[p]] > max_s) max_s = scores[indices[p]];
    float sum = 0.f;
    for (i32 p = 0; p < k; p++) {
        scores[indices[p]] = expf(scores[indices[p]] - max_s);
        sum += scores[indices[p]];
    }
    for (i32 p = 0; p < k; p++) scores[indices[p]] /= sum;

    /* Sample: uniform threshold scan over sorted probabilities.
     * TODO: wire to a seeded PRNG for reproducibility. */
    float r = (float)rand() / (float)(RAND_MAX + 1.0);
    float cumulative = 0.f;
    for (i32 p = 0; p < k; p++) {
        cumulative += scores[indices[p]];
        if (r < cumulative) return indices[p];
    }
    return indices[k - 1];  /* fallback: highest probability token */
}

/* ═══════════════════════════════════════════════════════════════════
 * MCU inference context
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef CPX_PROFILE_MCU

int cpx_mcu_inference_init(CpxMCUInferenceCtx* ctx,
                             const uint8_t* weights_flash,
                             size_t         weights_size,
                             i32 vocab_size, i32 hidden_dim,
                             i32 num_layers, i32 num_heads,
                             i32 max_seq_len) {
    assert(ctx != NULL);

    /* Weights live in flash — validate size only. */
    if (weights_size > CPX_MCU_SRAM_BUDGET) return -1;

    ctx->weights_flash = weights_flash;
    ctx->weights_size  = weights_size;
    ctx->vocab_size    = vocab_size;
    ctx->hidden_dim    = hidden_dim;
    ctx->num_layers    = num_layers;
    ctx->num_heads     = num_heads;
    ctx->head_dim      = (num_heads > 0) ? hidden_dim / num_heads : hidden_dim;
    ctx->max_seq_len   = max_seq_len;

    /* Build arena over the static SRAM pool — no heap. */
    ctx->arena.buffer     = ctx->sram_pool;
    ctx->arena.capacity   = CPX_MCU_SRAM_BUDGET;
    ctx->arena.offset     = 0;
    ctx->arena.peak_usage = 0;

    return 0;
}

const float* cpx_mcu_decode_step(CpxMCUInferenceCtx* ctx, i32 token_id) {
    assert(ctx != NULL);

    /* Allocate logits buffer from SRAM arena — no heap. */
    float* logits = (float*)arena_alloc(&ctx->arena,
                                         (size_t)ctx->vocab_size * sizeof(float),
                                         sizeof(float));
    if (!logits) return NULL;  /* SRAM exhausted */

    /* ── Placeholder inference kernel ────────────────────────────────
     * In production this block would:
     *   1. Dequantize INT4 weight rows from weights_flash on the fly.
     *   2. Run a scaled dot-product attention step using sram_pool.
     *   3. Apply FFN projection to produce logits.
     *
     * For now, compute a deterministic arithmetic placeholder that
     * exercises the arena path without requiring the full transformer
     * to be wired up (which requires transformer.c refactoring for
     * static-allocation paths — tracked as a separate integration task).
     * ──────────────────────────────────────────────────────────────── */
    for (i32 v = 0; v < ctx->vocab_size; v++) {
        /* Deterministic, bounded, no float overflow. */
        int raw = (token_id * 6271 + v * 3571 + ctx->num_layers * 113) % 32749;
        logits[v] = (float)(raw - 16374) / 16374.0f;
    }

    /* If flash weights exist, bias logits by first weight nibble of each row.
     * This demonstrates the dequant-from-flash access pattern. */
    if (ctx->weights_flash && ctx->weights_size > 0) {
        size_t max_rows = ctx->weights_size * 2;  /* 2 nibbles per byte */
        i32 rows = (ctx->vocab_size < (i32)max_rows) ? ctx->vocab_size : (i32)max_rows;
        for (i32 v = 0; v < rows; v++) {
            uint8_t byte = ctx->weights_flash[v / 2];
            int     nibble = (v % 2 == 0) ? (byte & 0x0F) : (byte >> 4);
            int     q = nibble - 8;  /* signed range -8..7 */
            logits[v] += (float)q * 0.01f;
        }
    }

    return logits;
}

void cpx_mcu_arena_reset(CpxMCUInferenceCtx* ctx) {
    assert(ctx != NULL);
    /* Reset arena offset only — weights_flash and config are unchanged. */
    ctx->arena.offset = 0;
    /* Preserve peak_usage for profiling. */
}

#endif /* CPX_PROFILE_MCU */
