/*
 * Casprix ML Runtime — Quantization Engine
 *
 * ════════════════════════════════════════════════════════════════════
 * QUANTIZATION MODEL
 * ════════════════════════════════════════════════════════════════════
 *
 * Supported formats:
 *
 *   INT8 symmetric (W8A8):
 *     weight_int8 = round(weight_f32 / scale)  ∈ [-127, 127]
 *     output_f32  = (A_int8 × W_int8) * (scale_A × scale_W)
 *     Kernel: VNNI-accelerated on AVX512VNNI (_mm512_dpbusds_epi32),
 *             falls back to AVX2 _mm256_madd_epi16 on older CPUs.
 *
 *   INT4 packed (W4A16 — weights only, activations in FP16/F32):
 *     Two INT4 values packed per byte (low nibble / high nibble).
 *     weight_f32 = int4_val * scale + zero_point  (per group-of-128)
 *     Dequantize to FP32 on-the-fly during GEMM.
 *     Memory: 2× compression vs FP8, 8× vs FP32.
 *     Kernel: dequant 8 bytes → 16 floats per cycle on AVX2.
 *
 *   BF16 (mixed precision):
 *     Activations: FP32 compute, BF16 storage (truncate mantissa).
 *     Weights: BF16 → upcast to FP32 in GEMM inner loop.
 *     Advantage: same dynamic range as FP32, half memory bandwidth.
 *     Available: natively on GCC with __bf16 on x86 w/ AVX512BF16.
 *
 * ════════════════════════════════════════════════════════════════════
 * QUANTIZATION GRANULARITY
 * ════════════════════════════════════════════════════════════════════
 *
 * Per-tensor:   one scale per entire weight matrix (cheapest)
 * Per-channel:  one scale per output channel (row of W)
 * Per-group:    one scale per group of G=128 elements along K dim
 *               (best accuracy; standard in GGML/llama.cpp)
 *
 * Group quantization (G=128):
 *   For W: [N, K], we have K/G groups per row → [N, K/G] scales.
 *   Each group stored as: 128 × INT4 packed bytes + 1 FP16 scale.
 *   Memory per row: 128/2 + 2 = 66 bytes vs 512 bytes FP32 → 7.8×
 *
 * ════════════════════════════════════════════════════════════════════
 * QUANTIZED GEMM INNER LOOP (INT8 W8A8)
 * ════════════════════════════════════════════════════════════════════
 *
 * For each micro-tile [MR × NR]:
 *   Accumulate in INT32: C_int32[m,n] += A_int8[m,k] * W_int8[n,k]
 *   After tile: C_f32 = C_int32 * (scale_A × scale_W[n])
 *
 * VNNI path (_mm512_dpbusds_epi32):
 *   Computes 4 INT8 dot-products per instruction (4× throughput vs FP32).
 *   64 × INT8 accumulates per cycle per AVX-512 port.
 *   Peak: 1.5 TOPS on a 3 GHz SKX with 2 ports.
 *
 * ════════════════════════════════════════════════════════════════════
 * SPECULATIVE DECODING INTEGRATION
 * ════════════════════════════════════════════════════════════════════
 *
 * The draft model (small, e.g. INT4) generates K candidate tokens.
 * The target model (large, e.g. INT8) verifies all K in one forward.
 * Accepted tokens skip expensive target sampling.
 * CPU advantage: draft model fits entirely in L3; verification
 * is memory-bound on the KV-cache read.
 */

#ifndef CPX_QUANT_H
#define CPX_QUANT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "cpx_hpc_kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════════
 * 1. QUANTIZATION PARAMETERS
 * ════════════════════════════════════════════════════════════════════ */

typedef enum {
    QUANT_NONE     = 0,
    QUANT_INT8_SYM,        /* symmetric: zero_point = 0               */
    QUANT_INT8_AFF,        /* affine: scale + zero_point              */
    QUANT_INT4_PACKED,     /* 2× INT4 per byte, per-group scale       */
    QUANT_BF16,            /* bfloat16 storage, f32 compute           */
    QUANT_F16,             /* float16 storage, f32 compute            */
} QuantType;

/* Per-tensor or per-channel quantization scale/zp. */
typedef struct {
    QuantType type;
    float*    scales;       /* [1] per-tensor, [N] per-channel,
                               [N × K/G] per-group                    */
    int8_t*   zero_points;  /* NULL for symmetric quantization        */
    int       num_scales;
    int       group_size;   /* 0 = per-tensor/channel; 128 = typical  */
} QuantParams;

/* ════════════════════════════════════════════════════════════════════
 * 2. INT8 WEIGHT QUANTIZATION
 * ════════════════════════════════════════════════════════════════════ */

/*
 * Quantize weight matrix [N, K] to INT8, per-channel (one scale/row).
 * scale[n] = max(|W[n,:]|) / 127
 * w_int8[n,k] = round(W[n,k] / scale[n])
 */
void cpx_quantize_weights_int8(const float* W_f32, int8_t* W_int8,
                                 float* scales,
                                 int N, int K);

/*
 * Quantize activation tensor [B, K] to INT8.
 * Dynamic: scale computed per-call (cannot use precomputed).
 * scale = max(|X|) / 127
 */
void cpx_quantize_activations_int8(const float* X_f32, int8_t* X_int8,
                                     float* scale,
                                     int B, int K);

