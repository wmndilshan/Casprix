/*
 * Casprix ML Runtime — Quantization Implementation
 */

#include "cpx_quant.h"
#include <string.h>
#include <math.h>
#include <assert.h>
#include <stdlib.h>

#if defined(__AVX2__) && defined(__FMA__)
#  include <immintrin.h>
#  define HAVE_AVX2 1
#else
#  define HAVE_AVX2 0
#endif

/* ════════════════════════════════════════════════════════════════════
 * 1. INT8 WEIGHT QUANTIZATION
 * ════════════════════════════════════════════════════════════════════ */

void cpx_quantize_weights_int8(const float* W_f32, int8_t* W_int8,
                                  float* scales, int N, int K) {
    for (int n = 0; n < N; n++) {
        const float* row = W_f32 + n * K;
        /* Find max absolute value for this row. */
        float max_abs = 0.f;
        for (int k = 0; k < K; k++) {
            float a = fabsf(row[k]);
            if (a > max_abs) max_abs = a;
        }
        float scale = (max_abs > 0.f) ? max_abs / 127.f : 1.f;
        scales[n] = scale;
        float inv_scale = 1.f / scale;
        int8_t* out = W_int8 + n * K;
        for (int k = 0; k < K; k++) {
            float v = row[k] * inv_scale;
            int r = (int)roundf(v);
            if (r >  127) r =  127;
            if (r < -127) r = -127;
            out[k] = (int8_t)r;
        }
    }
}

void cpx_quantize_activations_int8(const float* X_f32, int8_t* X_int8,
                                      float* scale, int B, int K) {
    int n = B * K;
    float max_abs = 0.f;
    for (int i = 0; i < n; i++) {
        float a = fabsf(X_f32[i]);
        if (a > max_abs) max_abs = a;
    }
    *scale = (max_abs > 0.f) ? max_abs / 127.f : 1.f;
    float inv = 1.f / *scale;
    for (int i = 0; i < n; i++) {
        float v = X_f32[i] * inv;
        int r = (int)roundf(v);
        if (r >  127) r =  127;
        if (r < -127) r = -127;
        X_int8[i] = (int8_t)r;
    }
}

