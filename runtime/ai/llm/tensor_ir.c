/*
 * LLM Runtime - Tensor IR Implementation
 *
 * SSA-style IR builder, tape->IR lowering, printer, and verification.
 */

#include "tensor_ir.h"
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * IR Graph Creation
 * ==========================================================================*/

#define IR_INITIAL_VALUES 256

IRGraph* ir_graph_create(Arena* arena) {
    IRGraph* graph = (IRGraph*)arena_alloc(arena, sizeof(IRGraph), 8);
    if (!graph) return NULL;
    memset(graph, 0, sizeof(IRGraph));

    graph->arena = arena;
    graph->head = NULL;
    graph->tail = NULL;
    graph->node_count = 0;
    graph->next_value_id = 0;

    graph->value_capacity = IR_INITIAL_VALUES;
    graph->values = (IRValue**)arena_alloc(arena,
        graph->value_capacity * sizeof(IRValue*), 8);
    graph->value_count = 0;

    return graph;
}

/* ============================================================================
 * SSA Value Creation
 * ==========================================================================*/

static void ir_track_value(IRGraph* graph, IRValue* val) {
    if (graph->value_count >= graph->value_capacity) {
        i32 new_cap = graph->value_capacity * 2;
        IRValue** new_arr = (IRValue**)arena_alloc(graph->arena,
            new_cap * sizeof(IRValue*), 8);
        if (!new_arr) return;
        memcpy(new_arr, graph->values, graph->value_count * sizeof(IRValue*));
        graph->values = new_arr;
        graph->value_capacity = new_cap;
    }
    graph->values[graph->value_count++] = val;
}

IRValue* ir_value_create(IRGraph* graph, i32 ndim, const i32* shape) {
    IRValue* val = (IRValue*)arena_alloc(graph->arena, sizeof(IRValue), 8);
    if (!val) return NULL;
    memset(val, 0, sizeof(IRValue));

    val->id = graph->next_value_id++;
    val->ndim = ndim;
    val->size = 1;
    for (i32 i = 0; i < ndim && i < MAX_TENSOR_DIM; i++) {
        val->shape[i] = shape[i];
        val->size *= shape[i];
    }
    val->use_count = 0;
    val->is_param = false;
    val->external = NULL;
    val->def = NULL;

    ir_track_value(graph, val);
    return val;
}

IRValue* ir_value_external(IRGraph* graph, Tensor* tensor, bool is_param) {
    IRValue* val = ir_value_create(graph, tensor->ndim, tensor->shape);
    if (!val) return NULL;
    val->is_param = is_param;
    val->external = tensor;
    return val;
}

/* ============================================================================
 * Instruction Emission
 * ==========================================================================*/

IRNode* ir_emit(IRGraph* graph, IROpCode op, IRValue** inputs, i32 num_inputs,
                i32 ndim, const i32* out_shape) {
    IRNode* node = (IRNode*)arena_alloc(graph->arena, sizeof(IRNode), 8);
    if (!node) return NULL;
    memset(node, 0, sizeof(IRNode));

    node->op = op;
    node->num_inputs = num_inputs;
    for (i32 i = 0; i < num_inputs && i < IR_MAX_INPUTS; i++) {
        node->inputs[i] = inputs[i];
        if (inputs[i]) {
            inputs[i]->use_count++;
        }
    }

    /* Create output value */
    node->output = ir_value_create(graph, ndim, out_shape);
    if (node->output) {
        node->output->def = node;
    }

    /* Append to linked list */
    node->prev = graph->tail;
    node->next = NULL;
    if (graph->tail) {
        graph->tail->next = node;
    } else {
        graph->head = node;
    }
    graph->tail = node;
    graph->node_count++;

    return node;
}

/* ============================================================================
 * Convenience Emitters
 * ==========================================================================*/

IRNode* ir_emit_gemm(IRGraph* graph, IRValue* A, IRValue* B,
                      i32 M, i32 K, i32 N) {
    IRValue* inputs[] = { A, B };
    i32 shape[] = { M, N };
    IRNode* node = ir_emit(graph, IR_GEMM, inputs, 2, 2, shape);
    if (node) {
        node->M = M; node->K = K; node->N = N;
    }
    return node;
}

