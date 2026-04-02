/*
 * Regression test: GQA (Grouped Query Attention) correctness
 *
 * Verifies that cpx_attention_gqa() with num_kv_heads=H/G gives the same
 * result as expanding K/V to full num_q_heads and running standard attention.
 *
 * Tolerance: 1e-4 float32.
 *
 * Build:
 *   gcc -O2 -I runtime/ai/llm \
 *       tests/runtime/test_attention_gqa.c \
 *       runtime/ai/llm/attention.c runtime/ai/llm/tensor.c runtime/ai/llm/ops.c \
 *       -lm -o test_attention_gqa
 */

#include "attention.h"
#include "tensor.h"
#include "ops.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float randf_seed(unsigned* s) {
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 1) / (float)(1u << 31) * 2.f - 1.f;
}

static float max_abs_diff(const float* a, const float* b, int n) {
    float m = 0.f;
    for (int i = 0; i < n; i++) {
        float d = fabsf(a[i] - b[i]);
        if (d > m) m = d;
    }
    return m;
}

/*
 * Reference: expand KV from [batch, num_kv_heads, seq, dim] to
 * [batch, num_q_heads, seq, dim] by replicating each KV head group_size times,
 * then run standard attention.
 */
static void naive_standard_attention(
    const float* Q, const float* K_full, const float* V_full,
    float* out,
    int batch, int num_q_heads, int seq, int head_dim, float scale)
{
    for (int b = 0; b < batch; b++) {
        for (int h = 0; h < num_q_heads; h++) {
            const float* Qbh = Q      + ((size_t)(b * num_q_heads + h) * seq * head_dim);
            const float* Kbh = K_full + ((size_t)(b * num_q_heads + h) * seq * head_dim);
            const float* Vbh = V_full + ((size_t)(b * num_q_heads + h) * seq * head_dim);
            float*       obh = out    + ((size_t)(b * num_q_heads + h) * seq * head_dim);

            float* S = (float*)malloc(seq * seq * sizeof(float));
            for (int i = 0; i < seq; i++) {
                for (int j = 0; j < seq; j++) {
                    float dot = 0.f;
                    for (int d = 0; d < head_dim; d++)
                        dot += Qbh[i*head_dim+d] * Kbh[j*head_dim+d];
                    S[i*seq+j] = dot / scale;
                }
                /* Softmax row i. */
                float mx = S[i*seq];
                for (int j=1;j<seq;j++) if(S[i*seq+j]>mx) mx=S[i*seq+j];
                float sm=0.f;
                for (int j=0;j<seq;j++){S[i*seq+j]=expf(S[i*seq+j]-mx);sm+=S[i*seq+j];}
                for (int j=0;j<seq;j++) S[i*seq+j]/=sm;
            }
            for (int i=0;i<seq;i++)
                for (int d=0;d<head_dim;d++){
                    float acc=0.f;
                    for (int j=0;j<seq;j++) acc+=S[i*seq+j]*Vbh[j*head_dim+d];
                    obh[i*head_dim+d]=acc;
                }
            free(S);
        }
    }
}

static void run_gqa_test(int batch, int num_q_heads, int num_kv_heads,
                          int seq, int head_dim, float tol, const char* label) {
    unsigned seed = 12345u + (unsigned)(num_q_heads * 1000 + seq);
    int group = num_q_heads / num_kv_heads;

    size_t q_total  = (size_t)batch * num_q_heads  * seq * head_dim;
    size_t kv_total = (size_t)batch * num_kv_heads * seq * head_dim;
    size_t qf_total = (size_t)batch * num_q_heads  * seq * head_dim;  /* expanded KV */

    float* Q      = (float*)malloc(q_total  * sizeof(float));
    float* K      = (float*)malloc(kv_total * sizeof(float));
    float* V      = (float*)malloc(kv_total * sizeof(float));
    float* K_full = (float*)malloc(qf_total * sizeof(float));  /* expanded */
    float* V_full = (float*)malloc(qf_total * sizeof(float));
    float* ref    = (float*)malloc(q_total  * sizeof(float));
    float* got    = (float*)malloc(q_total  * sizeof(float));

    for (size_t i = 0; i < q_total;  i++) Q[i] = randf_seed(&seed);
    for (size_t i = 0; i < kv_total; i++) K[i] = randf_seed(&seed);
    for (size_t i = 0; i < kv_total; i++) V[i] = randf_seed(&seed);

    /* Expand K/V: replicate each KV head to group_size Q heads. */
    for (int b = 0; b < batch; b++) {
        for (int hq = 0; hq < num_q_heads; hq++) {
            int hkv = hq / group;
            for (int s = 0; s < seq; s++) {
                for (int d = 0; d < head_dim; d++) {
                    size_t kv_off = ((size_t)(b*num_kv_heads+hkv)*seq + s)*head_dim + d;
                    size_t qf_off = ((size_t)(b*num_q_heads +hq)  *seq + s)*head_dim + d;
                    K_full[qf_off] = K[kv_off];
                    V_full[qf_off] = V[kv_off];
                }
            }
        }
    }

    float scale = sqrtf((float)head_dim);

    /* Reference: expand KV and run standard attention. */
    naive_standard_attention(Q, K_full, V_full, ref, batch, num_q_heads, seq, head_dim, scale);

    /* GQA kernel under test. */
    size_t arena_sz = (size_t)64 * 1024 * 1024;
    Arena* arena = arena_create(arena_sz);
    cpx_attention_gqa(Q, K, V, got, batch, num_q_heads, num_kv_heads, seq, head_dim, scale, arena);

    float diff = max_abs_diff(ref, got, (int)q_total);
    printf("  %-35s  BH=%d/%d seq=%3d dim=%2d  max_diff=%.2e  %s\n",
           label, num_q_heads, num_kv_heads, seq, head_dim, (double)diff,
           diff < tol ? "PASS" : "FAIL");
    assert(diff < tol);

    arena_destroy(arena);
    free(Q); free(K); free(V); free(K_full); free(V_full); free(ref); free(got);
}

int main(void) {
    printf("=== test_attention_gqa ===\n");

    /* 1. MQA: 4 Q heads share 1 KV head */
    run_gqa_test(1, 4, 1, 16, 32, 1e-4f, "MQA q=4 kv=1");

    /* 2. GQA 8Q/2KV: standard LLaMA-style config */
    run_gqa_test(1, 8, 2, 16, 32, 1e-4f, "GQA q=8 kv=2");

    /* 3. GQA 8Q/4KV batch=2 */
    run_gqa_test(2, 8, 4, 8, 16, 1e-4f, "GQA q=8 kv=4 batch=2");

    /* 4. GQA with group=1 (standard MHA, no sharing) */
    run_gqa_test(1, 4, 4, 12, 16, 1e-4f, "GQA group=1 (MHA)");

    printf("All GQA tests passed.\n");
    return 0;
}
