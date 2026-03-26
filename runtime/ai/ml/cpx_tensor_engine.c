/*
 * Casprix ML Runtime — Tensor Engine Implementation
 */

#include "cpx_tensor_engine.h"
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#if defined(_WIN32)
#  include <windows.h>
static double now_ms(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
#else
#  include <time.h>
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif

/* ════════════════════════════════════════════════════════════════════
 * 1. OP GRAPH
 * ════════════════════════════════════════════════════════════════════ */

void cpx_opgraph_init(CpxOpGraph* g) {
    g->num_nodes = 0;
    g->topo_len  = 0;
    g->is_sorted = false;
}

void cpx_opgraph_reset(CpxOpGraph* g) {
    cpx_opgraph_init(g);
}

int cpx_opgraph_add_op(CpxOpGraph* g, CpxOpType op,
                         const CpxTensorView* inputs, int num_in,
                         CpxTensorView* outputs, int num_out) {
    assert(g->num_nodes < CPX_OPGRAPH_MAX_NODES);
    int id = g->num_nodes++;
    CpxOpNode* n = &g->nodes[id];
    memset(n, 0, sizeof(*n));
    n->id       = id;
    n->op       = op;
    n->num_inputs  = num_in;
    n->num_outputs = num_out;
    for (int i = 0; i < num_in;  i++) n->inputs[i]  = inputs[i];
    for (int i = 0; i < num_out; i++) n->outputs[i] = outputs[i];
    g->is_sorted = false;
    return id;
}

void cpx_opgraph_depend(CpxOpGraph* g, int from, int to) {
    CpxOpNode* f = &g->nodes[from];
    CpxOpNode* t = &g->nodes[to];
    assert(f->num_succ < CPX_OP_MAX_OUTPUTS);
    assert(t->num_pred < CPX_OP_MAX_INPUTS);
    f->successors[f->num_succ++]   = to;
    t->predecessors[t->num_pred++] = from;
    g->is_sorted = false;
}

/* Kahn's topological sort. */
bool cpx_opgraph_toposort(CpxOpGraph* g) {
    int in_deg[CPX_OPGRAPH_MAX_NODES] = {0};
    int queue[CPX_OPGRAPH_MAX_NODES];
    int head = 0, tail = 0;

    for (int i = 0; i < g->num_nodes; i++)
        in_deg[i] = g->nodes[i].num_pred;
    for (int i = 0; i < g->num_nodes; i++)
        if (in_deg[i] == 0) queue[tail++] = i;

    g->topo_len = 0;
    while (head < tail) {
        int u = queue[head++];
        g->topo_order[g->topo_len++] = u;
        for (int j = 0; j < g->nodes[u].num_succ; j++) {
            int v = g->nodes[u].successors[j];
            if (--in_deg[v] == 0) queue[tail++] = v;
        }
    }
    g->is_sorted = (g->topo_len == g->num_nodes);
    return g->is_sorted;
}

/* ════════════════════════════════════════════════════════════════════
 * 2. FUSION PASS
 * ════════════════════════════════════════════════════════════════════ */

/* Check if an op has exactly one consumer in the graph. */
static bool single_consumer(const CpxOpGraph* g, int op_id) {
    return g->nodes[op_id].num_succ == 1;
}

/* Check successor op type. */
static CpxOpType succ_type(const CpxOpGraph* g, int op_id) {
    if (g->nodes[op_id].num_succ == 0) return OP_NONE;
    return g->nodes[g->nodes[op_id].successors[0]].op;
}

/* Merge node `child` into node `parent`, set parent op to `fused`. */
static void merge_nodes(CpxOpGraph* g, int parent, int child,
                        CpxOpType fused, CpxFusionFlags flags) {
    CpxOpNode* p = &g->nodes[parent];
    CpxOpNode* c = &g->nodes[child];

    p->op       = fused;
    p->fusion  |= flags;

    /* Inherit outputs from child. */
    p->num_outputs = c->num_outputs;
    for (int i = 0; i < c->num_outputs; i++) p->outputs[i] = c->outputs[i];

    /* Re-wire successors: parent inherits child's successors. */
    p->num_succ = c->num_succ;
    for (int j = 0; j < c->num_succ; j++) {
        int s = c->successors[j];
        p->successors[j] = s;
        /* Fix predecessor pointer in s. */
        for (int k = 0; k < g->nodes[s].num_pred; k++) {
            if (g->nodes[s].predecessors[k] == child)
                g->nodes[s].predecessors[k] = parent;
        }
    }

    /* Remove child by marking it OP_NONE (no-op). */
    c->op      = OP_NONE;
    c->num_pred = 0;
    c->num_succ = 0;

    g->is_sorted = false;
}

bool cpx_try_fuse_gemm_bias_act(CpxOpGraph* g, int gemm_id) {
    if (g->nodes[gemm_id].op != OP_GEMM) return false;
    if (!single_consumer(g, gemm_id))    return false;

    int succ1 = g->nodes[gemm_id].successors[0];
    CpxOpType t1 = g->nodes[succ1].op;

    /* GEMM + BIAS. */
    if (t1 == OP_ADD) {
        g->nodes[gemm_id].fusion |= FUSION_WITH_BIAS;
        /* Carry the bias tensor as extra input. */
        int ni = g->nodes[gemm_id].num_inputs;
        g->nodes[gemm_id].inputs[ni] = g->nodes[succ1].inputs[1];
        g->nodes[gemm_id].num_inputs = ni + 1;

        if (single_consumer(g, succ1)) {
            int succ2  = g->nodes[succ1].successors[0];
            CpxOpType t2 = g->nodes[succ2].op;
            CpxFusionFlags act_flag = 0;
            CpxOpType fused = OP_FUSED_GEMM_BIAS;
            if      (t2 == OP_RELU)       { act_flag = FUSION_WITH_RELU; fused = OP_FUSED_GEMM_BIAS_RELU; }
            else if (t2 == OP_GELU)       { act_flag = FUSION_WITH_GELU; fused = OP_FUSED_GEMM_BIAS_GELU; }
            else if (t2 == OP_GELU_APPROX){ act_flag = FUSION_WITH_GELU; fused = OP_FUSED_GEMM_BIAS_GELU; }
            else if (t2 == OP_SILU)       { act_flag = FUSION_WITH_SILU; fused = OP_FUSED_GEMM_BIAS_SILU; }

            if (act_flag) {
                merge_nodes(g, succ1, succ2, t2, act_flag);
            }
        }
        merge_nodes(g, gemm_id, succ1, OP_FUSED_GEMM_BIAS, FUSION_WITH_BIAS);
        return true;
    }
    return false;
}

bool cpx_try_fuse_norm_scale(CpxOpGraph* g, int norm_id) {
    CpxOpType nt = g->nodes[norm_id].op;
    if (nt != OP_LAYERNORM && nt != OP_RMSNORM) return false;
    if (!single_consumer(g, norm_id)) return false;
    int succ1 = g->nodes[norm_id].successors[0];
    if (g->nodes[succ1].op != OP_MUL) return false;
    merge_nodes(g, norm_id, succ1,
                nt == OP_LAYERNORM ? OP_FUSED_LAYERNORM_SCALE
                                   : OP_FUSED_RMSNORM_SCALE,
                FUSION_WITH_SCALE);
    return true;
}

bool cpx_try_fuse_residual_norm(CpxOpGraph* g, int add_id) {
    if (g->nodes[add_id].op != OP_ADD) return false;
    if (!single_consumer(g, add_id)) return false;
    int succ1 = g->nodes[add_id].successors[0];
    CpxOpType nt = g->nodes[succ1].op;
    if (nt != OP_LAYERNORM && nt != OP_RMSNORM) return false;
    merge_nodes(g, add_id, succ1, OP_FUSED_RESIDUAL_NORM, FUSION_WITH_RESIDUAL);
    return true;
}

int cpx_fusion_pass(CpxOpGraph* g, const CpxCpuInfo* cpu) {
    (void)cpu;
    if (!g->is_sorted) cpx_opgraph_toposort(g);
    int count = 0;
    for (int ti = 0; ti < g->topo_len; ti++) {
        int id = g->topo_order[ti];
        if (cpx_try_fuse_gemm_bias_act(g, id))  { count++; continue; }
        if (cpx_try_fuse_norm_scale(g, id))      { count++; continue; }
        if (cpx_try_fuse_residual_norm(g, id))   { count++; continue; }
    }
    /* Re-sort after fusions removed nodes. */
    if (count > 0) cpx_opgraph_toposort(g);
    return count;
}

/* ════════════════════════════════════════════════════════════════════
 * 3. COST MODEL
 * ════════════════════════════════════════════════════════════════════ */

uint64_t cpx_cost_flops(const CpxOpNode* node) {
    /* Attrs convention for GEMM: attrs[0]=M, attrs[1]=K, attrs[2]=N. */
    switch (node->op) {
        case OP_GEMM:
        case OP_FUSED_GEMM_BIAS:
        case OP_FUSED_GEMM_BIAS_RELU:
        case OP_FUSED_GEMM_BIAS_GELU:
        case OP_FUSED_GEMM_BIAS_SILU: {
            uint64_t M = node->attrs[0], K = node->attrs[1], N = node->attrs[2];
            return 2 * M * K * N;
        }
        case OP_ATTENTION: {
            /* 2 GEMMs + softmax: approx 4 × B × H × Sq × Skv × D. */
            uint64_t B  = node->attrs[0], H = node->attrs[1];
            uint64_t Sq = node->attrs[2], Skv = node->attrs[3];
            uint64_t D  = node->attrs[4];
            return 4 * B * H * Sq * Skv * D;
        }
        case OP_LAYERNORM:
        case OP_RMSNORM: {
            uint64_t B = node->attrs[0], D = node->attrs[1];
            return 5 * B * D; /* mean/var/norm = ~5 ops/elem */
        }
        case OP_SOFTMAX: {
            uint64_t B = node->attrs[0], D = node->attrs[1];
            return 4 * B * D;
        }
        default: {
            /* Generic: 1 FLOP per element. */
            uint64_t n = 1;
            for (int i = 0; i < node->outputs[0].ndim; i++)
                n *= node->outputs[0].shape[i];
            return n;
        }
    }
}

uint64_t cpx_cost_bytes(const CpxOpNode* node) {
    uint64_t bytes = 0;
    for (int i = 0; i < node->num_inputs; i++) {
        uint64_t n = 1;
        for (int d = 0; d < node->inputs[i].ndim; d++)
            n *= node->inputs[i].shape[d];
        bytes += n * cpx_dtype_size(node->inputs[i].dtype);
    }
    for (int i = 0; i < node->num_outputs; i++) {
        uint64_t n = 1;
        for (int d = 0; d < node->outputs[i].ndim; d++)
            n *= node->outputs[i].shape[d];
        bytes += n * cpx_dtype_size(node->outputs[i].dtype);
    }
    return bytes;
}

float cpx_cost_predict_ms(const CpxOpNode* node, const CpxCpuInfo* cpu,
                             int num_threads) {
    uint64_t flops = cpx_cost_flops(node);
    uint64_t bytes = cpx_cost_bytes(node);

    /* Rough roofline: 32 GFLOPS/core/thread (AVX2), 40 GB/s mem bw. */
    double gflops = (cpu && cpu->avx2) ? 32.0 : 8.0;
    gflops *= num_threads;
    double bw_gb = 40.0;

    double t_compute = (double)flops / (gflops * 1e9) * 1000.0;  /* ms */
    double t_memory  = (double)bytes / (bw_gb * 1e9) * 1000.0;

    return (float)(t_compute > t_memory ? t_compute : t_memory);
}

/* ════════════════════════════════════════════════════════════════════
 * 4. PLANNER
 * ════════════════════════════════════════════════════════════════════ */

void cpx_planner_run(const CpxOpGraph* g, const CpxCpuInfo* cpu,
                       int num_threads, CpxSchedulePlan* out) {
    out->num_plans        = 0;
    out->total_predicted_ms = 0.f;

    for (int ti = 0; ti < g->topo_len; ti++) {
        int id = g->topo_order[ti];
        const CpxOpNode* node = &g->nodes[id];
        if (node->op == OP_NONE) continue;

        CpxExecPlan* plan = &out->plans[out->num_plans++];
        plan->op_id = id;
        plan->num_threads = num_threads;

        /* Tile sizes. */
        plan->mc = GEMM_MC; plan->nc = GEMM_NC; plan->kc = GEMM_KC;
        plan->mr = (cpu && cpu->avx512f) ? GEMM_MR_AVX512 : GEMM_MR_AVX2;
        plan->nr = (cpu && cpu->avx512f) ? GEMM_NR_AVX512 : GEMM_NR_AVX2;

        /* Parallelise along M for GEMM-like ops. */
        plan->parallel_dim = 0;
        if (node->attrs[0] > 0)
            plan->chunk_size = ((int)node->attrs[0] + num_threads - 1) / num_threads;
        else
            plan->chunk_size = 1;

        plan->working_set_bytes = (size_t)plan->mc * plan->kc * sizeof(float)
                                 + (size_t)plan->kc * plan->nc * sizeof(float);
        plan->arith_intensity   = (float)cpx_cost_flops(node)
                                / (float)(cpx_cost_bytes(node) + 1);
        plan->predicted_ms      = cpx_cost_predict_ms(node, cpu, num_threads);
        out->total_predicted_ms += plan->predicted_ms;
    }
}

/* ════════════════════════════════════════════════════════════════════
 * 5. KERNEL TABLE (stubs — real implementations in cpx_hpc_kernel.c)
 * ════════════════════════════════════════════════════════════════════ */

/* Forward declarations of kernel wrappers defined in this file. */
static void kern_noop(const CpxTensorView* in, int nin,
                       CpxTensorView* out, int nout,
                       const int64_t* attrs, int nattrs,
                       struct CpxScheduler* sched) {
    (void)in; (void)nin; (void)out; (void)nout;
    (void)attrs; (void)nattrs; (void)sched;
}

void cpx_kernel_table_init(CpxKernelTable* tbl, const CpxCpuInfo* cpu) {
    memset(tbl, 0, sizeof(*tbl));
    /* Populate with no-op stubs; real dispatch happens in run_op. */
    for (int op = 0; op < OP_COUNT; op++) {
        for (int i = 0; i < 4; i++) {
            tbl->entries[op].impls[i] = kern_noop;
        }
        tbl->entries[op].best_impl = cpu ? (cpu->avx512f ? 0 : cpu->avx2 ? 1 : 3) : 3;
    }
}

CpxKernelFn cpx_jit_select(CpxKernelTable* tbl, CpxOpType op,
                              const CpxCpuInfo* cpu) {
    int idx = cpu ? (cpu->avx512f ? 0 : cpu->avx2 ? 1 : 3) : 3;
    tbl->entries[op].best_impl = idx;
    return tbl->entries[op].impls[idx];
}

/* ════════════════════════════════════════════════════════════════════
 * 6. TENSOR ENGINE
 * ════════════════════════════════════════════════════════════════════ */

void cpx_tensor_engine_init(CpxTensorEngine* eng,
                               CpxScheduler* sched,
                               CpxMemArena* mem,
                               const CpxCpuInfo* cpu,
                               bool enable_profiling) {
    if (cpu) eng->cpu = *cpu;
    else     cpx_cpu_info(&eng->cpu);
    eng->sched            = sched;
    eng->mem              = mem;
    eng->enable_profiling = enable_profiling;
    cpx_kernel_table_init(&eng->kernel_tbl, &eng->cpu);
}

void cpx_tensor_engine_destroy(CpxTensorEngine* eng) {
    (void)eng; /* nothing heap-allocated in the engine itself */
}

void cpx_tensor_engine_run_op(CpxTensorEngine* eng, CpxOpNode* node) {
    if (node->op == OP_NONE) return;
    CpxKernelFn fn = cpx_jit_select(&eng->kernel_tbl, node->op, &eng->cpu);
    fn(node->inputs,  node->num_inputs,
       node->outputs, node->num_outputs,
       node->attrs,   node->num_attrs,
       eng->sched);
}

void cpx_tensor_engine_run(CpxTensorEngine* eng, CpxOpGraph* g) {
    /* Fusion pass. */
    cpx_fusion_pass(g, &eng->cpu);

    /* Topological order. */
    if (!g->is_sorted) cpx_opgraph_toposort(g);

    for (int ti = 0; ti < g->topo_len; ti++) {
        int id = g->topo_order[ti];
        if (g->nodes[id].op == OP_NONE) continue;
        cpx_tensor_engine_run_op(eng, &g->nodes[id]);
    }
}

/* ════════════════════════════════════════════════════════════════════
 * 7. PROFILING
 * ════════════════════════════════════════════════════════════════════ */

static const char* op_name(CpxOpType op) {
    switch (op) {
#define X(v) case v: return #v;
        X(OP_GEMM)          X(OP_GEMV)          X(OP_BATCHED_MATMUL)
        X(OP_ATTENTION)     X(OP_ATTENTION_DECODE)
        X(OP_LAYERNORM)     X(OP_RMSNORM)
        X(OP_RELU)          X(OP_GELU)           X(OP_GELU_APPROX)
        X(OP_SILU)          X(OP_SOFTMAX)
        X(OP_ADD)           X(OP_MUL)            X(OP_SCALE)
        X(OP_AXPBY)
        X(OP_EMBEDDING_LOOKUP) X(OP_EMBEDDING_UPDATE)
        X(OP_REDUCE_SUM)    X(OP_REDUCE_MEAN)    X(OP_REDUCE_MAX)
        X(OP_CROSS_ENTROPY) X(OP_MSE)
        X(OP_RESHAPE)       X(OP_TRANSPOSE)
        X(OP_SLICE)         X(OP_CONCAT)         X(OP_PAD)
        X(OP_FUSED_GEMM_BIAS)
        X(OP_FUSED_GEMM_BIAS_RELU)
        X(OP_FUSED_GEMM_BIAS_GELU)
        X(OP_FUSED_GEMM_BIAS_SILU)
        X(OP_FUSED_LAYERNORM_SCALE)
        X(OP_FUSED_RMSNORM_SCALE)
        X(OP_FUSED_ATTENTION)
        X(OP_FUSED_RESIDUAL_NORM)
        X(OP_FUSED_CROSS_ENTROPY_SOFTMAX)
        X(OP_FUSED_EMBEDDING_LAYERNORM)
#undef X
        default: return "OP_UNKNOWN";
    }
}

void cpx_engine_profile_print(const CpxEngineProfile* p) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Casprix Tensor Engine Profile                          ║\n");
    printf("╠═══════════════════════════════╦═══════╦═══════╦═════════╣\n");
    printf("║ Op                            ║ ms    ║GFLOPS ║ Eff%%    ║\n");
    printf("╠═══════════════════════════════╬═══════╬═══════╬═════════╣\n");
    for (int i = 0; i < p->num_ops; i++) {
        const CpxOpProfile* op = &p->ops[i];
        printf("║ %-29s ║%6.2f ║%6.1f ║%7.1f%% ║\n",
               op_name(op->op),
               op->elapsed_ms,
               op->gflops_per_sec,
               op->efficiency_pct);
    }
    printf("╠═══════════════════════════════╬═══════╬═══════╬═════════╣\n");
    printf("║ TOTAL                         ║%6.2f ║       ║         ║\n",
           p->total_ms);
    printf("║   GEMM                        ║%6.2f ║       ║         ║\n",
           p->gemm_ms);
    printf("║   Attention                   ║%6.2f ║       ║         ║\n",
           p->attention_ms);
    printf("║   Norm                        ║%6.2f ║       ║         ║\n",
           p->norm_ms);
    printf("╚═══════════════════════════════╩═══════╩═══════╩═════════╝\n");
}

void cpx_engine_profile_reset(CpxEngineProfile* p) {
    memset(p, 0, sizeof(*p));
}
