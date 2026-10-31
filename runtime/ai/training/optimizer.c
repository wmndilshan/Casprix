#include "optimizer.h"

#include <math.h>
#include <stdlib.h>

static bool tensor_pair_valid(const Tensor* parameter, const Tensor* gradient) {
    if (!parameter || !gradient || !parameter->data || !gradient->data ||
        parameter->ndim != gradient->ndim || parameter->size != gradient->size ||
        !tensor_is_contiguous(parameter) || !tensor_is_contiguous(gradient)) return false;
    for (i32 d = 0; d < parameter->ndim; d++)
        if (parameter->shape[d] != gradient->shape[d]) return false;
    return true;
}

AiOptimizerConfig ai_optimizer_default_config(AiOptimizerKind kind, float learning_rate) {
    AiOptimizerConfig config = { learning_rate, 0.0f, 0.0f, 1e-8f, 0.0f, false, false };
    if (kind == AI_OPTIMIZER_ADAM) { config.beta1 = 0.9f; config.beta2 = 0.999f; }
    else if (kind == AI_OPTIMIZER_RMSPROP) config.beta1 = 0.99f;
    return config;
}

AiOptimizer* ai_optimizer_create(AiOptimizerKind kind, int parameter_count,
                                 AiOptimizerConfig config) {
    if (kind < AI_OPTIMIZER_SGD || kind > AI_OPTIMIZER_ADAGRAD || parameter_count <= 0 ||
        config.learning_rate < 0.0f || config.epsilon <= 0.0f) return NULL;
    AiOptimizer* optimizer = (AiOptimizer*)calloc(1, sizeof(*optimizer));
    if (!optimizer) return NULL;
    optimizer->kind = kind;
    optimizer->config = config;
    optimizer->parameter_count = parameter_count;
    optimizer->first = (Tensor**)calloc((size_t)parameter_count, sizeof(Tensor*));
    optimizer->second = (Tensor**)calloc((size_t)parameter_count, sizeof(Tensor*));
    optimizer->maximum = (Tensor**)calloc((size_t)parameter_count, sizeof(Tensor*));
    if (!optimizer->first || !optimizer->second || !optimizer->maximum) {
        ai_optimizer_destroy(optimizer);
        return NULL;
    }
    return optimizer;
}

AiTrainStatus ai_optimizer_init_state(AiOptimizer* optimizer, int index,
                                      i32 ndim, const i32* shape) {
    if (!optimizer || index < 0 || index >= optimizer->parameter_count)
        return AI_TRAIN_INVALID_ARGUMENT;
    tensor_destroy(optimizer->first[index]);
    tensor_destroy(optimizer->second[index]);
    tensor_destroy(optimizer->maximum[index]);
    optimizer->first[index] = optimizer->second[index] = optimizer->maximum[index] = NULL;

    bool needs_first = optimizer->kind == AI_OPTIMIZER_ADAM ||
                       (optimizer->kind == AI_OPTIMIZER_SGD && optimizer->config.beta1 != 0.0f) ||
                       (optimizer->kind == AI_OPTIMIZER_RMSPROP && optimizer->config.centered);
    bool needs_second = optimizer->kind == AI_OPTIMIZER_ADAM ||
                        optimizer->kind == AI_OPTIMIZER_RMSPROP ||
                        optimizer->kind == AI_OPTIMIZER_ADAGRAD;
    if (needs_first) optimizer->first[index] = tensor_create(ndim, shape);
    if (needs_second) optimizer->second[index] = tensor_create(ndim, shape);
    if (optimizer->kind == AI_OPTIMIZER_ADAM && optimizer->config.amsgrad)
        optimizer->maximum[index] = tensor_create(ndim, shape);
    if ((needs_first && !optimizer->first[index]) ||
        (needs_second && !optimizer->second[index]) ||
        (optimizer->config.amsgrad && !optimizer->maximum[index]))
        return AI_TRAIN_INVALID_ARGUMENT;
    return AI_TRAIN_OK;
}

