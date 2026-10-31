#include "bindings.h"
#include "../training/optimizer.h"
#include <math.h>
#include <stdlib.h>

void* cpx_ai_graph_create(void) { return ai_graph_create(); }
void cpx_ai_graph_destroy(void* graph) { ai_graph_destroy((AiGraph*)graph); }
const char* cpx_ai_graph_error(void* graph) { return ai_graph_error((AiGraph*)graph); }

void* cpx_ai_tensor(void* graph, i32 rows, i32 cols, i32 requires_grad) {
    i32 shape[2] = {rows, cols};
    return ai_value_copy((AiGraph*)graph, 2, shape, NULL, requires_grad != 0);
}

void* cpx_ai_parameter(void* graph, i32 rows, i32 cols, double initial_value) {
    i32 shape[2] = {rows, cols};
    AiValue* value = ai_parameter((AiGraph*)graph, 2, shape, NULL);
    if (value) tensor_fill(value->data, (f32)initial_value);
    return value;
}

void* cpx_ai_parameter_normal(void* graph, i32 rows, i32 cols,
                              double standard_deviation, i32 seed) {
    AiValue* value = (AiValue*)cpx_ai_parameter(graph, rows, cols, 0.0);
    if (!value || standard_deviation < 0.0) return NULL;
    unsigned state = seed ? (unsigned)seed : 0x9e3779b9u;
    for (size_t i = 0; i < value->data->size; i += 2) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        f32 u1 = ((state & 0x00ffffffu) + 1.0f) / 16777217.0f;
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        f32 u2 = ((state & 0x00ffffffu) + 1.0f) / 16777217.0f;
        f32 radius = sqrtf(-2.0f * logf(u1)) * (f32)standard_deviation;
        f32 angle = 6.28318530718f * u2;
        value->data->data[i] = radius * cosf(angle);
        if (i + 1 < value->data->size) value->data->data[i + 1] = radius * sinf(angle);
    }
    return value;
}

void cpx_ai_set(void* handle, i32 index, double number) {
    AiValue* value = (AiValue*)handle;
    if (value && index >= 0 && (size_t)index < value->data->size)
        value->data->data[index] = (f32)number;
}

double cpx_ai_get(void* handle, i32 index) {
    AiValue* value = (AiValue*)handle;
    return value && index >= 0 && (size_t)index < value->data->size ? value->data->data[index] : 0.0f;
}

double cpx_ai_grad(void* handle, i32 index) {
    AiValue* value = (AiValue*)handle;
    return value && value->grad && index >= 0 && (size_t)index < value->grad->size
        ? value->grad->data[index] : 0.0f;
}

i32 cpx_ai_is_near(void* handle, i32 index, double expected, double tolerance) {
    return fabs(cpx_ai_get(handle, index) - expected) <= tolerance ? 1 : 0;
}

void* cpx_ai_add(void* a, void* b) { return ai_add((AiValue*)a, (AiValue*)b); }
void* cpx_ai_sub(void* a, void* b) { return ai_sub((AiValue*)a, (AiValue*)b); }
void* cpx_ai_mul(void* a, void* b) { return ai_mul((AiValue*)a, (AiValue*)b); }
void* cpx_ai_matmul(void* a, void* b) { return ai_matmul((AiValue*)a, (AiValue*)b); }
void* cpx_ai_relu(void* x) { return ai_relu((AiValue*)x); }
void* cpx_ai_sigmoid(void* x) { return ai_sigmoid((AiValue*)x); }
void* cpx_ai_mean(void* x) { return ai_mean((AiValue*)x); }
void* cpx_ai_mse(void* a, void* b) { return ai_mse_loss((AiValue*)a, (AiValue*)b); }
void* cpx_ai_embedding(void* ids, void* table) { return ai_embedding((AiValue*)ids, (AiValue*)table); }
void* cpx_ai_layer_norm(void* value, void* gamma, void* beta, double epsilon) {
    return ai_layer_norm((AiValue*)value, (AiValue*)gamma, (AiValue*)beta, (f32)epsilon);
}
void* cpx_ai_causal_attention(void* query, void* key, void* value,
                              i32 heads, i32 sequence_length) {
    return ai_causal_attention((AiValue*)query, (AiValue*)key, (AiValue*)value,
                               heads, sequence_length);
}
void* cpx_ai_cross_entropy(void* logits, void* targets) {
    return ai_cross_entropy((AiValue*)logits, (AiValue*)targets);
}
i32 cpx_ai_backward(void* loss) { return ai_backward((AiValue*)loss) ? 1 : 0; }
void cpx_ai_sgd(void* parameter, double learning_rate) {
    AiValue* value = (AiValue*)parameter;
    ai_graph_sgd_step(&value, 1, (f32)learning_rate);
}
void cpx_ai_zero_grad(void* graph) { ai_graph_zero_grad((AiGraph*)graph); }
void cpx_ai_next_step(void* graph) { ai_graph_next_step((AiGraph*)graph); }

