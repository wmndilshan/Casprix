/*
 * Casprix ML Runtime — Tensor Execution Engine
 *
 * ════════════════════════════════════════════════════════════════════
 * ARCHITECTURE
 * ════════════════════════════════════════════════════════════════════
 *
 * The tensor engine sits between the high-level model API and the
 * low-level HPC kernels.  Its jobs are:
 *
 *   1. GRAPH CONSTRUCTION
 *      OpNode DAG with typed inputs/outputs (CpxTensorView).
 *      Each node carries a CpxFusionFlags bitfield that the fusion
 *      pass fills in.
 *
 *   2. OPERATOR FUSION (single-pass, rule-based)
 *      Fusion rules fire during graph build or as a post-pass.
 *      Fused groups get a single composite kernel_fn pointer.
 *      Examples:
 *        GEMM + BIAS + ACT  → cpx_gemm_bias_act_fused
 *        LAYERNORM + SCALE  → cpx_layernorm_scale_fused
 *        ATTENTION + SOFTMAX + V_PROJ → cpx_attention_fused
 *        RESIDUAL + NORM    → cpx_residual_norm_fused
 *
 *   3. TILED EXECUTION PLANNER
 *      For each fused op, computes the blocking factors
 *      (M_tile, N_tile, K_tile) that maximise register/L1/L2 reuse.
 *      Cost model: roofline — min(FLOP/peak_flops, bytes/peak_bw).
 *      Writes a CpxExecPlan per OpNode.
 *
 *   4. DISPATCH
 *      Walk topological order of CpxExecPlan list.
 *      Parallel ops dispatched via CpxScheduler (cpx_parallel_for).
 *      Sequential bottlenecks (e.g. vocab softmax) run on thread-0.
 *
 *   5. JIT KERNEL SELECTION
 *      Kernel function pointer chosen at runtime based on CpxCpuInfo.
 *      AVX-512 path selected if avx512f + fma available.
 *      Fallback: AVX2, then scalar.
 *
 * ════════════════════════════════════════════════════════════════════
 * FUSION GRAPH EXAMPLE — TRANSFORMER FFN BLOCK
 * ════════════════════════════════════════════════════════════════════
 *
 *   Input x [B, D]
 *     ├─► LAYERNORM [B, D]
 *     │     └─► GEMM W1 [B, D×4] ──► GELU ──► GEMM W2 [B, D]
 *     └─► ADD residual ──► Output [B, D]
 *
 *  After fusion:
 *    Node A: LAYERNORM (standalone)
 *    Node B: GEMM_GELU_FUSED  (W1 + GELU in one kernel)
 *    Node C: GEMM_BIAS_FUSED  (W2 + bias in one kernel)
 *    Node D: RESIDUAL_ADD     (vectorised, ~1 cycle/elem)
 *
 * ════════════════════════════════════════════════════════════════════
 * TILING STRATEGY
 * ════════════════════════════════════════════════════════════════════
 *
 * Given L1 = 32 KiB, L2 = 256 KiB, L3 = 8 MiB, sizeof(f32) = 4:
 *   KC = 256  → A-panel [MC,KC] = 288×256×4 = 288 KiB (L2)
 *   MC = 288  → B-panel [KC,NC] = 256×512×4 = 512 KiB (L2)
 *   NC = 512
 *
 * Register tile (AVX2):
 *   MR=6 rows of A, NR=16 cols of B (2× ymm), C: 6×2 ymm = 12 regs
 *   Remaining 4 ymm for A broadcast + B loads.
 *
 * Register tile (AVX-512):
 *   MR=8, NR=32 (2× zmm), C: 8×2 zmm = 16 regs, 1 zmm A, 2 zmm B
 *
 * ════════════════════════════════════════════════════════════════════
 * COST MODEL
 * ════════════════════════════════════════════════════════════════════
 *
 * For each op, compute:
 *   flops    = 2 × M × K × N  (GEMM), M×N×(head_dim) for attention
 *   bytes    = sizeof inputs + sizeof outputs (streaming, not reused)
 *   arith_intensity = flops / bytes  (FLOP/byte)
 *
 * Roofline:
 *   t_compute = flops / peak_gflops
 *   t_memory  = bytes / peak_gbps
 *   t_bound   = max(t_compute, t_memory)
 *
 * For GEMM:  arith_intensity grows with M,K,N.  Large GEMM is
 *            compute-bound.  GEMV (B=1) is memory-bound.
 * Decision:  choose tile sizes to exceed ridge point (where
 *            arith_intensity = peak_gflops / peak_gbps).
 *
 * Typical x86: peak = 512 GFLOPS (AVX-512), bw = 50 GB/s
 *   Ridge point ≈ 512/50 ≈ 10 FLOP/byte
 *   GEMM with K,N≥256 is above ridge → compute-bound ✓
 */

