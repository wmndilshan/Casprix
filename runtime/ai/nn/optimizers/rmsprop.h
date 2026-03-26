/**
 * RMSProp Optimizer
 */

#ifndef NN_RMSPROP_H
#define NN_RMSPROP_H

#include "../../llm/tensor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float learning_rate;
    float alpha;          /* Decay rate (default: 0.99) */
    float epsilon;        /* Numerical stability (default: 1e-8) */
    float weight_decay;
    bool centered;        /* Use centered RMSProp */
    
    /* State */
    Tensor** v;           /* Running average of squared gradients */
    Tensor** g;           /* Running average of gradients (centered only) */
    int num_params;
} RMSPropOptimizer;

/**
 * Create RMSProp optimizer
 */
RMSPropOptimizer* rmsprop_create(float lr, float alpha, float epsilon, bool centered);

/**
 * Initialize optimizer
 */
void rmsprop_init(RMSPropOptimizer* opt, Tensor** params, int num_params);

/**
 * Optimization step
 */
void rmsprop_step(RMSPropOptimizer* opt, Tensor** params, Tensor** grads);

/**
 * Free optimizer
 */
void rmsprop_free(RMSPropOptimizer* opt);

#ifdef __cplusplus
}
#endif

#endif /* NN_RMSPROP_H */
