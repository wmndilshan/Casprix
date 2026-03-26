/**
 * SGD Optimizer with Momentum
 */

#ifndef NN_SGD_H
#define NN_SGD_H

#include "../../llm/tensor.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float learning_rate;
    float momentum;
    float weight_decay;
    float dampening;
    bool nesterov;
    
    /* Velocity buffers (one per parameter) */
    Tensor** velocities;
    int num_params;
} SGDOptimizer;

/**
 * Create SGD optimizer
 */
SGDOptimizer* sgd_create(float lr, float momentum, float weight_decay, bool nesterov);

/**
 * Initialize optimizer for model parameters
 */
void sgd_init(SGDOptimizer* opt, Tensor** params, int num_params);

/**
 * Perform optimization step
 * @param opt Optimizer
 * @param params Model parameters
 * @param grads Parameter gradients
 */
void sgd_step(SGDOptimizer* opt, Tensor** params, Tensor** grads);

/**
 * Zero gradients
 */
void sgd_zero_grad(Tensor** grads, int num_params);

/**
 * Free optimizer
 */
void sgd_free(SGDOptimizer* opt);

#ifdef __cplusplus
}
#endif

#endif /* NN_SGD_H */
