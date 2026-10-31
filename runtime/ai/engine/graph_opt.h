/*
 * LLM Runtime - Graph Optimization Passes
 *
 * Optimization passes over the Tensor IR:
 *   1. Dead Code Elimination (DCE)
 *   2. Constant Folding
 *   3. Operator Fusion (GEMM+bias+activation, elementwise chains)
 *   4. Memory Planning (liveness-based buffer reuse)
 *
 * All passes operate in-place on the IRGraph.
 */

#ifndef LLM_GRAPH_OPT_H
#define LLM_GRAPH_OPT_H

#include "tensor_ir.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Individual Passes
 * ==========================================================================*/

/* Dead Code Elimination: remove nodes whose output is never used.
 * Iterates until fixed point. Returns number of nodes eliminated. */
i32 ir_dce(IRGraph* graph);

/* Constant Folding: evaluate compile-time-known expressions.
 * Folds scale(x, 1.0) -> identity, add(x, 0) -> identity.
 * Returns number of nodes folded. */
i32 ir_constant_fold(IRGraph* graph);

/* Operator Fusion: fuse compatible adjacent operations.
 * Patterns:
 *   GEMM_BIAS + GELU -> FUSED_GEMM_BIAS_GELU
 *   GEMM_BIAS + RELU -> FUSED_GEMM_BIAS_RELU
 *   ADD + SCALE -> FUSED_ADD_SCALE
 * Returns number of fusions performed. */
i32 ir_fuse_ops(IRGraph* graph);

/* Memory Planning: assign memory slots to IR values based on liveness.
 * Values with non-overlapping lifetimes share the same memory slot.
 * Sets node->memory_slot for each node. Returns number of unique slots. */
i32 ir_memory_plan(IRGraph* graph);

/* ============================================================================
 * Combined Pipeline
 * ==========================================================================*/

/* Run all optimization passes in order */
typedef struct {
    i32 dce_eliminated;
    i32 constants_folded;
    i32 ops_fused;
    i32 memory_slots;
    i32 nodes_before;
    i32 nodes_after;
} IROptStats;

IROptStats ir_optimize(IRGraph* graph);

/* Print optimization statistics */
void ir_opt_print_stats(IROptStats* stats);

#ifdef __cplusplus
}
#endif

#endif /* LLM_GRAPH_OPT_H */
