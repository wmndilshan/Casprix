/*
 * LLM Runtime - Flash Attention 2
 *
 * Tiled attention that avoids materializing the full NxN score matrix.
 * All temporaries come from the caller-supplied arena; no malloc/free.
 */

#ifndef LLM_ATTENTION_H
#define LLM_ATTENTION_H

#include "tensor.h"

/* Flash Attention 2: tiled attention, avoids materializing NxN score matrix.
 * Q/K/V: [batch, heads, seq_len, head_dim] — row-major
 * out:   [batch, heads, seq_len, head_dim]
 * Tile size: FLASH_TILE = 64 (fits L1 cache on x86/ARM)
 * All temporaries allocated from arena — no malloc. */
void cpx_attention_flash2(
    const float* Q,
    const float* K,
    const float* V,
    float*       out,
    int batch, int heads, int seq_len, int head_dim,
    float scale,
    Arena* arena
);

/* GQA: Grouped Query Attention.
 * Q:   [batch, num_q_heads,  seq_len, head_dim]
 * K,V: [batch, num_kv_heads, seq_len, head_dim]
 * out: [batch, num_q_heads,  seq_len, head_dim]
 * Constraint: num_q_heads % num_kv_heads == 0
 * All temporaries from arena — no malloc. */
void cpx_attention_gqa(
    const float* Q,
    const float* K,
    const float* V,
    float*       out,
    int batch, int num_q_heads, int num_kv_heads,
    int seq_len, int head_dim, float scale,
    Arena* arena
);

#endif /* LLM_ATTENTION_H */