/* Dequantize INT8 output [B, N] back to FP32. */
void cpx_dequantize_output_int8(const int32_t* Y_int32, float* Y_f32,
                                  const float* scale_a,
                                  const float* scale_w,  /* [N] */
                                  int B, int N);

/* ════════════════════════════════════════════════════════════════════
 * 3. INT4 WEIGHT QUANTIZATION (group-wise)
 * ════════════════════════════════════════════════════════════════════ */

/*
 * Pack FP32 weights into INT4 group-quantized format.
 *   W_f32:  [N, K]
 *   W_int4: [N, K/2]  — two INT4 per byte (low/high nibble)
 *   scales: [N, K/group_size]
 *
 * group_size: typically 32, 64, or 128.
 */
void cpx_quantize_weights_int4(const float* W_f32,
                                  uint8_t* W_int4,
                                  float* scales,
                                  int N, int K, int group_size);

/*
 * Dequantize a single INT4 row [K] to FP32 on the fly.
 * Used during GEMV (decode phase) when activations are FP32.
 */
void cpx_dequant_row_int4(const uint8_t* row_int4,
                             const float* row_scales,
                             float* out_f32,
                             int K, int group_size);

/* ════════════════════════════════════════════════════════════════════
 * 4. QUANTIZED GEMM
 *
 * W8A8: INT8 weights × INT8 activations → INT32 accumulate → FP32 output
 * W4A32: INT4 weights × FP32 activations → FP32 output (dequant inline)
 * ════════════════════════════════════════════════════════════════════ */

/*
 * W8A8 GEMM: C_f32 = A_int8 × W_int8 × (scale_a * scale_w[n])
 * A: [B, K]  W: [N, K]  C: [B, N]
 * Uses AVX512VNNI if available, else AVX2 16-bit accumulation.
 */
void CPX_HOT cpx_gemm_w8a8(const int8_t* A, const int8_t* W,
                             float* C,
                             float scale_a, const float* scale_w,
                             int B, int K, int N,
                             struct CpxScheduler* sched);

/*
 * W4A32 GEMM: C_f32 = A_f32 × dequant(W_int4)
 * Dequantizes W tile-by-tile during multiply — never fully materialises.
 */
void CPX_HOT cpx_gemm_w4a32(const float* A,
                              const uint8_t* W_int4,
                              const float* W_scales,
                              float* C,
                              int B, int K, int N,
                              int group_size,
                              struct CpxScheduler* sched);

/* ════════════════════════════════════════════════════════════════════
 * 5. BF16 UTILITIES
 * ════════════════════════════════════════════════════════════════════ */

/* BF16 is the upper 16 bits of a FP32 IEEE float (truncate mantissa).
 * Conversion is a right-shift + optional rounding.              */

typedef uint16_t bf16;

/* Convert FP32 → BF16 (truncate, no rounding). */
CPX_FORCE_INLINE bf16 cpx_f32_to_bf16(float x) {
    uint32_t bits;
    __builtin_memcpy(&bits, &x, 4);
    return (bf16)(bits >> 16);
}

/* Convert BF16 → FP32. */
CPX_FORCE_INLINE float cpx_bf16_to_f32(bf16 x) {
    uint32_t bits = (uint32_t)x << 16;
    float f;
    __builtin_memcpy(&f, &bits, 4);
    return f;
}

/* Vectorised FP32→BF16 conversion for entire arrays. */
void cpx_cvt_f32_to_bf16(const float* src, bf16* dst, int n);
void cpx_cvt_bf16_to_f32(const bf16* src, float* dst, int n);

/* ════════════════════════════════════════════════════════════════════
 * 6. QUANTIZATION-AWARE TRAINING (QAT) — straight-through estimator
 * ════════════════════════════════════════════════════════════════════ */

/*
 * Fake-quantize: round weights to INT8 grid during forward, pass
 * gradients straight through during backward.
 * w_fq = clamp(round(w / scale) * scale, -127*scale, 127*scale)
 */
void cpx_fake_quantize_f32(const float* w, float* w_fq,
                              const float* scale, int N, int K);

/* Gradient of fake-quantize (straight-through: dL/dw = dL/dw_fq
 * for |w/scale| <= 127, else 0). */
void cpx_fake_quantize_bwd(const float* grad_out, const float* w,
                              const float* scale,
                              float* grad_in,
                              int N, int K);

/* ════════════════════════════════════════════════════════════════════
 * 7. W4A32 GEMM WITH ZERO-POINT — arena-based hot path
 *
 * C[M, N] = A[M, K] × dequant(W_int4[N, K/2])
 * W_int4:  packed nibbles, 2 weights per byte, row-major [N, K/2]
 * scales:  per-row scale [N]
 * zeros:   per-row zero-point [N]  (NULL → symmetric, zero = 0)
 * All scratch allocated from arena — no malloc in hot path.
 * ════════════════════════════════════════════════════════════════════ */
void CPX_HOT cpx_gemm_w4a32_zp(const uint8_t* W_int4,
                                  const float*   scales,
                                  const float*   zeros,
                                  const float*   A,
                                  float*         C,
                                  int M, int N, int K,
                                  struct CpxArena* arena);

#ifdef __cplusplus
}
#endif

#endif /* CPX_QUANT_H */
