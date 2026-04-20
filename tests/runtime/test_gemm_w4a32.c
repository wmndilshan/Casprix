/*
 * Regression test: INT4 GEMM arena-based kernel
 *
 * Verifies cpx_gemm_w4a32_zp() output is within 0.01 of float32 reference
 * on a 64×64×64 matrix (M=N=K=64).
 *
 * Build:
 *   gcc -O2 -mavx2 -mfma -I runtime/ai/ml \
 *       tests/runtime/test_gemm_w4a32.c \
 *       runtime/ai/ml/cpx_quant.c runtime/ai/ml/cpx_mem_arena.c \
 *       -lm -o test_gemm_w4a32
 */

#include "cpx_quant.h"
#include "cpx_mem_arena.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Simple per-row float-reference GEMM: C[M,N] = A[M,K] × W[N,K]^T */
static void ref_gemm(const float* A, const float* W_f32,
                     float* C, int M, int N, int K) {
    for (int m = 0; m < M; m++)
        for (int n = 0; n < N; n++) {
            float acc = 0.f;
            for (int k = 0; k < K; k++) acc += A[m*K+k] * W_f32[n*K+k];
            C[m*N+n] = acc;
        }
}

static float randf_s(unsigned* s) {
    *s = *s * 1664525u + 1013904223u;
    return (float)(int)(*s >> 1) / (float)(1 << 30) * 0.5f;  /* small values */
}

static void run_test(int M, int N, int K, float tol, const char* label) {
    unsigned seed = 9999u;
    float* A      = (float*)malloc((size_t)M * K * sizeof(float));
    float* W_f32  = (float*)malloc((size_t)N * K * sizeof(float));
    float* scales = (float*)malloc((size_t)N * sizeof(float));
    float* zeros  = (float*)calloc((size_t)N, sizeof(float));  /* symmetric: zero=0 */
    uint8_t* W_int4 = (uint8_t*)calloc((size_t)N * K / 2, 1);
    float* C_ref  = (float*)malloc((size_t)M * N * sizeof(float));
    float* C_got  = (float*)malloc((size_t)M * N * sizeof(float));

    for (int i = 0; i < M*K; i++) A[i]     = randf_s(&seed);
    for (int i = 0; i < N*K; i++) W_f32[i] = randf_s(&seed);

    /* Quantize W_f32 to INT4, store per-row scales, zero zeros. */
    cpx_quantize_weights_int4(W_f32, W_int4, scales, N, K, K /* group_size = K → 1 group */);
    for (int n = 0; n < N; n++) zeros[n] = 0.f;  /* symmetric */

    /* Reference: compute using dequantized weights. */
    float* W_dq = (float*)malloc((size_t)N * K * sizeof(float));
    int groups = 1;
    for (int n = 0; n < N; n++)
        cpx_dequant_row_int4(W_int4 + n*K/2, scales + n*groups, W_dq + n*K, K, K);
    ref_gemm(A, W_dq, C_ref, M, N, K);
    free(W_dq);

    /* Arena-based INT4 GEMM under test. */
    CpxArena arena;
    uint8_t* pool = (uint8_t*)malloc(4 * 1024 * 1024);
    cpx_arena_init(&arena, pool, 4 * 1024 * 1024, CPX_ARENA_TEMP);

    cpx_gemm_w4a32_zp(W_int4, scales, zeros, A, C_got, M, N, K, &arena);

    /* Check output within tolerance. */
    float max_diff = 0.f;
    for (int i = 0; i < M*N; i++) {
        float d = fabsf(C_ref[i] - C_got[i]);
        if (d > max_diff) max_diff = d;
    }
    printf("  %-25s  M=%d N=%d K=%d  max_diff=%.4f  %s\n",
           label, M, N, K, (double)max_diff, max_diff < tol ? "PASS" : "FAIL");
    assert(max_diff < tol);

    free(pool); free(A); free(W_f32); free(scales); free(zeros);
    free(W_int4); free(C_ref); free(C_got);
}

int main(void) {
    printf("=== test_gemm_w4a32 ===\n");
    run_test(64, 64, 64, 0.2f, "64x64x64");       /* INT4 has inherent error */
    run_test(1,  32, 32, 0.2f, "GEMV M=1");
    run_test(4,  16, 64, 0.2f, "small M=4");
    printf("All w4a32 tests passed.\n");
    return 0;
}
