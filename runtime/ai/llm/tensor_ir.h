/*
 * LLM Runtime - Tensor Intermediate Representation
 *
 * SSA-style IR for tensor operations. Each IRValue is defined exactly once.
 * Used as a bridge between the autograd tape and optimization passes.
 *
 * Tape -> IR lowering: tape_to_ir()
 * IR -> optimized IR: ir_optimize() (in graph_opt.h)
 * IR -> execution: interpret or JIT compile
 */

#ifndef LLM_TENSOR_IR_H
#define LLM_TENSOR_IR_H

#include "tensor.h"
#include "autograd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * IR Operation Codes
 * ==========================================================================*/

typedef enum {
    /* Matrix operations */
    IR_GEMM,            /* %out = gemm(%A, %B)  A:[M,K] B:[K,N] -> [M,N]    */
    IR_GEMM_BIAS,       /* %out = gemm(%A, %B) + %bias                       */
    IR_TRANSPOSE,       /* %out = transpose(%A)                               */

    /* Elementwise operations */
    IR_ADD,             /* %out = %a + %b                                     */
    IR_MUL,             /* %out = %a * %b                                     */
    IR_SCALE,           /* %out = %a * scalar                                 */
    IR_GELU,            /* %out = gelu(%a)                                    */
    IR_RELU,            /* %out = relu(%a)                                    */
    IR_SILU,            /* %out = silu(%a)                                    */

    /* Normalization */
    IR_LAYERNORM,       /* %out = layernorm(%x, %gamma, %beta)                */
    IR_RMSNORM,         /* %out = rmsnorm(%x, %gamma)                         */
    IR_SOFTMAX,         /* %out = softmax(%x)                                 */

    /* Memory */
    IR_LOAD,            /* %out = load(ptr)  -- load from external tensor     */
    IR_STORE,           /* store(%val, ptr)  -- write to external tensor      */
    IR_CONST,           /* %out = constant scalar                             */

    /* Fused operations (created by optimization passes) */
    IR_FUSED_GEMM_BIAS_GELU,   /* %out = gelu(gemm(%A,%B) + %bias)           */
    IR_FUSED_GEMM_BIAS_RELU,   /* %out = relu(gemm(%A,%B) + %bias)           */
    IR_FUSED_ADD_SCALE,         /* %out = (%a + %b) * scalar                  */
    IR_FUSED_BIAS_RESIDUAL,     /* %out = %x + %bias + %residual              */

    /* Special */
    IR_EMBEDDING,       /* %out = embedding(%table, ids)                      */
    IR_CROSS_ENTROPY,   /* %out = cross_entropy(%logits, targets)             */
    IR_ATTENTION,       /* %out = attention(%x, %Wq, %Wk, %Wv, %Wo)          */

    IR_OP_COUNT
} IROpCode;

/* ============================================================================
 * IR Value (SSA value - defined once, used many times)
 * ==========================================================================*/

typedef struct IRNode IRNode;

typedef struct IRValue {
    i32 id;                     /* Unique SSA value number (%0, %1, ...)      */
    i32 shape[MAX_TENSOR_DIM];  /* Tensor shape                              */
    i32 ndim;                   /* Number of dimensions                      */
    size_t size;                /* Total elements                            */
    IRNode* def;                /* The instruction that defines this value   */
    i32 use_count;              /* Number of times used as input             */
    bool is_param;              /* True if this is a model parameter         */
    Tensor* external;           /* For IR_LOAD: pointer to external tensor   */
} IRValue;

/* ============================================================================
 * IR Node (single instruction)
 * ==========================================================================*/

#define IR_MAX_INPUTS 5

struct IRNode {
    IROpCode op;

    /* Inputs and output (SSA values) */
    IRValue* inputs[IR_MAX_INPUTS];
    i32 num_inputs;
    IRValue* output;

    /* Scalar attributes */
    f32 scalar;                 /* For IR_SCALE, IR_CONST                    */
    f32 eps;                    /* For IR_LAYERNORM                          */

    /* Shape attributes */
    i32 M, K, N;               /* For GEMM: A[M,K] * B[K,N]                */
    i32 batch, dim;             /* For norms/softmax                        */
    i32 seq_len, num_heads, head_dim;  /* For attention                     */
    i32 vocab_size;             /* For embedding/cross_entropy               */

    /* External data pointers (for embedding/cross_entropy) */
    const i32* token_ids;
    const i32* targets;

    /* Linked list within graph */
    IRNode* next;
    IRNode* prev;

    /* Optimization metadata */
    bool eliminated;            /* Marked for removal by DCE                */
    i32 schedule_order;         /* Execution order after scheduling         */
    i32 memory_slot;            /* Memory reuse slot assigned by planner    */
};

/* ============================================================================
 * IR Graph
 * ==========================================================================*/

typedef struct {
    IRNode* head;               /* First instruction                        */
    IRNode* tail;               /* Last instruction                         */
    i32 node_count;             /* Number of active instructions            */
    i32 next_value_id;          /* Next SSA value ID to assign              */
    Arena* arena;               /* All IR nodes allocated here              */

    /* Value tracking */
    IRValue** values;           /* All IRValues indexed by id               */
    i32 value_count;
    i32 value_capacity;
} IRGraph;

/* ============================================================================
 * IR Graph API
 * ==========================================================================*/

/* Create an empty IR graph */
IRGraph* ir_graph_create(Arena* arena);

/* Create an SSA value */
IRValue* ir_value_create(IRGraph* graph, i32 ndim, const i32* shape);

/* Create an SSA value for external tensor (parameter or input) */
IRValue* ir_value_external(IRGraph* graph, Tensor* tensor, bool is_param);

/* Append an instruction to the graph */
IRNode* ir_emit(IRGraph* graph, IROpCode op, IRValue** inputs, i32 num_inputs,
                i32 ndim, const i32* out_shape);

/* Convenience emitters */
IRNode* ir_emit_gemm(IRGraph* graph, IRValue* A, IRValue* B,
                      i32 M, i32 K, i32 N);
IRNode* ir_emit_gemm_bias(IRGraph* graph, IRValue* A, IRValue* B, IRValue* bias,
                           i32 M, i32 K, i32 N);
IRNode* ir_emit_add(IRGraph* graph, IRValue* a, IRValue* b);
IRNode* ir_emit_scale(IRGraph* graph, IRValue* a, f32 scalar);
IRNode* ir_emit_gelu(IRGraph* graph, IRValue* x);
IRNode* ir_emit_relu(IRGraph* graph, IRValue* x);
IRNode* ir_emit_layernorm(IRGraph* graph, IRValue* x, IRValue* gamma, IRValue* beta,
                           i32 batch, i32 dim, f32 eps);
IRNode* ir_emit_softmax(IRGraph* graph, IRValue* x, i32 batch, i32 dim);

/* ============================================================================
 * Tape -> IR Lowering
 * ==========================================================================*/

/* Convert an autograd tape to an IR graph */
IRGraph* tape_to_ir(Tape* tape, Arena* ir_arena);

/* ============================================================================
 * IR Utilities
 * ==========================================================================*/

/* Print IR graph in textual form */
void ir_graph_print(IRGraph* graph);

/* Get op name string */
const char* ir_op_name(IROpCode op);

/* Count live (non-eliminated) nodes */
i32 ir_graph_live_count(IRGraph* graph);

/* Verify IR graph integrity (SSA property, type consistency) */
bool ir_graph_verify(IRGraph* graph);

#ifdef __cplusplus
}
#endif

#endif /* LLM_TENSOR_IR_H */
