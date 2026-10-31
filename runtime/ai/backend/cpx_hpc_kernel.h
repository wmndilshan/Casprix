/*
 * Casprix ML Runtime — SIMD Abstraction & HPC Micro-Kernels
 *
 * ════════════════════════════════════════════════════════════════════
 * EXECUTION MODEL
 * ════════════════════════════════════════════════════════════════════
 *
 * All kernels target a three-level register/cache hierarchy:
 *
 *   L1 (32 KiB, ~4 cycles)   — micro-kernel register tile lives here
 *   L2 (256 KiB, ~12 cycles) — panel tile (A-panel or B-panel)
 *   L3 (shared, ~40 cycles)  — block tile (outermost loop)
 *
 * GEMM tiling strategy (Goto-style):
 *
 *   for kc in [0, K, KC):          # fits B-panel in L3
 *     pack B-panel B[kc:kc+KC, :]  # contiguous for L2 prefetch
 *     for mc in [0, M, MC):        # fits A-panel in L2
 *       pack A-panel A[mc:mc+MC, kc:kc+KC]
 *       for nc in [0, N, NC):      # register tile
 *         micro_kernel(mr, nr)     # fully unrolled AVX2/AVX-512
 *
 * Blocking parameters for AVX2 (256-bit = 8 floats):
 *   MR = 6   (rows of C held in registers)
 *   NR = 16  (cols of C = 2 × 256-bit registers)
 *   MC = 288 (fits A-panel in L2: 288×KC×4 bytes)
 *   NC = 512 (fits B-panel in L3: KC×512×4 bytes)
 *   KC = 256 (depth of micro-kernel)
 *
 * AVX-512 variant doubles NR to 32 (4 × 512-bit registers per row).
 *
 * ════════════════════════════════════════════════════════════════════
 * ATTENTION KERNEL
 * ════════════════════════════════════════════════════════════════════
 *
 * Flash-attention style: tiles of (Q, K, V) processed together so
 * the attention matrix never materialises fully in memory.
 *
 *   for q_tile in Q:
 *     acc_O = 0,  acc_l = 0,  acc_m = -inf    # running softmax state
 *     for kv_tile in K,V:
 *       S = Q_tile × K_tile^T / sqrt(d_head)  # [Bq × Bkv] — fits L1
 *       m_new = rowmax(S)
 *       P = exp(S - m_new)                    # numerically stable
 *       l_new = rowsum(P)
 *       rescale acc_O by exp(m_old - m_new)
 *       acc_O += P × V_tile
 *       update running (m, l)
 *     O_tile = acc_O / acc_l                  # normalise
 *
 * Tile size Bq=64, Bkv=64 for typical d_head=64/128; fits in L1.
 */

#ifndef CPX_HPC_KERNEL_H
#define CPX_HPC_KERNEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <float.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════════
 * 1. CPU FEATURE DETECTION
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    bool avx;
    bool avx2;
    bool avx512f;
    bool avx512bw;
    bool avx512vnni;    /* INT8 dot-product instructions               */
    bool fma;
    bool f16c;          /* half-precision conversion                   */
    bool amx;           /* Intel AMX (tile compute, Sapphire Rapids+)  */
    int  num_physical_cores;
    int  num_logical_cores;
    int  l1_cache_bytes;
    int  l2_cache_bytes;
    int  l3_cache_bytes;
    int  cache_line_bytes;
    bool has_numa;
    int  num_numa_nodes;
} CpxCpuInfo;

/* Detect CPU capabilities at runtime. */
void cpx_cpu_info(CpxCpuInfo* info);
static inline void cpx_cpu_detect(CpxCpuInfo* info) { cpx_cpu_info(info); }

/* ════════════════════════════════════════════════════════════════════
 * 2. SIMD TYPE ALIASES
 * ════════════════════════════════════════════════════════════════════ */

typedef float     f32;
typedef uint16_t  f16;
typedef int8_t    i8;
typedef int32_t   i32;
typedef uint32_t  u32;
typedef int64_t   i64;

/* Cache-line size (always 64 bytes on x86). */
#define CPX_CACHE_LINE  64

