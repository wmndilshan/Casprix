/*
 * LLM Runtime - Flash Attention 2 Implementation
 *
 * Algorithm reference: Dao et al., "FlashAttention-2: Faster Attention with
 * Better Parallelism and Work Partitioning" (2023).
 *
 * This file implements the tiled forward pass only.  No backward pass.
 * All scratch memory is drawn from the caller-supplied Arena; the caller is
 * responsible for resetting the arena between training steps.
 *
 * Layout (row-major):
 *   Q / K / V / out : [batch, heads, seq_len, head_dim]
 *   Element at (b, h, s, d):
 *     base + ((b * heads + h) * seq_len + s) * head_dim + d
 */

#include "attention.h"
#include "ops.h"

#include <assert.h>
#include <math.h>    /* expf, fmaxf */
#include <string.h>  /* memset */
#include <stddef.h>

/* Tile size: 64 rows/cols per tile.  At head_dim ≤ 128 this keeps the three
 * working sets (Q-tile, K-tile, V-tile) inside a typical 32 KiB L1 cache. */
#define FLASH_TILE 64

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Pointer to row s of head (b, h) inside a [batch, heads, seq_len, head_dim]
 * flat buffer. */
static inline const float* qkv_row(const float* buf,
                                   int heads, int seq_len, int head_dim,
                                   int b, int h, int s)
{
    return buf + ((b * heads + h) * seq_len + s) * head_dim;
}

static inline float* out_row(float* buf,
                              int heads, int seq_len, int head_dim,
                              int b, int h, int s)
{
    return buf + ((b * heads + h) * seq_len + s) * head_dim;
}

/* -------------------------------------------------------------------------
 * cpx_attention_flash2
 * ---------------------------------------------------------------------- */

void cpx_attention_flash2(
    const float* Q,
    const float* K,
    const float* V,
    float*       out,
    int batch, int heads, int seq_len, int head_dim,
    float scale,
    Arena* arena)
{
    /* ---- preconditions ---- */
    assert(Q       != NULL);
    assert(out     != NULL);
    assert(head_dim % 8 == 0);

    /* Effective tile heights (may be smaller than FLASH_TILE at boundaries). */
    /* We work with plain ints inside the loops; FLASH_TILE is just the cap.  */

    for (int b = 0; b < batch; b++) {
        for (int h = 0; h < heads; h++) {

            /* Outer loop: tile over query positions. */
            for (int q_start = 0; q_start < seq_len; q_start += FLASH_TILE) {
                int q_end  = q_start + FLASH_TILE;
                if (q_end > seq_len) q_end = seq_len;
                int Tq = q_end - q_start;   /* actual rows in this Q-tile */

                /* ---- Allocate per-Q-tile accumulators from arena ---- */

                /* o_tile[Tq, head_dim] – accumulated output, starts at zero */
                size_t o_bytes = (size_t)Tq * (size_t)head_dim * sizeof(float);
                float* o_tile  = (float*)arena_alloc(arena, o_bytes, 64);
                memset(o_tile, 0, o_bytes);

                /* l_tile[Tq] – running softmax denominator, starts at zero */
                float* l_tile = (float*)arena_alloc(arena, (size_t)Tq * sizeof(float), 64);
                for (int i = 0; i < Tq; i++) l_tile[i] = 0.0f;

                /* m_tile[Tq] – running row-wise maximum, starts at -inf */
                float* m_tile = (float*)arena_alloc(arena, (size_t)Tq * sizeof(float), 64);
                for (int i = 0; i < Tq; i++) m_tile[i] = -1e38f; /* −∞ approx */

                /* Inner loop: tile over key/value positions. */
                for (int kv_start = 0; kv_start < seq_len; kv_start += FLASH_TILE) {
                    int kv_end = kv_start + FLASH_TILE;
                    if (kv_end > seq_len) kv_end = seq_len;
                    int Tkv = kv_end - kv_start;

                    /* ---- S[Tq, Tkv] = Q_tile × K_tile^T / scale ---- */
                    size_t s_bytes = (size_t)Tq * (size_t)Tkv * sizeof(float);
                    float* S = (float*)arena_alloc(arena, s_bytes, 64);

                    /* Compute S = Q_tile * K_tile^T, then divide by scale.
                     * We do this with a plain scalar triple loop to stay
                     * correct and avoid a K-transpose allocation.
                     *
                     * AVX2 path: TODO – replace inner loop body with
                     * _mm256_fmadd_ps over head_dim / 8 chunks.
                     */
#ifdef __AVX2__
                    /* AVX2 path (TODO): placeholder falls through to scalar. */
                    (void)0;
#endif
                    {
                        /* Scalar path – correct reference implementation. */
                        for (int qi = 0; qi < Tq; qi++) {
                            const float* q_row =
                                qkv_row(Q, heads, seq_len, head_dim,
                                        b, h, q_start + qi);

                            for (int ki = 0; ki < Tkv; ki++) {
                                const float* k_row =
                                    qkv_row(K, heads, seq_len, head_dim,
                                            b, h, kv_start + ki);

                                /* dot(q_row, k_row) */
                                float dot = 0.0f;
                                for (int d = 0; d < head_dim; d++) {
                                    dot += q_row[d] * k_row[d];
                                }
                                S[qi * Tkv + ki] = dot / scale;
                            }
                        }
                    }

                    /* ---- Online softmax + accumulate into o_tile ---- */
                    for (int qi = 0; qi < Tq; qi++) {
                        const float* s_row = S + qi * Tkv;

                        /* Row maximum of this S block */
                        float s_max = s_row[0];
                        for (int ki = 1; ki < Tkv; ki++) {
                            if (s_row[ki] > s_max) s_max = s_row[ki];
                        }

                        /* New global maximum for this query row */
                        float m_new = fmaxf(m_tile[qi], s_max);

                        /* p[ki] = exp(S[qi,ki] − m_new) */
                        /* Reuse s_row storage: we need to write p somewhere.
                         * S was allocated from the arena; we can write into it
                         * because we have already read the raw scores above.
                         * Cast away const for the p buffer – it's our scratch. */
                        float* p = S + qi * Tkv;   /* reuse row in-place */
                        float p_sum = 0.0f;
                        for (int ki = 0; ki < Tkv; ki++) {
                            p[ki] = expf(p[ki] - m_new);
                            p_sum += p[ki];
                        }

                        /* Correction factor for the running sum (rescale old
                         * accumulator to the new maximum). */
                        float corr = expf(m_tile[qi] - m_new);

                        /* l_new = corr * l_old + sum(p) */
                        float l_new = corr * l_tile[qi] + p_sum;

                        /* Rescale o_tile[qi] by corr */
                        float* o_row = o_tile + qi * head_dim;
                        for (int d = 0; d < head_dim; d++) {
                            o_row[d] *= corr;
                        }

                        /* o_tile[qi] += p × V_tile  (weighted sum over kv) */
                        for (int ki = 0; ki < Tkv; ki++) {
                            const float* v_row =
                                qkv_row(V, heads, seq_len, head_dim,
                                        b, h, kv_start + ki);
                            float p_ki = p[ki];
                            for (int d = 0; d < head_dim; d++) {
                                o_row[d] += p_ki * v_row[d];
                            }
                        }

                        /* Update running statistics */
                        m_tile[qi] = m_new;
                        l_tile[qi] = l_new;
                    }
                    /* S / p buffer from arena – caller resets between steps. */
                } /* end kv tile loop */

                /* ---- Normalize and write o_tile → out ---- */
                for (int qi = 0; qi < Tq; qi++) {
                    float l_inv = 1.0f / l_tile[qi];
                    float*       o_row  = o_tile + qi * head_dim;
                    float*       dst    = out_row(out, heads, seq_len, head_dim,
                                                  b, h, q_start + qi);
                    for (int d = 0; d < head_dim; d++) {
                        dst[d] = o_row[d] * l_inv;
                    }
                }
                /* o_tile, l_tile, m_tile remain in arena until caller resets. */
            } /* end q tile loop */
        } /* end head loop */
    } /* end batch loop */
}

