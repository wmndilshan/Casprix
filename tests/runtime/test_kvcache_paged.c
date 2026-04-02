/*
 * Regression test: Paged KV cache write/read correctness
 *
 * Verifies that the paged layout produces identical output to the
 * contiguous layout for write → read_range round-trips.
 *
 * Build:
 *   gcc -O2 -I runtime/ai/ml \
 *       tests/runtime/test_kvcache_paged.c \
 *       runtime/ai/ml/cpx_kvcache.c runtime/ai/ml/cpx_mem_arena.c \
 *       -o test_kvcache_paged
 */

#include "cpx_kvcache.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float randf(void) { return (float)rand() / (float)RAND_MAX * 2.f - 1.f; }

/* Fill a [head_dim] K or V vector with deterministic values. */
static void make_kv(float* buf, int head_dim, int token, int layer, int head, int is_v) {
    for (int d = 0; d < head_dim; d++) {
        /* Use a simple hash-like formula for reproducibility. */
        buf[d] = (float)((token * 3271 + layer * 1009 + head * 503 + d * 17 + is_v * 1000007)
                         % 65537) / 32768.f - 1.f;
    }
}

static void run_layout_compare(int num_layers, int num_kv_heads,
                                int head_dim, int max_seq_len,
                                int page_size, int tokens_to_write) {
    CpxKvCacheConfig cont_cfg = {
        .num_layers   = num_layers,
        .num_kv_heads = num_kv_heads,
        .head_dim     = head_dim,
        .max_seq_len  = max_seq_len,
        .max_batch    = 1,
        .layout       = KV_LAYOUT_CONTIGUOUS,
        .quant        = KV_QUANT_NONE,
        .page_size    = page_size,
        .backing_arena = NULL,
    };
    CpxKvCacheConfig paged_cfg = cont_cfg;
    paged_cfg.layout = KV_LAYOUT_PAGED;

    CpxKvCache* cont  = cpx_kvcache_create(&cont_cfg);
    CpxKvCache* paged = cpx_kvcache_create(&paged_cfg);
    assert(cont && paged);

    int cseq = cpx_kvcache_alloc_seq(cont,  0);
    int pseq = cpx_kvcache_alloc_seq(paged, 0);
    assert(cseq == 0 && pseq == 0);

    float* k_vec = (float*)malloc(head_dim * sizeof(float));
    float* v_vec = (float*)malloc(head_dim * sizeof(float));

    /* Write tokens to both caches. */
    for (int t = 0; t < tokens_to_write; t++) {
        for (int l = 0; l < num_layers; l++) {
            for (int h = 0; h < num_kv_heads; h++) {
                make_kv(k_vec, head_dim, t, l, h, 0);
                make_kv(v_vec, head_dim, t, l, h, 1);
                cpx_kvcache_write(cont,  cseq, l, h, k_vec, v_vec);
                cpx_kvcache_write(paged, pseq, l, h, k_vec, v_vec);
            }
        }
    }

    /* Read back and compare. */
    float* k_cont  = (float*)malloc(tokens_to_write * head_dim * sizeof(float));
    float* v_cont  = (float*)malloc(tokens_to_write * head_dim * sizeof(float));
    float* k_paged = (float*)malloc(tokens_to_write * head_dim * sizeof(float));
    float* v_paged = (float*)malloc(tokens_to_write * head_dim * sizeof(float));

    for (int l = 0; l < num_layers; l++) {
        for (int h = 0; h < num_kv_heads; h++) {
            cpx_kvcache_read_range(cont,  cseq, l, h, 0, tokens_to_write, k_cont,  v_cont);
            cpx_kvcache_read_range(paged, pseq, l, h, 0, tokens_to_write, k_paged, v_paged);

            for (int i = 0; i < tokens_to_write * head_dim; i++) {
                float dk = fabsf(k_cont[i] - k_paged[i]);
                float dv = fabsf(v_cont[i] - v_paged[i]);
                if (dk > 1e-6f || dv > 1e-6f) {
                    fprintf(stderr, "MISMATCH layer=%d head=%d elem=%d "
                            "k_diff=%.2e v_diff=%.2e\n", l, h, i, (double)dk, (double)dv);
                    assert(0);
                }
            }
        }
    }

    free(k_cont); free(v_cont); free(k_paged); free(v_paged);
    free(k_vec); free(v_vec);
    cpx_kvcache_destroy(cont);
    cpx_kvcache_destroy(paged);
}

static void test_paged_rollback(void) {
    /* Write 32 tokens, rollback to 16, verify seq_len. */
    CpxKvCacheConfig cfg = {
        .num_layers=2, .num_kv_heads=2, .head_dim=16,
        .max_seq_len=64, .max_batch=1,
        .layout=KV_LAYOUT_PAGED, .quant=KV_QUANT_NONE,
        .page_size=8, .backing_arena=NULL,
    };
    CpxKvCache* c = cpx_kvcache_create(&cfg);
    int sid = cpx_kvcache_alloc_seq(c, 0);

    float kv[16] = {0};
    for (int t = 0; t < 32; t++)
        for (int l = 0; l < 2; l++)
            for (int h = 0; h < 2; h++)
                cpx_kvcache_write(c, sid, l, h, kv, kv);

    assert(c->seqs[sid].seq_len == 32);
    cpx_kvcache_rollback(c, sid, 16);
    assert(c->seqs[sid].seq_len == 16);

    cpx_kvcache_destroy(c);
    printf("  paged_rollback: PASS\n");
}

int main(void) {
    printf("=== test_kvcache_paged ===\n");

    /* Single layer, head, short sequence */
    run_layout_compare(1, 1, 16, 64, 8, 20);
    printf("  layout_compare L=1 H=1 D=16 S=20 ps=8: PASS\n");

    /* Multiple layers and heads, sequence spanning several pages */
    run_layout_compare(2, 2, 32, 128, 16, 60);
    printf("  layout_compare L=2 H=2 D=32 S=60 ps=16: PASS\n");

    /* Sequence exactly fits one page */
    run_layout_compare(1, 1, 8, 32, 8, 8);
    printf("  layout_compare exact-page-fit: PASS\n");

    /* Rollback test */
    test_paged_rollback();

    printf("All paged KV cache tests passed.\n");
    return 0;
}