void cpx_dequantize_output_int8(const int32_t* Y_int32, float* Y_f32,
                                   const float* scale_a,
                                   const float* scale_w,
                                   int B, int N) {
    for (int b = 0; b < B; b++) {
        for (int n = 0; n < N; n++) {
            Y_f32[b * N + n] = (float)Y_int32[b * N + n]
                                * (*scale_a) * scale_w[n];
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
 * 2. INT4 WEIGHT QUANTIZATION (per-group)
 * ════════════════════════════════════════════════════════════════════ */

void cpx_quantize_weights_int4(const float* W_f32,
                                  uint8_t* W_int4,
                                  float* scales,
                                  int N, int K, int group_size) {
    int groups_per_row = (K + group_size - 1) / group_size;
    for (int n = 0; n < N; n++) {
        const float* row = W_f32 + n * K;
        for (int g = 0; g < groups_per_row; g++) {
            int k_start = g * group_size;
            int k_end   = k_start + group_size;
            if (k_end > K) k_end = K;
            int len = k_end - k_start;

            /* Find max abs value for this group. */
            float max_abs = 0.f;
            for (int k = k_start; k < k_end; k++) {
                float a = fabsf(row[k]);
                if (a > max_abs) max_abs = a;
            }
            float scale = (max_abs > 0.f) ? max_abs / 7.f : 1.f;
            scales[n * groups_per_row + g] = scale;
            float inv = 1.f / scale;

            /* Pack INT4 values (range -8..7, symmetric shifted to 0..15). */
            for (int i = 0; i < len; i++) {
                int k = k_start + i;
                int v = (int)roundf(row[k] * inv);
                if (v >  7) v =  7;
                if (v < -8) v = -8;
                /* Store as unsigned nibble: add 8 to shift to 0..15 range. */
                uint8_t nibble = (uint8_t)(v + 8) & 0x0F;
                int byte_idx = (n * K + k) / 2;
                int shift    = ((n * K + k) % 2) * 4;
                if (shift == 0)
                    W_int4[byte_idx]  = nibble;
                else
                    W_int4[byte_idx] |= (nibble << 4);
            }
        }
    }
}

void cpx_dequant_row_int4(const uint8_t* row_int4,
                             const float* row_scales,
                             float* out_f32,
                             int K, int group_size) {
    int groups = (K + group_size - 1) / group_size;
    for (int g = 0; g < groups; g++) {
        int k_start = g * group_size;
        int k_end   = k_start + group_size;
        if (k_end > K) k_end = K;
        float scale = row_scales[g];

        for (int k = k_start; k < k_end; k++) {
            int byte_idx = k / 2;
            int shift    = (k % 2) * 4;
            uint8_t nibble = (row_int4[byte_idx] >> shift) & 0x0F;
            int v = (int)nibble - 8;  /* restore signed range -8..7 */
            out_f32[k] = (float)v * scale;
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
 * 3. W8A8 GEMM
 * C[B, N] = A_int8[B, K] × W_int8[N, K]^T × (scale_a × scale_w[n])
 * Uses INT32 accumulation.
 * ════════════════════════════════════════════════════════════════════ */

void CPX_HOT cpx_gemm_w8a8(const int8_t* A, const int8_t* W,
                              float* C,
                              float scale_a, const float* scale_w,
                              int B, int K, int N,
                              struct CpxScheduler* sched) {
    /* Per-thread row range — simple chunked parallelism. */
    (void)sched; /* TODO: wire into scheduler parallel_for */

    for (int b = 0; b < B; b++) {
        const int8_t* a = A + b * K;
        for (int n = 0; n < N; n++) {
            const int8_t* w = W + n * K;
            int32_t acc = 0;

#if HAVE_AVX2
            __m256i sum = _mm256_setzero_si256();
            int k = 0;
            for (; k <= K - 32; k += 32) {
                /* Load 32 INT8 values and widen to INT16 for multiply. */
                __m128i a8 = _mm_loadu_si128((const __m128i*)(a + k));
                __m128i w8 = _mm_loadu_si128((const __m128i*)(w + k));
                /* Sign-extend to 16-bit. */
                __m256i a16 = _mm256_cvtepi8_epi16(a8);
                __m256i w16 = _mm256_cvtepi8_epi16(w8);
                /* Multiply and add pairs → 32-bit. */
                __m256i prod = _mm256_madd_epi16(a16, w16);
                sum = _mm256_add_epi32(sum, prod);

                /* Second 16 bytes. */
                __m128i a8b = _mm_loadu_si128((const __m128i*)(a + k + 16));
                __m128i w8b = _mm_loadu_si128((const __m128i*)(w + k + 16));
                __m256i a16b = _mm256_cvtepi8_epi16(a8b);
                __m256i w16b = _mm256_cvtepi8_epi16(w8b);
                __m256i prodb = _mm256_madd_epi16(a16b, w16b);
                sum = _mm256_add_epi32(sum, prodb);
            }
            /* Horizontal reduce the 8 INT32 lanes. */
            __m128i lo = _mm256_extracti128_si256(sum, 0);
            __m128i hi = _mm256_extracti128_si256(sum, 1);
            __m128i s2 = _mm_add_epi32(lo, hi);
            s2 = _mm_hadd_epi32(s2, s2);
            s2 = _mm_hadd_epi32(s2, s2);
            acc = _mm_extract_epi32(s2, 0);
            /* Remainder. */
            for (; k < K; k++) acc += (int32_t)a[k] * (int32_t)w[k];
#else
            for (int k = 0; k < K; k++)
                acc += (int32_t)a[k] * (int32_t)w[k];
#endif
            C[b * N + n] = (float)acc * scale_a * scale_w[n];
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
 * 4. W4A32 GEMM (dequantise on the fly)
 * ════════════════════════════════════════════════════════════════════ */

void CPX_HOT cpx_gemm_w4a32(const float* A,
                               const uint8_t* W_int4,
                               const float* W_scales,
                               float* C,
                               int B, int K, int N,
                               int group_size,
                               struct CpxScheduler* sched) {
    (void)sched;
    int groups_per_row = (K + group_size - 1) / group_size;

    /* Temporary dequantized row. */
    float* w_row = (float*)malloc((size_t)K * sizeof(float));

    for (int n = 0; n < N; n++) {
        /* Dequantize W row n into w_row. */
        const uint8_t* wn_int4 = W_int4 + n * K / 2;
        cpx_dequant_row_int4(wn_int4, W_scales + n * groups_per_row,
                               w_row, K, group_size);

        for (int b = 0; b < B; b++) {
            const float* a = A + b * K;
            float acc = 0.f;
            int k = 0;
#if HAVE_AVX2
            __m256 vsum = _mm256_setzero_ps();
            for (; k <= K - 8; k += 8) {
                vsum = _mm256_fmadd_ps(_mm256_loadu_ps(a + k),
                                        _mm256_loadu_ps(w_row + k),
                                        vsum);
            }
            float buf[8];
            _mm256_storeu_ps(buf, vsum);
            for (int j = 0; j < 8; j++) acc += buf[j];
#endif
            for (; k < K; k++) acc += a[k] * w_row[k];
            C[b * N + n] = acc;
        }
    }
    free(w_row);
}

/* ════════════════════════════════════════════════════════════════════
 * 5. BF16 VECTORISED CONVERSIONS
 * ════════════════════════════════════════════════════════════════════ */

void cpx_cvt_f32_to_bf16(const float* src, bf16* dst, int n) {
    int i = 0;
#if HAVE_AVX2
    for (; i <= n - 8; i += 8) {
        __m256i f = _mm256_loadu_si256((const __m256i*)(src + i));
        /* Shift right 16 to keep upper 16 bits (BF16). */
        __m256i b = _mm256_srli_epi32(f, 16);
        /* Pack 32→16 (saturate unused — values fit in uint16). */
        __m128i lo = _mm256_extracti128_si256(b, 0);
        __m128i hi = _mm256_extracti128_si256(b, 1);
        __m128i packed = _mm_packus_epi32(lo, hi);
        _mm_storeu_si128((__m128i*)(dst + i), packed);
    }
#endif
    for (; i < n; i++) dst[i] = cpx_f32_to_bf16(src[i]);
}

void cpx_cvt_bf16_to_f32(const bf16* src, float* dst, int n) {
    int i = 0;
#if HAVE_AVX2
    for (; i <= n - 8; i += 8) {
        __m128i s = _mm_loadu_si128((const __m128i*)(src + i));
        /* Unpack uint16 → int32, shift left 16. */
        __m256i wide = _mm256_cvtepu16_epi32(s);
        __m256i shifted = _mm256_slli_epi32(wide, 16);
        _mm256_storeu_ps(dst + i, _mm256_castsi256_ps(shifted));
    }
#endif
    for (; i < n; i++) dst[i] = cpx_bf16_to_f32(src[i]);
}

/* ════════════════════════════════════════════════════════════════════
 * 6. QAT FAKE QUANTIZE
 * ════════════════════════════════════════════════════════════════════ */

void cpx_fake_quantize_f32(const float* w, float* w_fq,
                              const float* scale, int N, int K) {
    float s = *scale;
    float inv = 1.f / s;
    float lo = -127.f * s, hi = 127.f * s;
    for (int i = 0; i < N * K; i++) {
        float q = roundf(w[i] * inv) * s;
        if (q < lo) q = lo;
        if (q > hi) q = hi;
        w_fq[i] = q;
    }
}

void cpx_fake_quantize_bwd(const float* grad_out, const float* w,
                              const float* scale, float* grad_in,
                              int N, int K) {
    float lo = -127.f * *scale, hi = 127.f * *scale;
    for (int i = 0; i < N * K; i++) {
        grad_in[i] = (w[i] >= lo && w[i] <= hi) ? grad_out[i] : 0.f;
    }
}
