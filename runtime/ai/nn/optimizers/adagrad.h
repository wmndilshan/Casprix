/**
 * AdaGrad Optimizer
 */

#ifndef NN_ADAGRAD_H
#define NN_ADAGRAD_H

#include "../../llm/tensor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float learning_rate;
    float epsilon;        /* Numerical stability (default: 1e-8) */
    float weight_decay;
    
    /* State */
    Tensor** sum_sq;      /* Accumulated squared gradients */
    int num_params;
} AdaGradOptimizer;

/**
 * Create AdaGrad optimizer
 */
AdaGradOptimizer* adagrad_create(float lr, float epsilon);

/**
 * Initialize optimizer
 */
void adagrad_init(AdaGradOptimizer* opt, Tensor** params, int num_params);

/**
 * Optimization step
 */
void adagrad_step(AdaGradOptimizer* opt, Tensor** params, Tensor** grads);

/**
 * Free optimizer
 */
void adagrad_free(AdaGradOptimizer* opt);

#ifdef __cplusplus
}
#endif

#endif /* NN_ADAGRAD_H */