#ifndef CPX_TENSOR_ENGINE_H
#define CPX_TENSOR_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "cpx_mem_arena.h"
#include "cpx_hpc_kernel.h"
#include "cpx_scheduler.h"
#include "cpx_quant.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════════
 * 1. OPERATION TYPES
 * ════════════════════════════════════════════════════════════════════ */

typedef enum {
    OP_NONE = 0,

    /* ── Linear algebra ── */
    OP_GEMM,
    OP_GEMV,
    OP_BATCHED_MATMUL,

    /* ── Attention ── */
    OP_ATTENTION,          /* full MHA / GQA                         */
    OP_ATTENTION_DECODE,   /* single-token decode (GEMV path)        */

    /* ── Normalization ── */
    OP_LAYERNORM,
    OP_RMSNORM,

    /* ── Activations ── */
    OP_RELU,
    OP_GELU,
    OP_GELU_APPROX,
    OP_SILU,
    OP_SOFTMAX,

    /* ── Elementwise ── */
    OP_ADD,
    OP_MUL,
    OP_SCALE,
    OP_AXPBY,

    /* ── Embedding ── */
    OP_EMBEDDING_LOOKUP,
    OP_EMBEDDING_UPDATE,   /* gradient scatter-add                   */

    /* ── Reduction ── */
    OP_REDUCE_SUM,
    OP_REDUCE_MEAN,
    OP_REDUCE_MAX,

    /* ── Loss ── */
    OP_CROSS_ENTROPY,
    OP_MSE,

    /* ── Memory ── */
    OP_RESHAPE,
    OP_TRANSPOSE,
    OP_SLICE,
    OP_CONCAT,
    OP_PAD,

    /* ── Fused composites (assigned by fusion pass) ── */
    OP_FUSED_GEMM_BIAS,
    OP_FUSED_GEMM_BIAS_RELU,
    OP_FUSED_GEMM_BIAS_GELU,
    OP_FUSED_GEMM_BIAS_SILU,
    OP_FUSED_LAYERNORM_SCALE,
    OP_FUSED_RMSNORM_SCALE,
    OP_FUSED_ATTENTION,          /* QKV matmuls + softmax + output   */
    OP_FUSED_RESIDUAL_NORM,      /* residual add + rmsnorm           */
    OP_FUSED_CROSS_ENTROPY_SOFTMAX,
    OP_FUSED_EMBEDDING_LAYERNORM,

    OP_COUNT,
} CpxOpType;

/* ════════════════════════════════════════════════════════════════════
 * 2. FUSION FLAGS
 * ════════════════════════════════════════════════════════════════════ */

typedef uint32_t CpxFusionFlags;

#define FUSION_NONE          0u
#define FUSION_WITH_BIAS    (1u << 0)
#define FUSION_WITH_RELU    (1u << 1)
#define FUSION_WITH_GELU    (1u << 2)
#define FUSION_WITH_SILU    (1u << 3)
#define FUSION_WITH_SCALE   (1u << 4)   /* post-norm scale/shift     */
#define FUSION_WITH_RESIDUAL (1u << 5)
#define FUSION_CAUSAL_MASK  (1u << 6)   /* attention causal mask     */
#define FUSION_INPLACE      (1u << 7)   /* output reuses input buffer*/
#define FUSION_QUANTIZED    (1u << 8)   /* inputs/weights in INT8/4  */

/* ════════════════════════════════════════════════════════════════════
 * 3. OP NODE
 * ════════════════════════════════════════════════════════════════════ */

