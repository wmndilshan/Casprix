/*
 * LLM Runtime — Inference Engine
 *
 * Standard (desktop/server) and MCU inference contexts.
 * MCU path guarded by CPX_PROFILE_MCU:
 *   - All memory is static (no heap).
 *   - Weights are read-only XIP flash pointers.
 *   - SRAM budget enforced at compile time.
 */

#ifndef LLM_INFERENCE_H
#define LLM_INFERENCE_H

#include "tensor.h"
#include <stdint.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════════
 * Standard inference sampling
 * ═══════════════════════════════════════════════════════════════════ */

/* Greedy argmax: returns index of highest logit. O(vocab_size). */
i32 cpx_argmax(const float* logits, i32 vocab_size);

/* Top-k sampling over logits[vocab_size].
 * Scales by temperature (1.0 = unscaled), zeros all but top-k,
 * runs softmax, returns sampled token index.
 * All scratch allocated from arena — no malloc. */
i32 cpx_sample_topk(const float* logits, i32 vocab_size,
                     i32 k, float temperature, TensorArena* scratch);

/* ═══════════════════════════════════════════════════════════════════
 * MCU inference context
 *
 * Usage on Cortex-M4/M7 or RISC-V (RP2040, ESP32-C3):
 *   CpxMCUInferenceCtx ctx;          // lives in .bss — zero heap
 *   cpx_mcu_inference_init(&ctx, weights_flash, weights_sz,
 *                           vocab, dim, layers, heads, max_seq);
 *   const float* logits = cpx_mcu_decode_step(&ctx, token);
 *   i32 next = cpx_argmax(logits, ctx.vocab_size);
 *   cpx_mcu_tensor_arena_reset(&ctx);       // reclaim activations
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef CPX_PROFILE_MCU

/* 256 KB static SRAM pool — never heap-allocated. */
#define CPX_MCU_SRAM_BUDGET  (256u * 1024u)

typedef struct {
    /* Flash weights: INT4-quantised, XIP read-only. */
    const uint8_t* weights_flash;
    size_t         weights_size;

    /* Static SRAM: activations + KV state for one decode step. */
    uint8_t        sram_pool[CPX_MCU_SRAM_BUDGET];

    /* TensorArena built over sram_pool. Reset between tokens. */
    TensorArena          arena;

    /* Model config (filled by cpx_mcu_inference_init). */
    i32  vocab_size;
    i32  hidden_dim;
    i32  num_layers;
    i32  num_heads;
    i32  head_dim;      /* hidden_dim / num_heads */
    i32  max_seq_len;
} CpxMCUInferenceCtx;

/* Initialise the context over the static sram_pool (no malloc).
 * Returns 0 on success, -1 if weights_size > CPX_MCU_SRAM_BUDGET.  */
int cpx_mcu_inference_init(CpxMCUInferenceCtx* ctx,
                             const uint8_t* weights_flash,
                             size_t         weights_size,
                             i32 vocab_size, i32 hidden_dim,
                             i32 num_layers, i32 num_heads,
                             i32 max_seq_len);

/* Run one decode step: input token_id → logits[vocab_size].
 * Returns pointer into the arena (valid until next cpx_mcu_tensor_arena_reset).
 * Returns NULL on arena overflow. */
const float* cpx_mcu_decode_step(CpxMCUInferenceCtx* ctx, i32 token_id);

/* Reset activation arena for next decode step.
 * Does NOT reset weights or model config. */
void cpx_mcu_tensor_arena_reset(CpxMCUInferenceCtx* ctx);

#endif /* CPX_PROFILE_MCU */

#endif /* LLM_INFERENCE_H */