IRNode* ir_emit_gemm_bias(IRGraph* graph, IRValue* A, IRValue* B, IRValue* bias,
                           i32 M, i32 K, i32 N) {
    IRValue* inputs[] = { A, B, bias };
    i32 shape[] = { M, N };
    IRNode* node = ir_emit(graph, IR_GEMM_BIAS, inputs, 3, 2, shape);
    if (node) {
        node->M = M; node->K = K; node->N = N;
    }
    return node;
}

IRNode* ir_emit_add(IRGraph* graph, IRValue* a, IRValue* b) {
    IRValue* inputs[] = { a, b };
    IRNode* node = ir_emit(graph, IR_ADD, inputs, 2, a->ndim, a->shape);
    return node;
}

IRNode* ir_emit_scale(IRGraph* graph, IRValue* a, f32 scalar) {
    IRValue* inputs[] = { a };
    IRNode* node = ir_emit(graph, IR_SCALE, inputs, 1, a->ndim, a->shape);
    if (node) {
        node->scalar = scalar;
    }
    return node;
}

IRNode* ir_emit_gelu(IRGraph* graph, IRValue* x) {
    IRValue* inputs[] = { x };
    return ir_emit(graph, IR_GELU, inputs, 1, x->ndim, x->shape);
}

IRNode* ir_emit_relu(IRGraph* graph, IRValue* x) {
    IRValue* inputs[] = { x };
    return ir_emit(graph, IR_RELU, inputs, 1, x->ndim, x->shape);
}

IRNode* ir_emit_layernorm(IRGraph* graph, IRValue* x, IRValue* gamma, IRValue* beta,
                           i32 batch, i32 dim, f32 eps) {
    IRValue* inputs[] = { x, gamma, beta };
    i32 shape[] = { batch, dim };
    IRNode* node = ir_emit(graph, IR_LAYERNORM, inputs, 3, 2, shape);
    if (node) {
        node->batch = batch; node->dim = dim; node->eps = eps;
    }
    return node;
}

IRNode* ir_emit_softmax(IRGraph* graph, IRValue* x, i32 batch, i32 dim) {
    IRValue* inputs[] = { x };
    i32 shape[] = { batch, dim };
    IRNode* node = ir_emit(graph, IR_SOFTMAX, inputs, 1, 2, shape);
    if (node) {
        node->batch = batch; node->dim = dim;
    }
    return node;
}

/* ============================================================================
 * Tape -> IR Lowering
 * ==========================================================================*/

