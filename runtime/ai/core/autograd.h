/* Dynamic reverse-mode autograd for the shared Casprix tensor ABI. */
#ifndef CPX_AI_CORE_AUTOGRAD_H
#define CPX_AI_CORE_AUTOGRAD_H

#include "tensor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AiGraph AiGraph;
typedef struct AiNode AiNode;

typedef struct AiValue {
    Tensor* data;
    Tensor* grad;
    AiGraph* graph;
    AiNode* creator;
    bool requires_grad;
    bool owns_data;
    bool persistent;
    size_t id;
} AiValue;

typedef enum AiOp {
    AI_OP_ADD,
    AI_OP_SUB,
    AI_OP_MUL,
    AI_OP_MATMUL,
    AI_OP_RELU,
    AI_OP_SIGMOID,
    AI_OP_SUM,
    AI_OP_MEAN,
    AI_OP_EMBEDDING,
    AI_OP_LAYER_NORM,
    AI_OP_CAUSAL_ATTENTION,
    AI_OP_CROSS_ENTROPY
} AiOp;

typedef struct AiBackwardGraph {
    AiNode** topological;
    size_t count;
} AiBackwardGraph;

AiGraph* ai_graph_create(void);
void ai_graph_destroy(AiGraph* graph);
const char* ai_graph_error(const AiGraph* graph);
/* Release the previous eager graph while preserving leaves/parameters. */
void ai_graph_next_step(AiGraph* graph);

/* Borrow an existing tensor, or create a graph-owned value/parameter. */
AiValue* ai_value(AiGraph* graph, Tensor* tensor, bool requires_grad);
AiValue* ai_value_copy(AiGraph* graph, i32 ndim, const i32* shape,
                       const f32* data, bool requires_grad);
AiValue* ai_value_copy_ephemeral(AiGraph* graph, i32 ndim, const i32* shape,
                                 const f32* data);
AiValue* ai_parameter(AiGraph* graph, i32 ndim, const i32* shape,
                      const f32* initial_data);

AiValue* ai_add(AiValue* a, AiValue* b);
AiValue* ai_sub(AiValue* a, AiValue* b);
AiValue* ai_mul(AiValue* a, AiValue* b);
AiValue* ai_matmul(AiValue* a, AiValue* b);
AiValue* ai_relu(AiValue* x);
AiValue* ai_sigmoid(AiValue* x);
AiValue* ai_sum(AiValue* x);
AiValue* ai_mean(AiValue* x);
AiValue* ai_mse_loss(AiValue* prediction, AiValue* target);
AiValue* ai_embedding(AiValue* token_ids, AiValue* table);
AiValue* ai_layer_norm(AiValue* x, AiValue* gamma, AiValue* beta, f32 epsilon);
AiValue* ai_causal_attention(AiValue* query, AiValue* key, AiValue* value,
                             i32 num_heads, i32 sequence_length);
AiValue* ai_cross_entropy(AiValue* logits, AiValue* target_ids);

/* Build the dependency-derived backward DAG in forward topological order. */
bool ai_build_backward_graph(AiValue* loss, AiBackwardGraph* backward);
void ai_backward_graph_destroy(AiBackwardGraph* backward);

/* Reverse the topological order and propagate/accumulate gradients. */
bool ai_backward(AiValue* loss);
bool ai_backward_with_gradient(AiValue* output, const Tensor* gradient);
void ai_graph_zero_grad(AiGraph* graph);
void ai_graph_sgd_step(AiValue** parameters, size_t count, f32 learning_rate);

#ifdef __cplusplus
}
#endif
#endif