typedef struct CpxAiOptimizer {
    AiOptimizer* implementation;
    AiValue** parameters;
    i32 count;
} CpxAiOptimizer;

void* cpx_ai_adamw_create(i32 parameter_count, double learning_rate,
                          double beta1, double beta2, double weight_decay) {
    if (parameter_count <= 0) return NULL;
    CpxAiOptimizer* handle = (CpxAiOptimizer*)calloc(1, sizeof(*handle));
    if (!handle) return NULL;
    AiOptimizerConfig config = ai_optimizer_default_config(AI_OPTIMIZER_ADAM,
                                                            (f32)learning_rate);
    config.beta1 = (f32)beta1; config.beta2 = (f32)beta2;
    config.weight_decay = (f32)weight_decay;
    handle->implementation = ai_optimizer_create(AI_OPTIMIZER_ADAM, parameter_count, config);
    handle->parameters = (AiValue**)calloc((size_t)parameter_count, sizeof(AiValue*));
    handle->count = parameter_count;
    if (!handle->implementation || !handle->parameters) {
        cpx_ai_adamw_destroy(handle); return NULL;
    }
    return handle;
}

i32 cpx_ai_adamw_add(void* opaque, i32 index, void* parameter) {
    CpxAiOptimizer* handle = (CpxAiOptimizer*)opaque;
    AiValue* value = (AiValue*)parameter;
    if (!handle || !value || !value->requires_grad || index < 0 || index >= handle->count)
        return 0;
    handle->parameters[index] = value;
    return ai_optimizer_init_state(handle->implementation, index, value->data->ndim,
                                   value->data->shape) == AI_TRAIN_OK;
}

i32 cpx_ai_adamw_step(void* opaque) {
    CpxAiOptimizer* handle = (CpxAiOptimizer*)opaque;
    if (!handle) return 0;
    Tensor** parameters = (Tensor**)calloc((size_t)handle->count, sizeof(Tensor*));
    Tensor** gradients = (Tensor**)calloc((size_t)handle->count, sizeof(Tensor*));
    if (!parameters || !gradients) { free(parameters); free(gradients); return 0; }
    for (i32 i = 0; i < handle->count; ++i) {
        if (!handle->parameters[i] || !handle->parameters[i]->grad) {
            free(parameters); free(gradients); return 0;
        }
        parameters[i] = handle->parameters[i]->data;
        gradients[i] = handle->parameters[i]->grad;
    }
    i32 ok = ai_optimizer_step(handle->implementation, parameters, gradients) == AI_TRAIN_OK;
    free(parameters); free(gradients); return ok;
}

void cpx_ai_adamw_set_lr(void* opaque, double learning_rate) {
    CpxAiOptimizer* handle = (CpxAiOptimizer*)opaque;
    if (handle && learning_rate >= 0.0)
        handle->implementation->config.learning_rate = (f32)learning_rate;
}

void cpx_ai_adamw_destroy(void* opaque) {
    CpxAiOptimizer* handle = (CpxAiOptimizer*)opaque;
    if (!handle) return;
    ai_optimizer_destroy(handle->implementation);
    free(handle->parameters); free(handle);
}
