#include "training.h"

#include <math.h>

static bool compatible(const Tensor* a, const Tensor* b) {
    if (!a || !b || !a->data || !b->data || a->ndim != b->ndim || a->size != b->size)
        return false;
    for (i32 d = 0; d < a->ndim; d++) if (a->shape[d] != b->shape[d]) return false;
    return tensor_is_contiguous(a) && tensor_is_contiguous(b);
}

AiTrainStatus ai_sgd_step(Tensor* const* parameters, Tensor* const* gradients,
                          int count, float learning_rate, float weight_decay) {
    if (!parameters || !gradients || count <= 0 || learning_rate < 0.0f ||
        weight_decay < 0.0f || !isfinite(learning_rate) || !isfinite(weight_decay))
        return AI_TRAIN_INVALID_ARGUMENT;
    for (int p = 0; p < count; p++)
        if (!compatible(parameters[p], gradients[p])) return AI_TRAIN_SHAPE_MISMATCH;
    /* Validate the complete update first so an error cannot leave a partially
     * updated parameter set. */
    for (int p = 0; p < count; p++)
        for (size_t i = 0; i < parameters[p]->size; i++) {
            float update = gradients[p]->data[i] + weight_decay * parameters[p]->data[i];
            if (!isfinite(parameters[p]->data[i]) || !isfinite(update))
                return AI_TRAIN_NONFINITE;
        }
    for (int p = 0; p < count; p++) {
        Tensor* parameter = parameters[p];
        Tensor* gradient = gradients[p];
        for (size_t i = 0; i < parameter->size; i++) {
            float update = gradient->data[i] + weight_decay * parameter->data[i];
            parameter->data[i] -= learning_rate * update;
        }
    }
    return AI_TRAIN_OK;
}

AiTrainStatus ai_clip_grad_norm(Tensor* const* gradients, int count,
                                float max_norm, float* original_norm) {
    if (!gradients || count <= 0 || max_norm <= 0.0f || !isfinite(max_norm))
        return AI_TRAIN_INVALID_ARGUMENT;
    double sum = 0.0;
    for (int p = 0; p < count; p++) {
        if (!gradients[p] || !gradients[p]->data || !tensor_is_contiguous(gradients[p]))
            return AI_TRAIN_INVALID_ARGUMENT;
        for (size_t i = 0; i < gradients[p]->size; i++) {
            double value = gradients[p]->data[i];
            if (!isfinite(value)) return AI_TRAIN_NONFINITE;
            sum += value * value;
        }
    }
    if (!isfinite(sum)) return AI_TRAIN_NONFINITE;
    float norm = (float)sqrt(sum);
    if (original_norm) *original_norm = norm;
    if (norm > max_norm) {
        float scale = max_norm / (norm + 1e-12f);
        for (int p = 0; p < count; p++)
            for (size_t i = 0; i < gradients[p]->size; i++) gradients[p]->data[i] *= scale;
    }
    return AI_TRAIN_OK;
}

float ai_lr_cosine(float initial_lr, float minimum_lr, int step, int total_steps) {
    if (total_steps <= 0 || step <= 0) return initial_lr;
    if (step >= total_steps) return minimum_lr;
    const double pi = 3.14159265358979323846;
    double ratio = (double)step / total_steps;
    return minimum_lr + (initial_lr - minimum_lr) * (float)(0.5 * (1.0 + cos(pi * ratio)));
}

void ai_early_stopping_init(AiEarlyStopping* state, int patience, float min_delta) {
    if (!state) return;
    state->best = INFINITY;
    state->min_delta = min_delta < 0.0f ? 0.0f : min_delta;
    state->patience = patience < 1 ? 1 : patience;
    state->stale_epochs = 0;
    state->initialized = false;
}

bool ai_early_stopping_update(AiEarlyStopping* state, float metric) {
    if (!state || !isfinite(metric)) return true;
    if (!state->initialized || metric < state->best - state->min_delta) {
        state->best = metric;
        state->stale_epochs = 0;
        state->initialized = true;
        return false;
    }
    return ++state->stale_epochs >= state->patience;
}