/* -------------------------------------------------------------------------
 * cpx_attention_gqa — Grouped Query Attention
 * K and V use num_kv_heads; Q uses num_q_heads.
 * Each Q head h_q reads from KV head h_kv = h_q / group_size.
 * ---------------------------------------------------------------------- */

void cpx_attention_gqa(
    const float* Q,
    const float* K,
    const float* V,
    float*       out,
    int batch, int num_q_heads, int num_kv_heads,
    int seq_len, int head_dim, float scale,
    Arena* arena)
{
    assert(Q   != NULL);
    assert(K   != NULL);
    assert(V   != NULL);
    assert(out != NULL);
    assert(num_q_heads % num_kv_heads == 0);

    const int group_size = num_q_heads / num_kv_heads;

    /* Per-head scratch: K^T [head_dim, seq_len] and scores [seq_len, seq_len]. */
    const size_t kt_bytes     = (size_t)head_dim * seq_len * sizeof(float);
    const size_t scores_bytes = (size_t)seq_len  * seq_len * sizeof(float);

    for (int b = 0; b < batch; b++) {
        for (int h_q = 0; h_q < num_q_heads; h_q++) {
            const int h_kv = h_q / group_size;

            const float* Q_bh = Q   + ((size_t)(b * num_q_heads  + h_q)  * seq_len * head_dim);
            const float* K_bh = K   + ((size_t)(b * num_kv_heads + h_kv) * seq_len * head_dim);
            const float* V_bh = V   + ((size_t)(b * num_kv_heads + h_kv) * seq_len * head_dim);
            float*       o_bh = out + ((size_t)(b * num_q_heads  + h_q)  * seq_len * head_dim);

            /* Scratch from arena — no malloc. */
            float* K_T    = (float*)arena_alloc(arena, kt_bytes,     sizeof(float));
            float* scores = (float*)arena_alloc(arena, scores_bytes, sizeof(float));

            /* Transpose K_bh [seq_len, head_dim] → K_T [head_dim, seq_len]. */
            for (int s = 0; s < seq_len; s++)
                for (int d = 0; d < head_dim; d++)
                    K_T[d * seq_len + s] = K_bh[s * head_dim + d];

            /* scores = Q_bh × K_T → [seq_len, seq_len] */
            gemm_f32(Q_bh, K_T, scores, seq_len, head_dim, seq_len);
            vec_scale_f32(scores, scale, scores, seq_len * seq_len);

            /* Row-wise softmax. */
            softmax_f32(scores, scores, seq_len, seq_len);

            /* o_bh = scores × V_bh → [seq_len, head_dim] */
            gemm_f32(scores, V_bh, o_bh, seq_len, seq_len, head_dim);
        }
    }
}