AiTrainStatus ai_optimizer_init(AiOptimizer* optimizer, Tensor* const* parameters) {
    if (!optimizer || !parameters) return AI_TRAIN_INVALID_ARGUMENT;
    for (int i = 0; i < optimizer->parameter_count; i++) {
        if (!parameters[i]) return AI_TRAIN_INVALID_ARGUMENT;
        AiTrainStatus status = ai_optimizer_init_state(optimizer, i, parameters[i]->ndim,
                                                       parameters[i]->shape);
        if (status != AI_TRAIN_OK) return status;
    }
    return AI_TRAIN_OK;
}

AiTrainStatus ai_optimizer_step(AiOptimizer* optimizer, Tensor* const* parameters,
                                Tensor* const* gradients) {
    if (!optimizer || !parameters || !gradients) return AI_TRAIN_INVALID_ARGUMENT;
    for (int p = 0; p < optimizer->parameter_count; p++) {
        if (!tensor_pair_valid(parameters[p], gradients[p])) return AI_TRAIN_SHAPE_MISMATCH;
        for (size_t i = 0; i < parameters[p]->size; i++)
            if (!isfinite(parameters[p]->data[i]) || !isfinite(gradients[p]->data[i]))
                return AI_TRAIN_NONFINITE;
    }
    optimizer->step++;
    const AiOptimizerConfig* c = &optimizer->config;
    float adam_c1 = 1.0f - powf(c->beta1, (float)optimizer->step);
    float adam_c2 = 1.0f - powf(c->beta2, (float)optimizer->step);
    for (int p = 0; p < optimizer->parameter_count; p++) {
        Tensor* parameter = parameters[p];
        Tensor* gradient = gradients[p];
        for (size_t i = 0; i < parameter->size; i++) {
            float g = gradient->data[i] + c->weight_decay * parameter->data[i];
            float update = g;
            if (optimizer->kind == AI_OPTIMIZER_ADAM) {
                float* m = &optimizer->first[p]->data[i];
                float* v = &optimizer->second[p]->data[i];
                *m = c->beta1 * *m + (1.0f - c->beta1) * g;
                *v = c->beta2 * *v + (1.0f - c->beta2) * g * g;
                float variance = *v / adam_c2;
                if (c->amsgrad) {
                    float* maximum = &optimizer->maximum[p]->data[i];
                    if (variance > *maximum) *maximum = variance;
                    variance = *maximum;
                }
                update = (*m / adam_c1) / (sqrtf(variance) + c->epsilon);
            } else if (optimizer->kind == AI_OPTIMIZER_SGD && c->beta1 != 0.0f) {
                float* velocity = &optimizer->first[p]->data[i];
                *velocity = c->beta1 * *velocity + g;
                update = *velocity;
            } else if (optimizer->kind == AI_OPTIMIZER_RMSPROP) {
                float* square = &optimizer->second[p]->data[i];
                *square = c->beta1 * *square + (1.0f - c->beta1) * g * g;
                float variance = *square;
                if (c->centered) {
                    float* mean = &optimizer->first[p]->data[i];
                    *mean = c->beta1 * *mean + (1.0f - c->beta1) * g;
                    variance -= *mean * *mean;
                    if (variance < 0.0f) variance = 0.0f;
                }
                update = g / (sqrtf(variance) + c->epsilon);
            } else if (optimizer->kind == AI_OPTIMIZER_ADAGRAD) {
                float* sum = &optimizer->second[p]->data[i];
                *sum += g * g;
                update = g / (sqrtf(*sum) + c->epsilon);
            }
            parameter->data[i] -= c->learning_rate * update;
        }
    }
    return AI_TRAIN_OK;
}

void ai_optimizer_zero_grad(Tensor* const* gradients, int count) {
    if (!gradients) return;
    for (int i = 0; i < count; i++) tensor_zeros(gradients[i]);
}

void ai_optimizer_destroy(AiOptimizer* optimizer) {
    if (!optimizer) return;
    for (int i = 0; i < optimizer->parameter_count; i++) {
        tensor_destroy(optimizer->first ? optimizer->first[i] : NULL);
        tensor_destroy(optimizer->second ? optimizer->second[i] : NULL);
        tensor_destroy(optimizer->maximum ? optimizer->maximum[i] : NULL);
    }
    free(optimizer->first); free(optimizer->second); free(optimizer->maximum);
    free(optimizer);
}
