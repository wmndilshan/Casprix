/**
 * Dropout Layer
 * 
 * Regularization technique that randomly zeros elements during training
 */

#ifndef NN_DROPOUT_H
#define NN_DROPOUT_H

#include "../../llm/tensor.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float dropout_rate;  /* Probability of dropping (0.0 - 1.0) */
    bool training;
    
    /* Mask for backward pass */
    Tensor* mask;
} DropoutLayer;

/**
 * Create dropout layer
 * @param dropout_rate Probability of dropping elements (e.g., 0.5 for 50%)
 */
DropoutLayer* dropout_create(float dropout_rate);

/**
 * Set training mode
 */
void dropout_set_training(DropoutLayer* layer, bool training);

/**
 * Forward pass
 * During training: randomly zero elements and scale by 1/(1-p)
 * During inference: identity (no dropout)
 */
void dropout_forward(DropoutLayer* layer, const Tensor* input, Tensor* output);

/**
 * Backward pass
 */
void dropout_backward(DropoutLayer* layer, const Tensor* grad_output, Tensor* grad_input);

/**
 * Free layer
 */
void dropout_free(DropoutLayer* layer);

#ifdef __cplusplus
}
#endif

#endif /* NN_DROPOUT_H */