IRGraph* tape_to_ir(Tape* tape, Arena* ir_arena) {
    if (!tape || tape->count == 0) return NULL;

    IRGraph* graph = ir_graph_create(ir_arena);
    if (!graph) return NULL;

    /*
     * Map from GradTensor* -> IRValue* for SSA linkage.
     * We use a simple linear scan since tape tensors are small-numbered.
     */
    i32 max_tensors = tape->tensor_count;
    IRValue** tensor_map = (IRValue**)arena_alloc(ir_arena,
        max_tensors * sizeof(IRValue*), 8);
    if (!tensor_map) return NULL;
    memset(tensor_map, 0, max_tensors * sizeof(IRValue*));

    /* Helper: get or create IRValue for a GradTensor */
    #define GET_IR_VAL(gt) ( \
        (gt)->id < max_tensors && tensor_map[(gt)->id] \
            ? tensor_map[(gt)->id] \
            : (tensor_map[(gt)->id] = ir_value_external(graph, (gt)->data, (gt)->requires_grad)) \
    )

    /* Lower each tape entry to IR */
    for (i32 i = 0; i < tape->count; i++) {
        TapeEntry* entry = &tape->entries[i];
        IRNode* node = NULL;

        switch (entry->op) {
        case OP_GEMM: {
            IRValue* A = GET_IR_VAL(entry->inputs[0]);
            IRValue* B = GET_IR_VAL(entry->inputs[1]);
            node = ir_emit_gemm(graph, A, B, entry->M, entry->K, entry->N);
            break;
        }
        case OP_GEMM_BIAS: {
            IRValue* A = GET_IR_VAL(entry->inputs[0]);
            IRValue* B = GET_IR_VAL(entry->inputs[1]);
            IRValue* bias = GET_IR_VAL(entry->inputs[2]);
            node = ir_emit_gemm_bias(graph, A, B, bias, entry->M, entry->K, entry->N);
            break;
        }
        case OP_LAYERNORM: {
            IRValue* x = GET_IR_VAL(entry->inputs[0]);
            IRValue* gamma = GET_IR_VAL(entry->inputs[1]);
            IRValue* beta = GET_IR_VAL(entry->inputs[2]);
            node = ir_emit_layernorm(graph, x, gamma, beta,
                                      entry->batch, entry->dim, entry->eps);
            break;
        }
        case OP_GELU: {
            IRValue* x = GET_IR_VAL(entry->inputs[0]);
            node = ir_emit_gelu(graph, x);
            break;
        }
        case OP_RELU: {
            IRValue* x = GET_IR_VAL(entry->inputs[0]);
            node = ir_emit_relu(graph, x);
            break;
        }
        case OP_SOFTMAX: {
            IRValue* x = GET_IR_VAL(entry->inputs[0]);
            node = ir_emit_softmax(graph, x, entry->batch, entry->dim);
            break;
        }
        case OP_ADD: {
            IRValue* a = GET_IR_VAL(entry->inputs[0]);
            IRValue* b = GET_IR_VAL(entry->inputs[1]);
            node = ir_emit_add(graph, a, b);
            break;
        }
        case OP_SCALE: {
            IRValue* x = GET_IR_VAL(entry->inputs[0]);
            node = ir_emit_scale(graph, x, entry->scalar);
            break;
        }
        case OP_EMBEDDING: {
            IRValue* table = GET_IR_VAL(entry->inputs[0]);
            IRValue* inputs[] = { table };
            i32 shape[] = { entry->batch, entry->dim };
            node = ir_emit(graph, IR_EMBEDDING, inputs, 1, 2, shape);
            if (node) {
                node->token_ids = entry->token_ids;
                node->vocab_size = entry->vocab_size;
                node->batch = entry->batch;
                node->dim = entry->dim;
            }
            break;
        }
        case OP_CROSS_ENTROPY: {
            IRValue* logits = GET_IR_VAL(entry->inputs[0]);
            IRValue* inputs[] = { logits };
            i32 shape[] = { 1 };
            node = ir_emit(graph, IR_CROSS_ENTROPY, inputs, 1, 1, shape);
            if (node) {
                node->targets = entry->targets;
                node->batch = entry->batch;
                node->vocab_size = entry->vocab_size;
            }
            break;
        }
        case OP_ATTENTION: {
            IRValue* x = GET_IR_VAL(entry->inputs[0]);
            IRValue* Wq = GET_IR_VAL(entry->inputs[1]);
            IRValue* Wk = GET_IR_VAL(entry->inputs[2]);
            IRValue* Wv = GET_IR_VAL(entry->inputs[3]);
            IRValue* inputs[] = { x, Wq, Wk, Wv };
            i32 hid = entry->num_heads * entry->head_dim;
            i32 BS = entry->batch * entry->seq_len;
            i32 shape[] = { BS, hid };
            node = ir_emit(graph, IR_ATTENTION, inputs, 4, 2, shape);
            if (node) {
                node->batch = entry->batch;
                node->seq_len = entry->seq_len;
                node->num_heads = entry->num_heads;
                node->head_dim = entry->head_dim;
            }
            break;
        }
        case OP_VEC_ADD_BIAS: {
            IRValue* x = GET_IR_VAL(entry->inputs[0]);
            IRValue* bias = GET_IR_VAL(entry->inputs[1]);
            node = ir_emit_add(graph, x, bias);
            if (node) {
                node->M = entry->M;
                node->N = entry->N;
            }
            break;
        }
        case OP_TRANSPOSE:
            /* Transpose is handled implicitly within attention */
            break;
        }

        /* Map output GradTensor to the new IRValue */
        if (node && node->output && entry->output) {
            if (entry->output->id < max_tensors) {
                tensor_map[entry->output->id] = node->output;
            }
        }
    }

    #undef GET_IR_VAL

    return graph;
}

/* ============================================================================
 * IR Utilities
 * ==========================================================================*/