/* SIMD vector widths in floats. */
#define CPX_VEC_WIDTH_AVX2    8
#define CPX_VEC_WIDTH_AVX512  16

/* Alignment macros. */
#define CPX_ALIGN(x)    __attribute__((aligned(x)))
#define CPX_ALIGNAS64   CPX_ALIGN(64)
#define CPX_FORCE_INLINE __attribute__((always_inline)) static inline
#define CPX_NOINLINE    __attribute__((noinline))
#define CPX_HOT         __attribute__((hot))
#define CPX_COLD        __attribute__((cold))

/* ════════════════════════════════════════════════════════════════════
 * 3. GEMM BLOCKING CONSTANTS (auto-tuned at runtime, these are defaults)
 * ════════════════════════════════════════════════════════════════════ */

/* Micro-kernel register tile (AVX2). */
#define GEMM_MR_AVX2     6
#define GEMM_NR_AVX2    16   /* 2 × ymm registers wide */

/* Micro-kernel register tile (AVX-512). */
#define GEMM_MR_AVX512   8
#define GEMM_NR_AVX512  32   /* 2 × zmm registers wide */

/* Macro-kernel panel sizes (bytes tuned for typical L2/L3). */
#define GEMM_MC_DEFAULT   288
#define GEMM_NC_DEFAULT   512
#define GEMM_KC_DEFAULT   256

/* Packed-panel alignment (must be >= 64). */
#define GEMM_PACK_ALIGN   64

/* ════════════════════════════════════════════════════════════════════
 * 4. PACKED PANEL BUFFERS
 *
 * Packing linearises memory access patterns:
 *   A-panel: [MC, KC]  — row-major, 64-byte aligned
 *   B-panel: [KC, NC]  — column-major (transposed), 64-byte aligned
 *
 * This ensures that within the micro-kernel inner loop, both A and B
 * streams are sequential → maximises prefetch effectiveness.
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    f32*  data;             /* aligned allocation                      */
    int   rows, cols;       /* logical dimensions                      */
    int   stride;           /* leading dimension in floats             */
} PackedPanel;

PackedPanel* packed_panel_create(int rows, int cols);
void         packed_panel_destroy(PackedPanel* p);

/* Pack a sub-matrix of A into the A-panel buffer. */
void gemm_pack_a(const f32* A, int lda, f32* panel_a,
                 int mc, int kc, int mr);

/* Pack a sub-matrix of B into the B-panel buffer (transposed). */
void gemm_pack_b(const f32* B, int ldb, f32* panel_b,
                 int kc, int nc, int nr);

/* ════════════════════════════════════════════════════════════════════
 * 5. MICRO-KERNEL FUNCTION POINTERS
 *
 * The micro-kernel is the innermost loop, fully unrolled.
 * Signature: compute C[mr,nr] += A[mr,kc] * B[kc,nr].
 * A and B are pre-packed (contiguous).
 * C is stored back to the output matrix in-place.
 *
 * Multiple variants exist (AVX2 / AVX-512 / scalar fallback).
 * The runtime selects the best one at startup.
 * ════════════════════════════════════════════════════════════════════ */

typedef void (*GemmMicroKernelFn)(
    int kc,
    const f32* CPX_ALIGNAS64 a,     /* A-panel column stripe [mr × kc] */
    const f32* CPX_ALIGNAS64 b,     /* B-panel row stripe [kc × nr]    */
    f32* c,                          /* output tile [mr × nr], row-major */
    int ldc                          /* leading dim of C                */
);

/* ════════════════════════════════════════════════════════════════════
 * 6. GEMM API
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    int M, K, N;
    int lda, ldb, ldc;
    f32 alpha, beta;
    bool transA, transB;

    /* Tile overrides (0 = use runtime defaults). */
    int mc, nc, kc;

    /* Threading: how many threads to use. */
    int num_threads;
} GemmParams;

/* Single-threaded GEMM with packing (Goto algorithm). */
void CPX_HOT cpx_sgemm(const GemmParams* p,
                        const f32* A, const f32* B, f32* C);

/* Multi-threaded GEMM — splits M dimension across threads. */
void CPX_HOT cpx_sgemm_mt(const GemmParams* p,
                            const f32* A, const f32* B, f32* C,
                            struct CpxScheduler* sched);

