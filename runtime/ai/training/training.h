#ifndef CPX_AI_TRAINING_H
#define CPX_AI_TRAINING_H

#include "../core/tensor.h"

typedef enum AiTrainStatus {
    AI_TRAIN_OK = 0,
    AI_TRAIN_INVALID_ARGUMENT,
    AI_TRAIN_SHAPE_MISMATCH,
    AI_TRAIN_NONFINITE
} AiTrainStatus;

typedef struct AiEarlyStopping {
    float best;
    float min_delta;
    int patience;
    int stale_epochs;
    bool initialized;
} AiEarlyStopping;

AiTrainStatus ai_sgd_step(Tensor* const* parameters, Tensor* const* gradients,
                          int count, float learning_rate, float weight_decay);
AiTrainStatus ai_clip_grad_norm(Tensor* const* gradients, int count,
                                float max_norm, float* original_norm);
float ai_lr_cosine(float initial_lr, float minimum_lr, int step, int total_steps);
void ai_early_stopping_init(AiEarlyStopping* state, int patience, float min_delta);
bool ai_early_stopping_update(AiEarlyStopping* state, float metric);

#endif
