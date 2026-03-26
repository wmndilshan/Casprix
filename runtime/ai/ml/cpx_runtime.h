/*
 * Casprix ML Runtime — High-Performance CPU Execution Engine
 *
 * Architecture:
 *   cpx_hpc_kernel.h   — SIMD abstraction, micro-kernels (GEMM, attn, norms)
 *   cpx_tensor_engine.h — tensor IR, operator fusion, tiled execution
 *   cpx_mem_arena.h    — multi-tier arena (params / activations / temp)
 *   cpx_scheduler.h    — work-stealing task scheduler, thread pinning
 *   cpx_quant.h        — INT8/INT4 quantization + mixed-precision
 *   cpx_kvcache.h      — KV-cache for autoregressive inference
 *   cpx_runtime.h      — top-level runtime (this file)
 *
 * Design constraints:
 *   - C11, no GPU, no external BLAS — all kernels are hand-written
 *   - AVX2+FMA baseline; AVX-512 detected at runtime and dispatched
 *   - All hot-path allocations are arena-based (zero malloc in training loop)
 *   - Thread count = physical core count with NUMA-aware placement
 *   - Everything 64-byte (cache-line) aligned
 */

#ifndef CPX_RUNTIME_H
#define CPX_RUNTIME_H

#include "cpx_hpc_kernel.h"
#include "cpx_tensor_engine.h"
#include "cpx_mem_arena.h"
#include "cpx_scheduler.h"
#include "cpx_quant.h"
#include "cpx_kvcache.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════════
 * Runtime configuration
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    int     num_threads;        /* 0 = auto-detect physical cores     */
    bool    pin_threads;        /* NUMA-aware thread pinning           */
    bool    avx512;             /* override: force enable/disable      */
    bool    avx2;               /* override: force enable/disable      */
    size_t  param_pool_mb;      /* parameter arena size in MiB         */
    size_t  act_pool_mb;        /* activation arena per thread in MiB  */
    size_t  grad_pool_mb;       /* gradient arena per thread in MiB    */
    size_t  temp_pool_mb;       /* per-op scratch pool in MiB          */
    bool    enable_fusion;      /* operator fusion pass                */
    bool    enable_profiling;   /* perf counters on ops                */
    int     gemm_tile_m;        /* 0 = auto-tune                       */
    int     gemm_tile_n;
    int     gemm_tile_k;
} CpxRuntimeConfig;

#define CPX_RUNTIME_CONFIG_DEFAULT { \
    .num_threads    = 0,            \
    .pin_threads    = true,         \
    .avx512         = false,        \
    .avx2           = true,         \
    .param_pool_mb  = 4096,         \
    .act_pool_mb    = 512,          \
    .grad_pool_mb   = 512,          \
    .temp_pool_mb   = 256,          \
    .enable_fusion  = true,         \
    .enable_profiling = false,      \
    .gemm_tile_m    = 0,            \
    .gemm_tile_n    = 0,            \
    .gemm_tile_k    = 0,            \
}

/* Runtime instance (one per process). */
typedef struct CpxRuntime CpxRuntime;

CpxRuntime* cpx_runtime_create(const CpxRuntimeConfig* cfg);
void        cpx_runtime_destroy(CpxRuntime* rt);

/* Query detected CPU capabilities. */
const CpxCpuInfo* cpx_runtime_cpu_info(CpxRuntime* rt);

/* Access sub-systems. */
CpxScheduler*   cpx_runtime_scheduler(CpxRuntime* rt);
CpxMemArena*    cpx_runtime_param_arena(CpxRuntime* rt);
CpxTensorEngine* cpx_runtime_engine(CpxRuntime* rt);

#ifdef __cplusplus
}
#endif

#endif /* CPX_RUNTIME_H */