#define CPX_OP_MAX_INPUTS   8
#define CPX_OP_MAX_OUTPUTS  4
#define CPX_OP_MAX_ATTRS    8

/*
 * Kernel function signature for fused ops.
 * The kernel receives a typed attribute array (int64_t) for shapes,
 * and TensorView pointers for inputs/outputs.
 */
typedef void (*CpxKernelFn)(
    const CpxTensorView* inputs,  int num_inputs,
    CpxTensorView*       outputs, int num_outputs,
    const int64_t*       attrs,   int num_attrs,
    struct CpxScheduler* sched
);

typedef struct CpxOpNode {
    int              id;
    CpxOpType        op;
    CpxFusionFlags   fusion;

    /* Input and output tensor views. */
    CpxTensorView    inputs[CPX_OP_MAX_INPUTS];
    int              num_inputs;
    CpxTensorView    outputs[CPX_OP_MAX_OUTPUTS];
    int              num_outputs;

    /* Integer attributes (shapes, strides, scalars as bits). */
    int64_t          attrs[CPX_OP_MAX_ATTRS];
    int              num_attrs;

    /* Quantization parameters (NULL = not quantised). */
    QuantParams*     quant;

    /* Kernel function pointer (set by fusion pass + JIT selection). */
    CpxKernelFn      kernel_fn;

    /* Graph topology. */
    int              predecessors[CPX_OP_MAX_INPUTS];
    int              num_pred;
    int              successors[CPX_OP_MAX_OUTPUTS];
    int              num_succ;

    /* Scheduling metadata (set by planner). */
    int              priority;      /* higher = earlier in schedule   */
    bool             is_critical_path;
} CpxOpNode;

/* ════════════════════════════════════════════════════════════════════
 * 4. OP GRAPH
 * ════════════════════════════════════════════════════════════════════ */

#define CPX_OPGRAPH_MAX_NODES  1024

typedef struct {
    CpxOpNode  nodes[CPX_OPGRAPH_MAX_NODES];
    int        num_nodes;
    int        topo_order[CPX_OPGRAPH_MAX_NODES];  /* topological sort */
    int        topo_len;
    bool       is_sorted;
} CpxOpGraph;

void cpx_opgraph_init(CpxOpGraph* g);
void cpx_opgraph_reset(CpxOpGraph* g);

/* Add an op node; returns its id. */
int cpx_opgraph_add_op(CpxOpGraph* g, CpxOpType op,
                         const CpxTensorView* inputs, int num_in,
                         CpxTensorView* outputs, int num_out);

/* Add dependency: op `from` must finish before op `to`. */
void cpx_opgraph_depend(CpxOpGraph* g, int from, int to);

/* Compute topological order (Kahn's algorithm). Returns false if cycle. */
bool cpx_opgraph_toposort(CpxOpGraph* g);

/* ════════════════════════════════════════════════════════════════════
 * 5. FUSION PASS
 * ════════════════════════════════════════════════════════════════════ */

/*
 * Rule-based fusion.  Traverses the graph in topological order,
 * merges eligible adjacent ops into a fused composite node.
 *
 * Fusion eligibility:
 *   - Merged op output has exactly one consumer.
 *   - Combined working set fits in L1/L2.
 *   - No side-effects requiring intermediate materialisation.
 *
 * Returns number of fusions applied.
 */
int cpx_fusion_pass(CpxOpGraph* g, const CpxCpuInfo* cpu);

/* Individual fusion rules — exposed for testing / manual use. */
bool cpx_try_fuse_gemm_bias_act(CpxOpGraph* g, int gemm_id);
bool cpx_try_fuse_norm_scale(CpxOpGraph* g, int norm_id);
bool cpx_try_fuse_attention(CpxOpGraph* g, int qkv_gemm_id);
bool cpx_try_fuse_residual_norm(CpxOpGraph* g, int add_id);

/* ════════════════════════════════════════════════════════════════════
 * 6. EXECUTION PLAN & TILED PLANNER
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    int op_id;

    /* GEMM tile factors. */
    int mc, nc, kc;  /* L2 cache tiles */
    int mr, nr;      /* register tile  */

    /* Parallelism. */
    int num_threads;         /* target thread count for this op      */
    int parallel_dim;        /* which output dimension to split (0,1)*/
    int chunk_size;          /* rows/cols per thread chunk           */

    /* Memory estimates. */
    size_t working_set_bytes;
    float  arith_intensity;  /* FLOP/byte                            */
    float  predicted_ms;
} CpxExecPlan;