const char* ir_op_name(IROpCode op) {
    static const char* names[] = {
        [IR_GEMM]       = "gemm",
        [IR_GEMM_BIAS]  = "gemm_bias",
        [IR_TRANSPOSE]  = "transpose",
        [IR_ADD]        = "add",
        [IR_MUL]        = "mul",
        [IR_SCALE]      = "scale",
        [IR_GELU]       = "gelu",
        [IR_RELU]       = "relu",
        [IR_SILU]       = "silu",
        [IR_LAYERNORM]  = "layernorm",
        [IR_RMSNORM]    = "rmsnorm",
        [IR_SOFTMAX]    = "softmax",
        [IR_LOAD]       = "load",
        [IR_STORE]      = "store",
        [IR_CONST]      = "const",
        [IR_FUSED_GEMM_BIAS_GELU]  = "fused.gemm_bias_gelu",
        [IR_FUSED_GEMM_BIAS_RELU]  = "fused.gemm_bias_relu",
        [IR_FUSED_ADD_SCALE]        = "fused.add_scale",
        [IR_FUSED_BIAS_RESIDUAL]    = "fused.bias_residual",
        [IR_EMBEDDING]      = "embedding",
        [IR_CROSS_ENTROPY]  = "cross_entropy",
        [IR_ATTENTION]      = "attention",
    };

    if (op >= 0 && op < IR_OP_COUNT) {
        return names[op] ? names[op] : "unknown";
    }
    return "unknown";
}

static void print_shape(i32 ndim, const i32* shape) {
    printf("[");
    for (i32 i = 0; i < ndim; i++) {
        printf("%d%s", shape[i], i < ndim - 1 ? "," : "");
    }
    printf("]");
}

void ir_graph_print(IRGraph* graph) {
    if (!graph) return;

    printf("=== Tensor IR Graph (%d nodes, %d values) ===\n",
           graph->node_count, graph->value_count);

    for (IRNode* node = graph->head; node; node = node->next) {
        if (node->eliminated) continue;

        /* Output */
        if (node->output) {
            printf("  %%%d", node->output->id);
            print_shape(node->output->ndim, node->output->shape);
        } else {
            printf("  void");
        }

        /* Operation */
        printf(" = %s(", ir_op_name(node->op));

        /* Inputs */
        for (i32 i = 0; i < node->num_inputs; i++) {
            if (node->inputs[i]) {
                printf("%%%d", node->inputs[i]->id);
                if (node->inputs[i]->is_param) printf("*");
            } else {
                printf("null");
            }
            if (i < node->num_inputs - 1) printf(", ");
        }

        printf(")");

        /* Attributes */
        switch (node->op) {
        case IR_GEMM:
        case IR_GEMM_BIAS:
        case IR_FUSED_GEMM_BIAS_GELU:
        case IR_FUSED_GEMM_BIAS_RELU:
            printf(" {M=%d, K=%d, N=%d}", node->M, node->K, node->N);
            break;
        case IR_LAYERNORM:
        case IR_RMSNORM:
            printf(" {batch=%d, dim=%d, eps=%.1e}", node->batch, node->dim, node->eps);
            break;
        case IR_SOFTMAX:
            printf(" {batch=%d, dim=%d}", node->batch, node->dim);
            break;
        case IR_SCALE:
            printf(" {scalar=%.6f}", node->scalar);
            break;
        case IR_ATTENTION:
            printf(" {batch=%d, seq=%d, heads=%d, head_dim=%d}",
                   node->batch, node->seq_len, node->num_heads, node->head_dim);
            break;
        default:
            break;
        }

        /* Use count */
        if (node->output) {
            printf("  ; uses=%d", node->output->use_count);
        }
        printf("\n");
    }

    printf("=== End IR ===\n");
}

i32 ir_graph_live_count(IRGraph* graph) {
    if (!graph) return 0;
    i32 count = 0;
    for (IRNode* node = graph->head; node; node = node->next) {
        if (!node->eliminated) count++;
    }
    return count;
}

bool ir_graph_verify(IRGraph* graph) {
    if (!graph) return false;

    bool ok = true;
    for (IRNode* node = graph->head; node; node = node->next) {
        if (node->eliminated) continue;

        /* Check output exists */
        if (!node->output) {
            fprintf(stderr, "IR verify: node %s has no output\n", ir_op_name(node->op));
            ok = false;
            continue;
        }

        /* Check output is defined by this node */
        if (node->output->def != node) {
            fprintf(stderr, "IR verify: %%%d def mismatch\n", node->output->id);
            ok = false;
        }

        /* Check inputs exist */
        for (i32 i = 0; i < node->num_inputs; i++) {
            if (!node->inputs[i]) {
                fprintf(stderr, "IR verify: %s input %d is null\n",
                        ir_op_name(node->op), i);
                ok = false;
            }
        }
    }

    return ok;
}
