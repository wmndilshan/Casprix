/**
 * Adam Optimizer
 */

#ifndef NN_ADAM_H
#define NN_ADAM_H

#include "../../llm/tensor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float learning_rate;
    float beta1;            /* First moment decay (default: 0.9) */
    float beta2;            /* Second moment decay (default: 0.999) */
    float epsilon;          /* Numerical stability (default: 1e-8) */
    float weight_decay;     /* L2  penalty (default: 0) */
    bool amsgrad;           /* Use AMSGrad variant */
    
    /* State */
    Tensor** m;             /* First moment estimates */
    Tensor** v;             /* Second moment estimates */
    Tensor** v_max;         /* Max of second moments (AMSGrad) */
    int timestep;           /* Current timestep */
    int num_params;
} AdamOptimizer;

/**
 * Create Adam optimizer
 */
AdamOptimizer* adam_create(float lr, float beta1, float beta2, float epsilon);

/**
 * Initialize optimizer for model parameters
 */
void adam_init(AdamOptimizer* opt, Tensor** params, int num_params);

/**
 * Perform optimization step
 */
void adam_step(AdamOptimizer* opt, Tensor** params, Tensor** grads);

/**
 * Free optimizer
 */
void adam_free(AdamOptimizer* opt);

#ifdef __cplusplus
}
#endif

#endif /* NN_ADAM_H */