typedef struct {
    CpxExecPlan  plans[CPX_OPGRAPH_MAX_NODES];
    int          num_plans;
    float        total_predicted_ms;
} CpxSchedulePlan;

/* Compute execution plan for all ops in topological order.
 * cpu: used to select tile sizes and check AVX-512 availability.
 * num_threads: available worker threads. */
void cpx_planner_run(const CpxOpGraph* g, const CpxCpuInfo* cpu,
                       int num_threads, CpxSchedulePlan* out);

/* ════════════════════════════════════════════════════════════════════
 * 7. JIT KERNEL DISPATCH TABLE
 *
 * Each op type has up to 4 implementations selected by CpxCpuInfo:
 *   [0] AVX-512  [1] AVX2  [2] SSE4  [3] scalar
 * cpx_kernel_table_init fills in the function pointers at startup.
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    CpxKernelFn impls[4];   /* [avx512, avx2, sse4, scalar]          */
    int         best_impl;  /* index selected by cpx_jit_select      */
} CpxKernelEntry;

typedef struct {
    CpxKernelEntry entries[OP_COUNT];
} CpxKernelTable;

void cpx_kernel_table_init(CpxKernelTable* tbl, const CpxCpuInfo* cpu);

/* Select and cache the best kernel implementation for an op. */
CpxKernelFn cpx_jit_select(CpxKernelTable* tbl, CpxOpType op,
                              const CpxCpuInfo* cpu);

/* ════════════════════════════════════════════════════════════════════
 * 8. TENSOR ENGINE (top-level executor)
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    CpxKernelTable   kernel_tbl;
    CpxCpuInfo       cpu;
    CpxScheduler*    sched;
    CpxMemArena*     mem;
    bool             enable_profiling;
} CpxTensorEngine;

void cpx_tensor_engine_init(CpxTensorEngine* eng,
                               CpxScheduler* sched,
                               CpxMemArena* mem,
                               const CpxCpuInfo* cpu,
                               bool enable_profiling);

void cpx_tensor_engine_destroy(CpxTensorEngine* eng);

/*
 * Execute an entire operator graph.
 * Applies fusion pass, runs planner, dispatches in topological order.
 */
void cpx_tensor_engine_run(CpxTensorEngine* eng, CpxOpGraph* g);

/*
 * Execute a single op node (used for debugging / unit tests).
 */
void cpx_tensor_engine_run_op(CpxTensorEngine* eng, CpxOpNode* node);

/* ════════════════════════════════════════════════════════════════════
 * 9. PROFILING
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    CpxOpType  op;
    int        op_id;
    double     elapsed_ms;
    uint64_t   flops;
    uint64_t   bytes;
    float      gflops_per_sec;
    float      bandwidth_gb_per_sec;
    float      efficiency_pct;   /* % of roofline peak               */
} CpxOpProfile;

typedef struct {
    CpxOpProfile  ops[CPX_OPGRAPH_MAX_NODES];
    int           num_ops;
    double        total_ms;
    double        gemm_ms;
    double        attention_ms;
    double        norm_ms;
    double        other_ms;
} CpxEngineProfile;

void cpx_engine_profile_print(const CpxEngineProfile* p);
void cpx_engine_profile_reset(CpxEngineProfile* p);

/* ════════════════════════════════════════════════════════════════════
 * 10. COST MODEL UTILITIES
 * ════════════════════════════════════════════════════════════════════ */

/* Estimate FLOPs for a given op node. */
uint64_t cpx_cost_flops(const CpxOpNode* node);

/* Estimate bytes accessed for a given op node. */
uint64_t cpx_cost_bytes(const CpxOpNode* node);

/* Predict execution time in ms given CPU info (roofline model). */
float cpx_cost_predict_ms(const CpxOpNode* node, const CpxCpuInfo* cpu,
                             int num_threads);

#ifdef __cplusplus
}
#endif

#endif /* CPX_TENSOR_ENGINE_H */