/* Batched GEMM: processes `batch` independent M×K × K×N products.
 * A[b], B[b], C[b] are pointer arrays. */
void cpx_sgemm_batched(const GemmParams* p,
                        const f32** A, const f32** B, f32** C,
                        int batch, struct CpxScheduler* sched);

/* ════════════════════════════════════════════════════════════════════
 * 7. ATTENTION KERNELS
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    int batch;
    int num_heads;
    int seq_len_q;
    int seq_len_kv;
    int head_dim;
    float scale;            /* 1/sqrt(head_dim)                        */
    bool causal;            /* apply causal mask                       */
    bool is_decode;         /* single-token decode (seq_len_q == 1)    */
    int  tile_q;            /* 0 = auto (default 64)                   */
    int  tile_kv;           /* 0 = auto (default 64)                   */
} AttentionParams;

/*
 * Flash-attention (tiled, online softmax).
 * Q: [B, H, Sq, D]  K: [B, H, Skv, D]  V: [B, H, Skv, D]
 * O: [B, H, Sq, D]
 * All inputs are row-major with standard transformer strides.
 *
 * Memory: O(Bq*D + Bkv*D) working set per tile — fits in L1 for D≤128.
 */
void CPX_HOT cpx_attention_flash(const AttentionParams* p,
                                  const f32* Q, const f32* K, const f32* V,
                                  f32* O,
                                  const f32* mask,      /* NULL = no mask */
                                  struct CpxScheduler* sched);

/*
 * Decode-phase attention: seq_len_q==1, seq_len_kv = full KV-cache length.
 * Single-vector × matrix product per head.
 * Fused with KV-cache read.
 */
void CPX_HOT cpx_attention_decode(const AttentionParams* p,
                                   const f32* q,   /* [B, H, D] */
                                   const f32* K,   /* [B, H, Skv, D] */
                                   const f32* V,   /* [B, H, Skv, D] */
                                   f32* o          /* [B, H, D] */);

/* ════════════════════════════════════════════════════════════════════
 * 8. NORMALIZATION KERNELS
 *
 * Both LayerNorm and RMSNorm are fused:
 *   - Single pass computes mean + variance
 *   - Second pass normalises + scales + shifts
 *   - AVX2: horizontal adds via _mm256_hadd_ps × 3 + correction
 * ════════════════════════════════════════════════════════════════════ */

/*
 * Fused LayerNorm forward.
 * x: [N, D]  gamma/beta: [D]  out: [N, D]
 * Saves (mean, rstd) per row for backward pass.
 */
void CPX_HOT cpx_layernorm_fwd(const f32* x, const f32* gamma, const f32* beta,
                                f32* out, f32* mean_out, f32* rstd_out,
                                int N, int D, f32 eps);

/*
 * Fused LayerNorm backward.
 * dout: [N,D]  x: [N,D]  gamma: [D]  mean/rstd: [N]
 * Outputs: dx: [N,D]  dgamma: [D]  dbeta: [D]
 *
 * Uses the compact Welford formula to avoid double-summation.
 */
void CPX_HOT cpx_layernorm_bwd(const f32* dout, const f32* x,
                                const f32* gamma,
                                const f32* mean, const f32* rstd,
                                f32* dx, f32* dgamma, f32* dbeta,
                                int N, int D);

/*
 * RMSNorm (no mean subtraction, used in LLaMA/Mistral/Qwen).
 * Fused: single-pass variance + normalize + scale.
 */
void CPX_HOT cpx_rmsnorm_fwd(const f32* x, const f32* gamma,
                               f32* out, f32* rstd_out,
                               int N, int D, f32 eps);

void CPX_HOT cpx_rmsnorm_bwd(const f32* dout, const f32* x,
                               const f32* gamma, const f32* rstd,
                               f32* dx, f32* dgamma,
                               int N, int D);

/* ════════════════════════════════════════════════════════════════════
 * 9. SOFTMAX KERNELS
 *
 * Three variants based on context:
 *   a) Training: full matrix [N, D], AVX2 horizontal max + sum
 *   b) Attention score row: fused with causal mask application
 *   c) Online (incremental): used in Flash-attention inner loop
 * ════════════════════════════════════════════════════════════════════ */

