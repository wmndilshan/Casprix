/*
 * Regression test: Flash Attention 2 correctness
 *
 * Verifies that cpx_attention_flash2() produces the same output
 * as naive O(N^2) attention within a tolerance of 1e-4.
 *
 * Build:
 *   gcc -O2 -I runtime/ai/llm \
 *       tests/runtime/test_attention_flash.c \
 *       runtime/ai/llm/attention.c runtime/ai/llm/tensor.c runtime/ai/llm/ops.c \
 *       -lm -o test_attention_flash
 */

#include "attention.h"
#include "tensor.h"
#include "ops.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─────────────────────────────────────────────────────────────────────
 * Naive O(N^2) attention reference
 * Input: Q/K/V [batch, heads, seq, head_dim]  (row-major)
 * Output: out  [batch, heads, seq, head_dim]
 * ─────────────────────────────────────────────────────────────────── */
static void naive_attention(const float* Q, const float* K, const float* V,
                             float* out,
                             int batch, int heads, int seq, int head_dim,
                             float scale) {
    for (int b = 0; b < batch; b++) {
        for (int h = 0; h < heads; h++) {
            int stride = seq * head_dim;
            int bh_off = (b * heads + h) * stride;

            /* Score matrix S[seq, seq] = Q * K^T / scale */
            float* S = (float*)malloc(seq * seq * sizeof(float));
            for (int i = 0; i < seq; i++) {
                for (int j = 0; j < seq; j++) {
                    float dot = 0.f;
                    for (int d = 0; d < head_dim; d++)
                        dot += Q[bh_off + i * head_dim + d]
                             * K[bh_off + j * head_dim + d];
                    S[i * seq + j] = dot / scale;
                }
            }

            /* Row-wise softmax */
            for (int i = 0; i < seq; i++) {
                float row_max = S[i * seq];
                for (int j = 1; j < seq; j++)
                    if (S[i * seq + j] > row_max) row_max = S[i * seq + j];
                float row_sum = 0.f;
                for (int j = 0; j < seq; j++) {
                    S[i * seq + j] = expf(S[i * seq + j] - row_max);
                    row_sum += S[i * seq + j];
                }
                for (int j = 0; j < seq; j++)
                    S[i * seq + j] /= row_sum;
            }

            /* out = S * V */
            for (int i = 0; i < seq; i++) {
                for (int d = 0; d < head_dim; d++) {
                    float acc = 0.f;
                    for (int j = 0; j < seq; j++)
                        acc += S[i * seq + j] * V[bh_off + j * head_dim + d];
                    out[bh_off + i * head_dim + d] = acc;
                }
            }
            free(S);
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * Helpers
 * ─────────────────────────────────────────────────────────────────── */
static float randf(void) { return (float)rand() / (float)RAND_MAX * 2.f - 1.f; }

static float max_abs_diff(const float* a, const float* b, int n) {
    float m = 0.f;
    for (int i = 0; i < n; i++) {
        float d = fabsf(a[i] - b[i]);
        if (d > m) m = d;
    }
    return m;
}

/* ─────────────────────────────────────────────────────────────────────
 * Test cases
 * ─────────────────────────────────────────────────────────────────── */

static void run_test(int batch, int heads, int seq, int head_dim, float tol,
                     const char* label) {
    srand(42);
    int total = batch * heads * seq * head_dim;
    float* Q   = (float*)malloc(total * sizeof(float));
    float* K   = (float*)malloc(total * sizeof(float));
    float* V   = (float*)malloc(total * sizeof(float));
    float* ref = (float*)malloc(total * sizeof(float));
    float* got = (float*)malloc(total * sizeof(float));

    for (int i = 0; i < total; i++) { Q[i] = randf(); K[i] = randf(); V[i] = randf(); }
    float scale = sqrtf((float)head_dim);

    /* Reference: naive O(N^2) */
    naive_attention(Q, K, V, ref, batch, heads, seq, head_dim, scale);

    /* FA2: arena must be large enough */
    size_t arena_cap = (size_t)1024 * 1024 * 16;  /* 16 MB scratch */
    Arena* arena = arena_create(arena_cap);

    cpx_attention_flash2(Q, K, V, got, batch, heads, seq, head_dim, scale, arena);

    float diff = max_abs_diff(ref, got, total);
    printf("  %-30s  batch=%d heads=%d seq=%3d dim=%2d  max_diff=%.2e  %s\n",
           label, batch, heads, seq, head_dim, (double)diff,
           diff < tol ? "PASS" : "FAIL");
    assert(diff < tol);

    arena_destroy(arena);
    free(Q); free(K); free(V); free(ref); free(got);
}

int main(void) {
    printf("=== test_attention_flash ===\n");

    /* 1. Tiny: single batch, single head, short seq */
    run_test(1, 1,  8, 16, 1e-4f, "single-batch/head");

    /* 2. Multi-head: 2 batches, 4 heads */
    run_test(2, 4, 16, 32, 1e-4f, "multi-head");

    /* 3. Sequence longer than one tile (FLASH_TILE=64): seq=128 */
    run_test(1, 2, 128, 64, 5e-4f, "multi-tile seq=128");

    /* 4. Seq exactly equal to tile boundary */
    run_test(1, 1, 64, 32, 1e-4f, "seq==FLASH_TILE");

    /* 5. Non-power-of-two seq */
    run_test(1, 2, 70, 16, 5e-4f, "non-pow2 seq=70");

    printf("All Flash Attention 2 tests passed.\n");
    return 0;
}
