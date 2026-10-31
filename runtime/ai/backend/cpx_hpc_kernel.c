/*
 * Casprix ML Runtime — HPC Kernel Implementations
 *
 * Micro-kernel register tile (AVX2, MR=6, NR=16):
 *
 *   C[6×16] accumulates in 12 ymm registers (c00..c51, each ymm = 8 floats).
 *   Inner loop per K iteration:
 *     - Load a[m]: _mm256_broadcast_ss  (1 cycle, 1 ymm reused for row)
 *     - Load b[0]: _mm256_load_ps       (1 cycle, ymm)
 *     - Load b[1]: _mm256_load_ps       (1 cycle, ymm)
 *     - 12× vfmadd231ps                 (1 cycle/FMA, ILP across 6 rows)
 *   FMA throughput: 16 FMAs/cycle on Skylake (2 ports × 8 floats).
 *   Achieved: 12 FMAs per iteration → 96 FLOPs per K-step.
 *   Peak for 6×16 tile: 96 FLOP / ~7 cycles ≈ 13.7 FLOP/cycle.
 *   Hardware peak (3 GHz × 16 FLOP/cycle): 48 GFLOPS/core.
 *
 * Pack A [M, K] → A_packed [MC, KC] row-major, MR rows per block:
 *   Panel [mc × kc] packed as MR × kc tiles, row-major within tile.
 *   Avoids gather during micro-kernel (sequential reads).
 *
 * Pack B [K, N] → B_packed [KC, NC] col-major (transposed):
 *   Panel [kc × nc] packed as kc × NR tiles, col-major within tile.
 *   Inner loop reads B in sequential 32-byte (AVX) bursts.
 */

#include "cpx_hpc_kernel.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

#if defined(__AVX2__) && defined(__FMA__)
#  include <immintrin.h>
#  define HAVE_AVX2 1
#else
#  define HAVE_AVX2 0
#endif

#if defined(__AVX512F__) && defined(__AVX512BW__)
#  include <immintrin.h>
#  define HAVE_AVX512 1
#else
#  define HAVE_AVX512 0
#endif

/* ════════════════════════════════════════════════════════════════════
 * 1. CPU FEATURE DETECTION
 * ════════════════════════════════════════════════════════════════════ */

#if defined(_WIN32)
#  include <intrin.h>
#  define cpuid(leaf, a, b, c, d) \
     do { int r[4]; __cpuid(r, leaf); \
          a=r[0]; b=r[1]; c=r[2]; d=r[3]; } while(0)
#  define cpuidex(leaf, sub, a, b, c, d) \
     do { int r[4]; __cpuidex(r, leaf, sub); \
          a=r[0]; b=r[1]; c=r[2]; d=r[3]; } while(0)
#else
#  include <cpuid.h>
#  define cpuid(leaf, a, b, c, d)         __cpuid(leaf, a, b, c, d)
#  define cpuidex(leaf, sub, a, b, c, d)  __cpuid_count(leaf, sub, a, b, c, d)
#endif

static void cpuid_xgetbv(uint32_t xcr, uint32_t* lo, uint32_t* hi) {
#if defined(_WIN32)
    uint64_t v = _xgetbv(xcr);
    *lo = (uint32_t)v;
    *hi = (uint32_t)(v >> 32);
#else
    __asm__ __volatile__("xgetbv" : "=a"(*lo), "=d"(*hi) : "c"(xcr));
#endif
}

void cpx_cpu_info(CpxCpuInfo* out) {
    uint32_t eax, ebx, ecx, edx;
    uint32_t lo, hi;

    memset(out, 0, sizeof(*out));

    /* Basic feature flags (leaf 1). */
    cpuid(1, eax, ebx, ecx, edx);
    bool osxsave = (ecx >> 27) & 1;
    bool avx_hw  = (ecx >> 28) & 1;
    bool fma_hw  = (ecx >> 12) & 1;
    bool f16c_hw = (ecx >> 29) & 1;

    if (osxsave && avx_hw) {
        cpuid_xgetbv(0, &lo, &hi);
        bool ymm_ok = (lo & 0x6) == 0x6;
        bool zmm_ok = (lo & 0xE6) == 0xE6;

        if (ymm_ok) {
            out->avx  = true;
            out->fma  = fma_hw;
            out->f16c = f16c_hw;

            /* Extended features (leaf 7, subleaf 0). */
            cpuidex(7, 0, eax, ebx, ecx, edx);
            out->avx2       = (ebx >> 5)  & 1;
            bool avx512f    = zmm_ok && ((ebx >> 16) & 1);
            out->avx512f    = avx512f;
            out->avx512bw   = avx512f && ((ebx >> 30) & 1);
            out->avx512vnni = avx512f && ((ecx >> 11) & 1);
            out->amx        = avx512f && ((edx >> 24) & 1);
        }
    }

    /* Core counts via leaf 4 (deterministic cache). */
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    out->physical_cores = si.dwNumberOfProcessors;
    out->logical_cores  = si.dwNumberOfProcessors;
#else
    out->logical_cores  = 1;
    out->physical_cores = 1;
#endif

    /* Cache sizes via leaf 4. */
    for (int i = 0; i < 6; i++) {
        cpuidex(4, i, eax, ebx, ecx, edx);
        int type = eax & 0x1f;
        if (type == 0) break;
        int level    = (eax >> 5) & 7;
        int ways     = ((ebx >> 22) & 0x3ff) + 1;
        int parts    = ((ebx >> 12) & 0x3ff) + 1;
        int line     = (ebx & 0xfff) + 1;
        int sets     = ecx + 1;
        int size_kb  = (ways * parts * line * sets) / 1024;
        if (level == 1 && type == 1) out->l1d_kb  = size_kb;
        if (level == 2)              out->l2_kb   = size_kb;
        if (level == 3)              out->l3_kb   = size_kb;
    }
}

