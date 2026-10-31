#ifndef CPX_AI_CORE_BINDINGS_H
#define CPX_AI_CORE_BINDINGS_H
#include "autograd.h"

void* cpx_ai_graph_create(void);
void cpx_ai_graph_destroy(void* graph);
const char* cpx_ai_graph_error(void* graph);
void* cpx_ai_tensor(void* graph, i32 rows, i32 cols, i32 requires_grad);
/* Casprix float-family values currently use the platform f64 call ABI. */
void* cpx_ai_parameter(void* graph, i32 rows, i32 cols, double initial_value);
void* cpx_ai_parameter_normal(void* graph, i32 rows, i32 cols,
                              double standard_deviation, i32 seed);
void cpx_ai_set(void* value, i32 index, double number);
double cpx_ai_get(void* value, i32 index);
double cpx_ai_grad(void* value, i32 index);
i32 cpx_ai_is_near(void* value, i32 index, double expected, double tolerance);
void* cpx_ai_add(void* a, void* b);
void* cpx_ai_sub(void* a, void* b);
void* cpx_ai_mul(void* a, void* b);
void* cpx_ai_matmul(void* a, void* b);
void* cpx_ai_relu(void* value);
void* cpx_ai_sigmoid(void* value);
void* cpx_ai_mean(void* value);
void* cpx_ai_mse(void* prediction, void* target);
void* cpx_ai_embedding(void* token_ids, void* table);
void* cpx_ai_layer_norm(void* value, void* gamma, void* beta, double epsilon);
void* cpx_ai_causal_attention(void* query, void* key, void* value,
                              i32 heads, i32 sequence_length);
void* cpx_ai_cross_entropy(void* logits, void* target_ids);
i32 cpx_ai_backward(void* loss);
void cpx_ai_sgd(void* parameter, double learning_rate);
void cpx_ai_zero_grad(void* graph);
void cpx_ai_next_step(void* graph);

void* cpx_ai_adamw_create(i32 parameter_count, double learning_rate,
                          double beta1, double beta2, double weight_decay);
i32 cpx_ai_adamw_add(void* optimizer, i32 index, void* parameter);
i32 cpx_ai_adamw_step(void* optimizer);
void cpx_ai_adamw_set_lr(void* optimizer, double learning_rate);
void cpx_ai_adamw_destroy(void* optimizer);

#endif
