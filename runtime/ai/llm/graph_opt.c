/*
 * LLM Runtime - Graph Optimization Passes
 *
 * Implements DCE, constant folding, operator fusion, and memory planning
 * over the SSA-style Tensor IR.
 */

#include "graph_opt.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ============================================================================
 * Dead Code Elimination
 * ==========================================================================*/

i32 ir_dce(IRGraph* graph) {
    if (!graph) return 0;

    i32 total_eliminated = 0;
    bool changed = true;

    while (changed) {
        changed = false;

        /* Recompute use counts */
        for (i32 i = 0; i < graph->value_count; i++) {
            graph->values[i]->use_count = 0;
        }
        for (IRNode* node = graph->head; node; node = node->next) {
            if (node->eliminated) continue;
            for (i32 i = 0; i < node->num_inputs; i++) {
                if (node->inputs[i]) {
                    node->inputs[i]->use_count++;
                }
            }
        }

        /* Eliminate nodes with zero uses (except the last node = loss) */
        for (IRNode* node = graph->head; node; node = node->next) {
            if (node->eliminated) continue;
            if (node == graph->tail) continue; /* Never eliminate the output */

            if (node->output && node->output->use_count == 0) {
                node->eliminated = true;
                total_eliminated++;
                changed = true;
            }
        }
    }

    return total_eliminated;
}

/* ============================================================================
 * Constant Folding
 * ==========================================================================*/

i32 ir_constant_fold(IRGraph* graph) {
    if (!graph) return 0;

    i32 folded = 0;

    for (IRNode* node = graph->head; node; node = node->next) {
        if (node->eliminated) continue;

        switch (node->op) {
        case IR_SCALE:
            /* scale(x, 1.0) -> identity */
            if (fabsf(node->scalar - 1.0f) < 1e-7f) {
                /* Replace all uses of node->output with node->inputs[0] */
                IRValue* replacement = node->inputs[0];
                for (IRNode* user = node->next; user; user = user->next) {
                    if (user->eliminated) continue;
                    for (i32 i = 0; i < user->num_inputs; i++) {
                        if (user->inputs[i] == node->output) {
                            user->inputs[i] = replacement;
                        }
                    }
                }
                node->eliminated = true;
                folded++;
            }
            /* scale(x, 0.0) -> zero (skip for now, rare case) */
            break;

        case IR_ADD:
            /* Cannot easily fold add(x, 0) without knowing if the input is zero.
             * Would need a constant propagation lattice. Skip for now. */
            break;

        case IR_MUL:
            /* mul(x, 1) -> identity. Similar to scale. */
            break;

        default:
            break;
        }
    }

    return folded;
}

/* ============================================================================
 * Operator Fusion
 * ==========================================================================*/

/* Check if a node's output is used exactly once by the given consumer */
static bool is_single_use_by(IRNode* producer, IRNode* consumer) {
    if (!producer || !producer->output) return false;
    if (producer->output->use_count != 1) return false;

    for (i32 i = 0; i < consumer->num_inputs; i++) {
        if (consumer->inputs[i] == producer->output) return true;
    }
    return false;
}

i32 ir_fuse_ops(IRGraph* graph) {
    if (!graph) return 0;

    i32 fused = 0;

    /* Recompute use counts first */
    for (i32 i = 0; i < graph->value_count; i++) {
        graph->values[i]->use_count = 0;
    }
    for (IRNode* node = graph->head; node; node = node->next) {
        if (node->eliminated) continue;
        for (i32 i = 0; i < node->num_inputs; i++) {
            if (node->inputs[i]) {
                node->inputs[i]->use_count++;
            }
        }
    }

    for (IRNode* node = graph->head; node; node = node->next) {
        if (node->eliminated) continue;

        /* Pattern: GEMM_BIAS -> GELU (single use) */
        if (node->op == IR_GELU && node->inputs[0]) {
            IRNode* producer = node->inputs[0]->def;
            if (producer && !producer->eliminated &&
                producer->op == IR_GEMM_BIAS &&
                is_single_use_by(producer, node))
            {
                /* Fuse: replace GELU with FUSED_GEMM_BIAS_GELU */
                node->op = IR_FUSED_GEMM_BIAS_GELU;
                node->inputs[0] = producer->inputs[0]; /* A */
                node->inputs[1] = producer->inputs[1]; /* B */
                node->inputs[2] = producer->inputs[2]; /* bias */
                node->num_inputs = 3;
                node->M = producer->M;
                node->K = producer->K;
                node->N = producer->N;

                /* Update output shape */
                node->output->ndim = producer->output->ndim;
                memcpy(node->output->shape, producer->output->shape,
                       sizeof(i32) * MAX_TENSOR_DIM);
                node->output->size = producer->output->size;

                producer->eliminated = true;
                fused++;
                continue;
            }
        }

        /* Pattern: GEMM_BIAS -> RELU (single use) */
        if (node->op == IR_RELU && node->inputs[0]) {
            IRNode* producer = node->inputs[0]->def;
            if (producer && !producer->eliminated &&
                producer->op == IR_GEMM_BIAS &&
                is_single_use_by(producer, node))
            {
                node->op = IR_FUSED_GEMM_BIAS_RELU;
                node->inputs[0] = producer->inputs[0];
                node->inputs[1] = producer->inputs[1];
                node->inputs[2] = producer->inputs[2];
                node->num_inputs = 3;
                node->M = producer->M;
                node->K = producer->K;
                node->N = producer->N;

                node->output->ndim = producer->output->ndim;
                memcpy(node->output->shape, producer->output->shape,
                       sizeof(i32) * MAX_TENSOR_DIM);
                node->output->size = producer->output->size;

                producer->eliminated = true;
                fused++;
                continue;
            }
        }

        /* Pattern: ADD -> SCALE (single use of ADD by SCALE) */
        if (node->op == IR_SCALE && node->inputs[0]) {
            IRNode* producer = node->inputs[0]->def;
            if (producer && !producer->eliminated &&
                producer->op == IR_ADD &&
                is_single_use_by(producer, node))
            {
                node->op = IR_FUSED_ADD_SCALE;
                node->inputs[0] = producer->inputs[0];
                node->inputs[1] = producer->inputs[1];
                node->num_inputs = 2;
                /* scalar already set from IR_SCALE */

                producer->eliminated = true;
                fused++;
                continue;
            }
        }
    }

    return fused;
}