/* ════════════════════════════════════════════════════════════════════
 * 2. SCALAR GEMM (fallback)
 * ════════════════════════════════════════════════════════════════════ */

static void sgemm_scalar(int M, int K, int N,
                          float alpha,
                          const float* A, int lda,
                          const float* B, int ldb,
                          float beta,
                          float* C, int ldc) {
    for (int m = 0; m < M; m++) {
        for (int n = 0; n < N; n++) {
            float acc = 0.f;
            for (int k = 0; k < K; k++) {
                acc += A[m * lda + k] * B[k * ldb + n];
            }
            C[m * ldc + n] = alpha * acc + beta * C[m * ldc + n];
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
 * 3. AVX2 GEMM PACKING
 * ════════════════════════════════════════════════════════════════════ */

#define MR 6
#define NR 16
#define MC GEMM_MC
#define NC GEMM_NC
#define KC GEMM_KC

/* Pack A sub-matrix [mc × kc] from src into dst in MR × kc tiles. */
static void pack_a(const float* src, int lda,
                   float* dst, int mc, int kc) {
    float* ptr = dst;
    for (int i = 0; i < mc; i += MR) {
        int rows = (i + MR <= mc) ? MR : mc - i;
        for (int k = 0; k < kc; k++) {
            for (int ii = 0; ii < rows; ii++) {
                *ptr++ = src[(i + ii) * lda + k];
            }
            /* Pad to MR if partial panel. */
            for (int ii = rows; ii < MR; ii++) {
                *ptr++ = 0.f;
            }
        }
    }
}

/* Pack B sub-matrix [kc × nc] from src into dst in kc × NR tiles.
 * B is accessed as src[k * ldb + j]. Packed as NR columns together. */
static void pack_b(const float* src, int ldb,
                   float* dst, int kc, int nc) {
    float* ptr = dst;
    for (int j = 0; j < nc; j += NR) {
        int cols = (j + NR <= nc) ? NR : nc - j;
        for (int k = 0; k < kc; k++) {
            for (int jj = 0; jj < cols; jj++) {
                *ptr++ = src[k * ldb + (j + jj)];
            }
            for (int jj = cols; jj < NR; jj++) {
                *ptr++ = 0.f;
            }
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
 * 4. AVX2 6×16 MICRO-KERNEL
 *
 * Registers:
 *   ymm0..ymm5   — broadcast A[m, k] (one per row, reused each k)
 *   ymm6, ymm7   — B row k: [8 floats], [8 floats]  (two ymm = 16)
 *   ymm8..ymm13  — C tile rows 0..5, first 8 cols (ymm)
 *   ymm14..ymm19 — not used in 6×2 scheme; C laid out differently
 *
 * We use 6 rows × 2 ymm accumulators = 12 ymm for C tile.
 * Notation: c[m][0] = first 8 cols of row m, c[m][1] = next 8.
 * ════════════════════════════════════════════════════════════════════ */

#if HAVE_AVX2
/* A packed panel [MR × kc], B packed panel [kc × NR], C [MR × NR]. */
static CPX_FORCE_INLINE void microkernel_6x16(int kc,
                                               const float* a,
                                               const float* b,
                                               float* c, int ldc) {
    __m256 c00 = _mm256_loadu_ps(c + 0*ldc + 0);
    __m256 c01 = _mm256_loadu_ps(c + 0*ldc + 8);
    __m256 c10 = _mm256_loadu_ps(c + 1*ldc + 0);
    __m256 c11 = _mm256_loadu_ps(c + 1*ldc + 8);
    __m256 c20 = _mm256_loadu_ps(c + 2*ldc + 0);
    __m256 c21 = _mm256_loadu_ps(c + 2*ldc + 8);
    __m256 c30 = _mm256_loadu_ps(c + 3*ldc + 0);
    __m256 c31 = _mm256_loadu_ps(c + 3*ldc + 8);
    __m256 c40 = _mm256_loadu_ps(c + 4*ldc + 0);
    __m256 c41 = _mm256_loadu_ps(c + 4*ldc + 8);
    __m256 c50 = _mm256_loadu_ps(c + 5*ldc + 0);
    __m256 c51 = _mm256_loadu_ps(c + 5*ldc + 8);

    const float* ap = a;
    const float* bp = b;

    for (int k = 0; k < kc; k++) {
        __m256 b0 = _mm256_loadu_ps(bp);
        __m256 b1 = _mm256_loadu_ps(bp + 8);
        bp += NR;

        __m256 a0 = _mm256_broadcast_ss(ap + 0);
        __m256 a1 = _mm256_broadcast_ss(ap + 1);
        __m256 a2 = _mm256_broadcast_ss(ap + 2);
        __m256 a3 = _mm256_broadcast_ss(ap + 3);
        __m256 a4 = _mm256_broadcast_ss(ap + 4);
        __m256 a5 = _mm256_broadcast_ss(ap + 5);
        ap += MR;

        c00 = _mm256_fmadd_ps(a0, b0, c00);
        c01 = _mm256_fmadd_ps(a0, b1, c01);
        c10 = _mm256_fmadd_ps(a1, b0, c10);
        c11 = _mm256_fmadd_ps(a1, b1, c11);
        c20 = _mm256_fmadd_ps(a2, b0, c20);
        c21 = _mm256_fmadd_ps(a2, b1, c21);
        c30 = _mm256_fmadd_ps(a3, b0, c30);
        c31 = _mm256_fmadd_ps(a3, b1, c31);
        c40 = _mm256_fmadd_ps(a4, b0, c40);
        c41 = _mm256_fmadd_ps(a4, b1, c41);
        c50 = _mm256_fmadd_ps(a5, b0, c50);
        c51 = _mm256_fmadd_ps(a5, b1, c51);
    }

    _mm256_storeu_ps(c + 0*ldc + 0, c00);
    _mm256_storeu_ps(c + 0*ldc + 8, c01);
    _mm256_storeu_ps(c + 1*ldc + 0, c10);
    _mm256_storeu_ps(c + 1*ldc + 8, c11);
    _mm256_storeu_ps(c + 2*ldc + 0, c20);
    _mm256_storeu_ps(c + 2*ldc + 8, c21);
    _mm256_storeu_ps(c + 3*ldc + 0, c30);
    _mm256_storeu_ps(c + 3*ldc + 8, c31);
    _mm256_storeu_ps(c + 4*ldc + 0, c40);
    _mm256_storeu_ps(c + 4*ldc + 8, c41);
    _mm256_storeu_ps(c + 5*ldc + 0, c50);
    _mm256_storeu_ps(c + 5*ldc + 8, c51);
}
#endif /* HAVE_AVX2 */

/* ════════════════════════════════════════════════════════════════════
 * 5. TILED GEMM DRIVER (Goto-style 3-loop + micro-kernel)
 *
 * Loop order (outer to inner):
 *   1. Partition N into NC-wide panels  (B-panel stays in L2/L3)
 *   2. Partition K into KC-wide slabs   (A+B working set in L2)
 *   3. Partition M into MC-wide panels  (A-panel in L1)
 *   4. Micro-kernel MR×NR tile
 * ════════════════════════════════════════════════════════════════════ */

/* A_pack / B_pack — caller-allocated, 64-byte aligned. */
typedef struct {
    float* a_pack;  /* [MC × KC] */
    float* b_pack;  /* [KC × NC] */
} GemmScratch;

/* Allocate aligned scratch — simple malloc wrapper. */
static GemmScratch alloc_scratch(void) {
    GemmScratch s;
    s.a_pack = (float*)_aligned_malloc((size_t)MC * KC * sizeof(float), 64);
    s.b_pack = (float*)_aligned_malloc((size_t)KC * NC * sizeof(float), 64);
    return s;
}
static void free_scratch(GemmScratch* s) {
    _aligned_free(s->a_pack);
    _aligned_free(s->b_pack);
}

static void sgemm_tiled(int M, int K, int N,
                          float alpha,
                          const float* A, int lda,
                          const float* B, int ldb,
                          float beta,
                          float* C, int ldc) {
#if !HAVE_AVX2
    sgemm_scalar(M, K, N, alpha, A, lda, B, ldb, beta, C, ldc);
    return;
#else
    GemmScratch sc = alloc_scratch();

    /* Scale C by beta up front (or zero it). */
    if (beta == 0.f) {
        for (int m = 0; m < M; m++)
            memset(C + m * ldc, 0, N * sizeof(float));
    } else if (beta != 1.f) {
        for (int m = 0; m < M; m++)
            for (int n = 0; n < N; n++)
                C[m * ldc + n] *= beta;
    }

    for (int nc = 0; nc < N; nc += NC) {
        int nc_sz = (nc + NC <= N) ? NC : N - nc;

        for (int kc = 0; kc < K; kc += KC) {
            int kc_sz = (kc + KC <= K) ? KC : K - kc;

            /* Pack B panel [kc_sz × nc_sz]. */
            pack_b(B + kc * ldb + nc, ldb, sc.b_pack, kc_sz, nc_sz);

            for (int mc = 0; mc < M; mc += MC) {
                int mc_sz = (mc + MC <= M) ? MC : M - mc;

                /* Pack A panel [mc_sz × kc_sz]. */
                pack_a(A + mc * lda + kc, lda, sc.a_pack, mc_sz, kc_sz);

                /* Micro-kernel loop. */
                for (int mr = 0; mr < mc_sz; mr += MR) {
                    int mr_sz = (mr + MR <= mc_sz) ? MR : mc_sz - mr;
                    for (int nr = 0; nr < nc_sz; nr += NR) {
                        float* c_tile = C + (mc + mr) * ldc + (nc + nr);

                        const float* a_tile =
                            sc.a_pack + (mr / MR) * MR * kc_sz;
                        const float* b_tile =
                            sc.b_pack + (nr / NR) * kc_sz * NR;

                        if (mr_sz == MR && (nc_sz - nr) >= NR) {
                            microkernel_6x16(kc_sz, a_tile, b_tile,
                                             c_tile, ldc);
                        } else {
                            /* Edge tile — scalar fallback. */
                            int nr_sz = (nr + NR <= nc_sz) ? NR : nc_sz - nr;
                            for (int m2 = 0; m2 < mr_sz; m2++) {
                                for (int n2 = 0; n2 < nr_sz; n2++) {
                                    float acc = 0.f;
                                    for (int k2 = 0; k2 < kc_sz; k2++) {
                                        acc += a_tile[m2 * kc_sz + k2]
                                             * b_tile[k2 * NR + n2];
                                    }
                                    c_tile[m2 * ldc + n2] += alpha * acc;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /* Apply alpha to the non-edge (micro-kernel) region. */
    if (alpha != 1.f) {
        for (int m = 0; m < M; m++)
            for (int n = 0; n < N; n++)
                C[m * ldc + n] *= alpha;
    }

    free_scratch(&sc);
#endif
}

/* Public API. */
void cpx_sgemm(const GemmParams* p) {
    CPX_PREFETCH_L1(p->A);
    CPX_PREFETCH_L1(p->B);
    sgemm_tiled(p->M, p->K, p->N,
                p->alpha,
                p->A, p->lda,
                p->B, p->ldb,
                p->beta,
                p->C, p->ldc);
}

/* Multi-threaded GEMM — partition M across threads. */
typedef struct { const GemmParams* p; int m_start; int m_end; } MtGemmArg;

static void mt_gemm_worker(void* arg_) {
    MtGemmArg* arg = (MtGemmArg*)arg_;
    const GemmParams* p = arg->p;
    int rows = arg->m_end - arg->m_start;
    sgemm_tiled(rows, p->K, p->N,
                p->alpha,
                p->A + arg->m_start * p->lda, p->lda,
                p->B, p->ldb,
                p->beta,
                p->C + arg->m_start * p->ldc, p->ldc);
}

void cpx_sgemm_mt(const GemmParams* p, struct CpxScheduler* sched) {
    if (!sched || p->num_threads <= 1) { cpx_sgemm(p); return; }

    int nt = p->num_threads;
    int chunk = (p->M + nt - 1) / nt;
    MtGemmArg args[64];
    for (int t = 0; t < nt; t++) {
        args[t].p       = p;
        args[t].m_start = t * chunk;
        args[t].m_end   = ((t+1) * chunk < p->M) ? (t+1)*chunk : p->M;
    }
    cpx_parallel_for(sched, mt_gemm_worker, args, sizeof(MtGemmArg), nt);
}

void cpx_sgemm_batched(const GemmParams* p, int batch_count,
                          struct CpxScheduler* sched) {
    size_t a_stride = (size_t)p->M * p->K;
    size_t b_stride = (size_t)p->K * p->N;
    size_t c_stride = (size_t)p->M * p->N;
    for (int b = 0; b < batch_count; b++) {
        GemmParams bp = *p;
        bp.A = p->A + b * a_stride;
        bp.B = p->B + b * b_stride;
        bp.C = p->C + b * c_stride;
        cpx_sgemm_mt(&bp, sched);
    }
}

/* ════════════════════════════════════════════════════════════════════
 * 6. FLASH ATTENTION
 *
 * Algorithm (per head):
 *   For each Q tile [Bq, d]:
 *     m_i = -inf, l_i = 0, O_i = 0     (running stats)
 *     For each KV tile [Bkv, d]:
 *       S = Q_tile × K_tile^T  [Bq, Bkv]   (GEMM)
 *       m_new = max(m_i, rowmax(S))
 *       P = exp(S - m_new)                  (stable softmax numerator)
 *       l_new = exp(m_i - m_new) * l_i + rowsum(P)
 *       O_i = (exp(m_i - m_new) * O_i + P × V_tile) / l_new
 *       m_i, l_i = m_new, l_new
 *     Write O_tile = O_i
 *   Working set: 2 × Bq×d + 2 × Bkv×d + Bq×Bkv floats
 *              ≈ 2×64×128 + 2×64×128 + 64×64 = 65536 + 4096 ≈ 69 KiB (L2)
 * ════════════════════════════════════════════════════════════════════ */

void CPX_HOT cpx_attention_flash(const AttentionParams* p,
                                   const float* Q,
                                   const float* K,
                                   const float* V,
                                   float* O) {
    int B       = p->batch;
    int H       = p->num_heads;
    int Sq      = p->seq_len_q;
    int Skv     = p->seq_len_kv;
    int D       = p->head_dim;
    float scale = p->scale;
    int Bq      = p->tile_q;
    int Bkv     = p->tile_kv;
    bool causal = p->causal;

    /* Temporary buffers allocated on the stack (small) or heap. */
    float* S   = (float*)malloc((size_t)Bq * Bkv * sizeof(float));
    float* m   = (float*)malloc((size_t)Bq * sizeof(float));
    float* l   = (float*)malloc((size_t)Bq * sizeof(float));
    float* m_n = (float*)malloc((size_t)Bq * sizeof(float));
    float* l_n = (float*)malloc((size_t)Bq * sizeof(float));
    float* O_i = (float*)malloc((size_t)Bq * D * sizeof(float));

    for (int b = 0; b < B; b++) {
        for (int h = 0; h < H; h++) {
            const float* Qh = Q + (b * H + h) * Sq  * D;
            const float* Kh = K + (b * H + h) * Skv * D;
            const float* Vh = V + (b * H + h) * Skv * D;
            float*       Oh = O + (b * H + h) * Sq  * D;

            for (int qi = 0; qi < Sq; qi += Bq) {
                int q_end = (qi + Bq < Sq) ? qi + Bq : Sq;
                int q_sz  = q_end - qi;

                /* Initialise running state for this Q tile. */
                for (int i = 0; i < q_sz; i++) {
                    m[i]   = -1e38f;
                    l[i]   = 0.f;
                }
                memset(O_i, 0, (size_t)q_sz * D * sizeof(float));

                for (int kvi = 0; kvi < Skv; kvi += Bkv) {
                    int kv_end = (kvi + Bkv < Skv) ? kvi + Bkv : Skv;
                    int kv_sz  = kv_end - kvi;

                    /* S = Q[qi:q_end] × K[kvi:kv_end]^T × scale */
                    GemmParams gp = {
                        .M = q_sz, .K = D, .N = kv_sz,
                        .lda = D, .ldb = D, .ldc = Bkv,
                        .alpha = scale, .beta = 0.f,
                        .A = Qh + qi  * D,
                        .B = Kh + kvi * D,
                        .C = S,
                        .num_threads = 1,
                    };
                    cpx_sgemm(&gp);

                    /* Optional causal mask: S[qi+i, kvi+j] = -inf if kvi+j > qi+i */
                    if (causal) {
                        for (int i = 0; i < q_sz; i++) {
                            for (int j = 0; j < kv_sz; j++) {
                                if (kvi + j > qi + i)
                                    S[i * Bkv + j] = -1e38f;
                            }
                        }
                    }

                    /* Online softmax update. */
                    for (int i = 0; i < q_sz; i++) {
                        /* New row max. */
                        float row_m = m[i];
                        for (int j = 0; j < kv_sz; j++) {
                            float v2 = S[i * Bkv + j];
                            if (v2 > row_m) row_m = v2;
                        }
                        m_n[i] = row_m;

                        /* Exponentiate S row. */
                        float row_sum = 0.f;
                        for (int j = 0; j < kv_sz; j++) {
                            float e = expf(S[i * Bkv + j] - row_m);
                            S[i * Bkv + j] = e;
                            row_sum += e;
                        }

                        float alpha2 = expf(m[i] - row_m);
                        l_n[i] = alpha2 * l[i] + row_sum;

                        /* Rescale O_i row: O_i[i] = alpha * O_i[i] */
                        for (int d = 0; d < D; d++) {
                            O_i[i * D + d] *= alpha2;
                        }
                    }

                    /* O_i += P × V[kvi:kv_end] */
                    {
                        GemmParams gv = {
                            .M = q_sz, .K = kv_sz, .N = D,
                            .lda = Bkv, .ldb = D, .ldc = D,
                            .alpha = 1.f, .beta = 1.f,
                            .A = S,
                            .B = Vh + kvi * D,
                            .C = O_i,
                            .num_threads = 1,
                        };
                        cpx_sgemm(&gv);
                    }

                    /* Update running state. */
                    for (int i = 0; i < q_sz; i++) {
                        m[i] = m_n[i];
                        l[i] = l_n[i];
                    }
                }

                /* Normalise and write output. */
                for (int i = 0; i < q_sz; i++) {
                    float inv_l = (l[i] > 0.f) ? 1.f / l[i] : 0.f;
                    for (int d = 0; d < D; d++) {
                        Oh[(qi + i) * D + d] = O_i[i * D + d] * inv_l;
                    }
                }
            }
        }
    }

    free(S); free(m); free(l); free(m_n); free(l_n); free(O_i);
}

/* Decode path: Q is single token [B, H, 1, D]. Pure GEMV. */
void CPX_HOT cpx_attention_decode(const AttentionParams* p,
                                    const float* Q,
                                    const float* K,
                                    const float* V,
                                    float* O) {
    int B       = p->batch;
    int H       = p->num_heads;
    int Skv     = p->seq_len_kv;
    int D       = p->head_dim;
    float scale = p->scale;

    float* scores = (float*)malloc((size_t)Skv * sizeof(float));
    float* probs  = (float*)malloc((size_t)Skv * sizeof(float));

    for (int b = 0; b < B; b++) {
        for (int h = 0; h < H; h++) {
            const float* q  = Q + (b * H + h) * D;
            const float* Kh = K + (b * H + h) * Skv * D;
            const float* Vh = V + (b * H + h) * Skv * D;
            float*       o  = O + (b * H + h) * D;

            /* scores = Q × K^T × scale  [Skv] */
            float max_s = -1e38f;
            for (int j = 0; j < Skv; j++) {
                float dot = 0.f;
                const float* kj = Kh + j * D;
                for (int d = 0; d < D; d++) dot += q[d] * kj[d];
                scores[j] = dot * scale;
                if (scores[j] > max_s) max_s = scores[j];
            }

            /* Softmax. */
            float sum = 0.f;
            for (int j = 0; j < Skv; j++) {
                probs[j] = expf(scores[j] - max_s);
                sum += probs[j];
            }
            float inv = (sum > 0.f) ? 1.f / sum : 0.f;
            for (int j = 0; j < Skv; j++) probs[j] *= inv;

            /* O = probs × V  [D] */
            memset(o, 0, D * sizeof(float));
            for (int j = 0; j < Skv; j++) {
                float w = probs[j];
                const float* vj = Vh + j * D;
                for (int d = 0; d < D; d++) o[d] += w * vj[d];
            }
        }
    }

    free(scores); free(probs);
}

/* ════════════════════════════════════════════════════════════════════
 * 7. LAYER NORM (Welford online mean/variance)
 * ════════════════════════════════════════════════════════════════════ */

void cpx_layernorm_fwd(const float* x, float* y,
                         const float* gamma, const float* beta,
                         float eps, int B, int D) {
    for (int b = 0; b < B; b++) {
        const float* xb = x + b * D;
        float*       yb = y + b * D;

        /* Welford mean + variance in one pass. */
        float mean = 0.f, M2 = 0.f;
        for (int d = 0; d < D; d++) {
            float delta = xb[d] - mean;
            mean += delta / (d + 1);
            M2   += delta * (xb[d] - mean);
        }
        float var  = M2 / D;
        float inv  = 1.f / sqrtf(var + eps);

        for (int d = 0; d < D; d++) {
            float x_hat = (xb[d] - mean) * inv;
            yb[d] = (gamma ? gamma[d] * x_hat : x_hat)
                  + (beta  ? beta[d]           : 0.f);
        }
    }
}

void cpx_layernorm_bwd(const float* dy, const float* x,
                         const float* gamma, float eps,
                         float* dx, float* dgamma, float* dbeta,
                         int B, int D) {
    for (int b = 0; b < B; b++) {
        const float* xb  = x  + b * D;
        const float* dyb = dy + b * D;
        float*       dxb = dx + b * D;

        /* Recompute mean and inv_std. */
        float mean = 0.f;
        for (int d = 0; d < D; d++) mean += xb[d];
        mean /= D;
        float var = 0.f;
        for (int d = 0; d < D; d++) { float t = xb[d]-mean; var += t*t; }
        float inv  = 1.f / sqrtf(var / D + eps);
        float inv3 = inv * inv * inv;

        /* Gradient through normalisation. */
        float dy_sum = 0.f, dy_xhat_sum = 0.f;
        for (int d = 0; d < D; d++) {
            float xhat  = (xb[d] - mean) * inv;
            float g     = gamma ? gamma[d] : 1.f;
            float dy_g  = dyb[d] * g;
            dy_sum      += dy_g;
            dy_xhat_sum += dy_g * xhat;
            if (dgamma) dgamma[d] += dyb[d] * xhat;
            if (dbeta)  dbeta[d]  += dyb[d];
        }
        float inv_D = 1.f / D;
        for (int d = 0; d < D; d++) {
            float xhat  = (xb[d] - mean) * inv;
            float g     = gamma ? gamma[d] : 1.f;
            dxb[d] = inv * (dyb[d] * g
                       - inv_D * dy_sum
                       - inv_D * xhat * dy_xhat_sum);
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
 * 8. RMS NORM
 * ════════════════════════════════════════════════════════════════════ */

void cpx_rmsnorm_fwd(const float* x, float* y, const float* gamma,
                       float eps, int B, int D) {
    for (int b = 0; b < B; b++) {
        const float* xb = x + b * D;
        float*       yb = y + b * D;

        float ss = 0.f;
        for (int d = 0; d < D; d++) ss += xb[d] * xb[d];
        float inv = 1.f / sqrtf(ss / D + eps);
        for (int d = 0; d < D; d++) {
            yb[d] = xb[d] * inv * (gamma ? gamma[d] : 1.f);
        }
    }
}

void cpx_rmsnorm_bwd(const float* dy, const float* x,
                       const float* gamma, float eps,
                       float* dx, float* dgamma,
                       int B, int D) {
    for (int b = 0; b < B; b++) {
        const float* xb  = x  + b * D;
        const float* dyb = dy + b * D;
        float*       dxb = dx + b * D;

        float ss = 0.f;
        for (int d = 0; d < D; d++) ss += xb[d] * xb[d];
        float rms  = sqrtf(ss / D + eps);
        float inv  = 1.f / rms;
        float inv3 = inv / (ss / D + eps);

        float dot_dy_x = 0.f;
        for (int d = 0; d < D; d++) {
            float g = gamma ? gamma[d] : 1.f;
            dot_dy_x += dyb[d] * g * xb[d];
            if (dgamma) dgamma[d] += dyb[d] * xb[d] * inv;
        }
        float scale = dot_dy_x * inv3 / D;
        for (int d = 0; d < D; d++) {
            float g = gamma ? gamma[d] : 1.f;
            dxb[d] = inv * dyb[d] * g - scale * xb[d];
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
 * 9. SOFTMAX
 * ════════════════════════════════════════════════════════════════════ */

void cpx_softmax_fwd(const float* x, float* y, int B, int D) {
    for (int b = 0; b < B; b++) {
        const float* xb = x + b * D;
        float*       yb = y + b * D;

        float m = xb[0];
        for (int d = 1; d < D; d++) if (xb[d] > m) m = xb[d];
        float s = 0.f;
        for (int d = 0; d < D; d++) { yb[d] = expf(xb[d] - m); s += yb[d]; }
        float inv = 1.f / s;
        for (int d = 0; d < D; d++) yb[d] *= inv;
    }
}

void cpx_softmax_causal(const float* x, float* y, int T, int D) {
    /* T tokens, causal: token i can attend to 1..i+1 positions. */
    for (int t = 0; t < T; t++) {
        int len = t + 1;
        cpx_softmax_fwd(x + t * D, y + t * D, 1, len);
        memset(y + t * D + len, 0, (D - len) * sizeof(float));
    }
}

void cpx_softmax_bwd(const float* y, const float* dy, float* dx,
                       int B, int D) {
    /* dx = y * (dy - dot(dy, y)) — Jacobian-vector product. */
    for (int b = 0; b < B; b++) {
        const float* yb  = y  + b * D;
        const float* dyb = dy + b * D;
        float*       dxb = dx + b * D;
        float dot = 0.f;
        for (int d = 0; d < D; d++) dot += dyb[d] * yb[d];
        for (int d = 0; d < D; d++) dxb[d] = yb[d] * (dyb[d] - dot);
    }
}

/* ════════════════════════════════════════════════════════════════════
 * 10. ACTIVATIONS
 * ════════════════════════════════════════════════════════════════════ */

#ifndef M_SQRT1_2
#  define M_SQRT1_2 0.7071067811865476f
#endif

void cpx_relu_fwd(const float* x, float* y, int n) {
    for (int i = 0; i < n; i++) y[i] = x[i] > 0.f ? x[i] : 0.f;
}
void cpx_relu_bwd(const float* x, const float* dy, float* dx, int n) {
    for (int i = 0; i < n; i++) dx[i] = x[i] > 0.f ? dy[i] : 0.f;
}

void cpx_gelu_fwd(const float* x, float* y, int n) {
    /* Exact GELU: 0.5 * x * (1 + erf(x / sqrt(2))) */
    for (int i = 0; i < n; i++) {
        y[i] = 0.5f * x[i] * (1.f + erff(x[i] * (float)M_SQRT1_2));
    }
}
void cpx_gelu_bwd(const float* x, const float* dy, float* dx, int n) {
    static const float INV_SQRT2    = (float)M_SQRT1_2;
    static const float INV_SQRT2PI  = 0.3989422804014327f;
    for (int i = 0; i < n; i++) {
        float cdf   = 0.5f * (1.f + erff(x[i] * INV_SQRT2));
        float pdf   = INV_SQRT2PI * expf(-0.5f * x[i] * x[i]);
        dx[i] = dy[i] * (cdf + x[i] * pdf);
    }
}

void cpx_gelu_approx_fwd(const float* x, float* y, int n) {
    /* Tanh approximation: 0.5x(1 + tanh(sqrt(2/pi)(x + 0.044715x^3))) */
    static const float c0 = 0.7978845608028654f; /* sqrt(2/pi) */
    static const float c1 = 0.044715f;
    for (int i = 0; i < n; i++) {
        float v = x[i];
        float inner = c0 * (v + c1 * v * v * v);
        y[i] = 0.5f * v * (1.f + tanhf(inner));
    }
}
void cpx_gelu_approx_bwd(const float* x, const float* dy,
                            float* dx, int n) {
    static const float c0 = 0.7978845608028654f;
    static const float c1 = 0.044715f;
    for (int i = 0; i < n; i++) {
        float v     = x[i];
        float inner = c0 * (v + c1 * v * v * v);
        float th    = tanhf(inner);
        float sech2 = 1.f - th * th;
        float dtanh = c0 * (1.f + 3.f * c1 * v * v) * sech2;
        dx[i] = dy[i] * (0.5f * (1.f + th) + 0.5f * v * dtanh);
    }
}

void cpx_silu_fwd(const float* x, float* y, int n) {
    /* SiLU(x) = x * sigmoid(x) */
    for (int i = 0; i < n; i++) {
        float s = 1.f / (1.f + expf(-x[i]));
        y[i] = x[i] * s;
    }
}
void cpx_silu_bwd(const float* x, const float* dy, float* dx, int n) {
    for (int i = 0; i < n; i++) {
        float s  = 1.f / (1.f + expf(-x[i]));
        float su = x[i] * s;           /* silu(x) */
        dx[i] = dy[i] * (s + su * (1.f - s));
    }
}

/* ════════════════════════════════════════════════════════════════════
 * 11. ELEMENTWISE VECTORISED
 * ════════════════════════════════════════════════════════════════════ */

void cpx_vadd(const float* a, const float* b, float* c, int n) {
    int i = 0;
#if HAVE_AVX2
    for (; i <= n - 8; i += 8)
        _mm256_storeu_ps(c+i, _mm256_add_ps(_mm256_loadu_ps(a+i),
                                            _mm256_loadu_ps(b+i)));
#endif
    for (; i < n; i++) c[i] = a[i] + b[i];
}

void cpx_vaxpby(float alpha2, const float* x,
                 float beta2,  const float* y, float* z, int n) {
    int i = 0;
#if HAVE_AVX2
    __m256 va = _mm256_set1_ps(alpha2);
    __m256 vb = _mm256_set1_ps(beta2);
    for (; i <= n - 8; i += 8) {
        __m256 r = _mm256_fmadd_ps(va, _mm256_loadu_ps(x+i),
                     _mm256_mul_ps(vb, _mm256_loadu_ps(y+i)));
        _mm256_storeu_ps(z+i, r);
    }
#endif
    for (; i < n; i++) z[i] = alpha2 * x[i] + beta2 * y[i];
}

void cpx_vfma(float a, const float* x, float* y, int n) {
    int i = 0;
#if HAVE_AVX2
    __m256 va = _mm256_set1_ps(a);
    for (; i <= n - 8; i += 8) {
        __m256 r = _mm256_fmadd_ps(va, _mm256_loadu_ps(x+i),
                                       _mm256_loadu_ps(y+i));
        _mm256_storeu_ps(y+i, r);
    }
#endif
    for (; i < n; i++) y[i] = a * x[i] + y[i];
}

float cpx_vsum(const float* x, int n) {
    float s = 0.f;
    int i = 0;
#if HAVE_AVX2
    __m256 acc = _mm256_setzero_ps();
    for (; i <= n - 8; i += 8)
        acc = _mm256_add_ps(acc, _mm256_loadu_ps(x+i));
    float buf[8];
    _mm256_storeu_ps(buf, acc);
    for (int j = 0; j < 8; j++) s += buf[j];
#endif
    for (; i < n; i++) s += x[i];
    return s;
}

float cpx_vmean(const float* x, int n) {
    return n > 0 ? cpx_vsum(x, n) / n : 0.f;
}

float cpx_vmax(const float* x, int n) {
    if (n == 0) return -1e38f;
    float m = x[0];
    for (int i = 1; i < n; i++) if (x[i] > m) m = x[i];
    return m;
}
