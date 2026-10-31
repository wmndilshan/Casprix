#ifndef CPX_AI_OPTIMIZER_H
#define CPX_AI_OPTIMIZER_H

#include "training.h"

typedef enum AiOptimizerKind {
    AI_OPTIMIZER_SGD = 0,
    AI_OPTIMIZER_ADAM,
    AI_OPTIMIZER_RMSPROP,
    AI_OPTIMIZER_ADAGRAD
} AiOptimizerKind;

typedef struct AiOptimizerConfig {
    float learning_rate;
    float beta1;       /* Adam beta1, SGD momentum, or RMSProp alpha. */
    float beta2;       /* Adam beta2. */
    float epsilon;
    float weight_decay;
    bool centered;     /* Centered RMSProp. */
    bool amsgrad;      /* AMSGrad Adam. */
} AiOptimizerConfig;

typedef struct AiOptimizer {
    AiOptimizerKind kind;
    AiOptimizerConfig config;
    Tensor** first;
    Tensor** second;
    Tensor** maximum;
    int parameter_count;
    int step;
} AiOptimizer;

AiOptimizerConfig ai_optimizer_default_config(AiOptimizerKind kind, float learning_rate);
AiOptimizer* ai_optimizer_create(AiOptimizerKind kind, int parameter_count,
                                 AiOptimizerConfig config);
AiTrainStatus ai_optimizer_init_state(AiOptimizer* optimizer, int index,
                                      i32 ndim, const i32* shape);
AiTrainStatus ai_optimizer_init(AiOptimizer* optimizer, Tensor* const* parameters);
AiTrainStatus ai_optimizer_step(AiOptimizer* optimizer, Tensor* const* parameters,
                                Tensor* const* gradients);
void ai_optimizer_zero_grad(Tensor* const* gradients, int count);
void ai_optimizer_destroy(AiOptimizer* optimizer);

#endif