/* ============================================================================
 * Memory Planning (Liveness-based buffer reuse)
 * ==========================================================================*/

i32 ir_memory_plan(IRGraph* graph) {
    if (!graph) return 0;

    /*
     * Simple greedy memory planning:
     * 1. Assign each non-eliminated node a schedule order
     * 2. Compute live range [def, last_use] for each value
     * 3. Greedily assign memory slots, reusing slots when lifetimes don't overlap
     */

    /* Step 1: Assign schedule order */
    i32 order = 0;
    for (IRNode* node = graph->head; node; node = node->next) {
        if (node->eliminated) continue;
        node->schedule_order = order++;
    }
    i32 total_nodes = order;
    if (total_nodes == 0) return 0;

    /* Step 2: Compute live ranges */
    /* For each value: def_order = schedule_order of defining node
     *                 last_use_order = max schedule_order of any user */
    for (i32 i = 0; i < graph->value_count; i++) {
        IRValue* val = graph->values[i];
        if (!val->def || val->def->eliminated) continue;
        /* Will be set during slot assignment */
    }

    /* Build last-use table: for each value id, track the latest using node */
    i32* last_use = (i32*)tensor_arena_alloc(graph->arena,
        graph->value_count * sizeof(i32), 4);
    if (!last_use) return 0;

    for (i32 i = 0; i < graph->value_count; i++) {
        last_use[i] = -1;
    }

    for (IRNode* node = graph->head; node; node = node->next) {
        if (node->eliminated) continue;
        for (i32 i = 0; i < node->num_inputs; i++) {
            if (node->inputs[i] && node->inputs[i]->id < graph->value_count) {
                i32 vid = node->inputs[i]->id;
                if (node->schedule_order > last_use[vid]) {
                    last_use[vid] = node->schedule_order;
                }
            }
        }
    }

    /* Step 3: Greedy slot assignment */
    /* Each slot tracks when it becomes free (last_use_order + 1) */
    #define MAX_SLOTS 512
    i32 slot_free_at[MAX_SLOTS];
    size_t slot_size[MAX_SLOTS];
    i32 num_slots = 0;

    for (IRNode* node = graph->head; node; node = node->next) {
        if (node->eliminated || !node->output) continue;

        IRValue* val = node->output;
        i32 def_order = node->schedule_order;
        i32 val_last_use = last_use[val->id];
        if (val_last_use < def_order) val_last_use = def_order;

        size_t needed = val->size * sizeof(f32);

        /* Try to reuse an existing slot */
        i32 best_slot = -1;
        for (i32 s = 0; s < num_slots; s++) {
            if (slot_free_at[s] <= def_order && slot_size[s] >= needed) {
                best_slot = s;
                break;
            }
        }

        if (best_slot >= 0) {
            node->memory_slot = best_slot;
            slot_free_at[best_slot] = val_last_use + 1;
        } else if (num_slots < MAX_SLOTS) {
            /* Allocate new slot */
            node->memory_slot = num_slots;
            slot_free_at[num_slots] = val_last_use + 1;
            slot_size[num_slots] = needed;
            num_slots++;
        } else {
            node->memory_slot = -1; /* No slot available */
        }
    }

    #undef MAX_SLOTS
    return num_slots;
}

/* ============================================================================
 * Combined Pipeline
 * ==========================================================================*/

IROptStats ir_optimize(IRGraph* graph) {
    IROptStats stats = {0};
    if (!graph) return stats;

    stats.nodes_before = ir_graph_live_count(graph);

    stats.dce_eliminated = ir_dce(graph);
    stats.constants_folded = ir_constant_fold(graph);
    stats.ops_fused = ir_fuse_ops(graph);
    stats.dce_eliminated += ir_dce(graph); /* Clean up after fusion */
    stats.memory_slots = ir_memory_plan(graph);

    stats.nodes_after = ir_graph_live_count(graph);

    return stats;
}

void ir_opt_print_stats(IROptStats* stats) {
    if (!stats) return;
    printf("=== IR Optimization Statistics ===\n");
    printf("  Nodes before:     %d\n", stats->nodes_before);
    printf("  Nodes after:      %d\n", stats->nodes_after);
    printf("  DCE eliminated:   %d\n", stats->dce_eliminated);
    printf("  Constants folded: %d\n", stats->constants_folded);
    printf("  Ops fused:        %d\n", stats->ops_fused);
    printf("  Memory slots:     %d\n", stats->memory_slots);
    printf("==================================\n");
}