/* Full softmax [N, D] with numerically stable rowwise max-subtract. */
void CPX_HOT cpx_softmax_fwd(const f32* x, f32* out, int N, int D);

/* Masked softmax: applies causal triangular mask before softmax. */
void CPX_HOT cpx_softmax_causal(const f32* x, f32* out,
                                 int N, int D, int row_offset);

/* Softmax backward: dX = P * (dY - sum(dY*P)) */
void CPX_HOT cpx_softmax_bwd(const f32* dout, const f32* out,
                               f32* dx, int N, int D);

/* ════════════════════════════════════════════════════════════════════
 * 10. ACTIVATION KERNELS
 *
 * All fused: forward + stores intermediate for backward in one pass.
 * AVX2: 8 floats/cycle throughput for ReLU/GELU/SiLU.
 * ════════════════════════════════════════════════════════════════════ */

void CPX_HOT cpx_relu_fwd(const f32* x, f32* out, int n);
void CPX_HOT cpx_relu_bwd(const f32* dout, const f32* x, f32* dx, int n);

/* GELU exact (erf-based). ~4 FMA + 1 erf per element.
 * For throughput use gelu_approx below. */
void CPX_HOT cpx_gelu_fwd(const f32* x, f32* out, int n);
void CPX_HOT cpx_gelu_bwd(const f32* dout, const f32* x, f32* dx, int n);

/* GELU approximation: x * 0.5 * (1 + tanh(0.7978845*(x + 0.044715*x^3)))
 * 2× faster than exact, <0.01% error for typical activation ranges. */
void CPX_HOT cpx_gelu_approx_fwd(const f32* x, f32* out, int n);
void CPX_HOT cpx_gelu_approx_bwd(const f32* dout, const f32* x, f32* dx, int n);

/* SiLU/Swish (used in LLaMA/Mistral FFN gate). */
void CPX_HOT cpx_silu_fwd(const f32* x, f32* out, int n);
void CPX_HOT cpx_silu_bwd(const f32* dout, const f32* x, f32* dx, int n);

/* ════════════════════════════════════════════════════════════════════
 * 11. ELEMENTWISE UTILITIES
 * ════════════════════════════════════════════════════════════════════ */

/* out = a + b  (fused: 1 load pair + 1 store, 2 FLOPs/element) */
void cpx_vadd_f32(const f32* a, const f32* b, f32* out, int n);

/* out = alpha*a + beta*b */
void cpx_vaxpby_f32(f32 alpha, const f32* a, f32 beta,
                     const f32* b, f32* out, int n);

/* Fused multiply-add: out = a * b + c */
void cpx_vfma_f32(const f32* a, const f32* b, const f32* c,
                   f32* out, int n);

/* Horizontal sum, mean, max, min — AVX2 tree-reduce. */
f32 cpx_vsum_f32(const f32* x, int n);
f32 cpx_vmean_f32(const f32* x, int n);
f32 cpx_vmax_f32(const f32* x, int n);

/* ════════════════════════════════════════════════════════════════════
 * 12. PREFETCH HELPERS
 * ════════════════════════════════════════════════════════════════════ */

/* Software prefetch hints (wrap __builtin_prefetch). */
#define CPX_PREFETCH_L1(ptr)    __builtin_prefetch((ptr), 0, 3)
#define CPX_PREFETCH_L2(ptr)    __builtin_prefetch((ptr), 0, 2)
#define CPX_PREFETCH_L3(ptr)    __builtin_prefetch((ptr), 0, 1)
#define CPX_PREFETCH_NTA(ptr)   __builtin_prefetch((ptr), 0, 0)
#define CPX_PREFETCH_W(ptr)     __builtin_prefetch((ptr), 1, 1)

/* Distance-ahead prefetch for streaming loops. */
#define CPX_PREFETCH_AHEAD(base, i, ahead, locality) \
    __builtin_prefetch((base) + (i) + (ahead), 0, (locality))

#ifdef __cplusplus
}
#endif

#endif /* CPX_HPC_KERNEL_H */
