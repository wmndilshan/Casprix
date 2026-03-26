/**
 * Batch Normalization Layer
 * 
 * Normalizes activations across batch dimension for training stability
 */

#ifndef NN_BATCHNORM_H
#define NN_BATCHNORM_H

#include "../../llm/tensor.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Learnable parameters */
    Tensor* gamma;   /* Scale [num_features] */
    Tensor* beta;    /* Shift [num_features] */
    
    /* Gradients */
    Tensor* grad_gamma;
    Tensor* grad_beta;
    
    /* Running statistics (for inference) */
    Tensor* running_mean;
    Tensor* running_var;
    float momentum;
    
    /* Configuration */
    int num_features;
    float epsilon;
    bool training;
    
    /* Cached for backward */
    Tensor* input_cache;
    Tensor* normalized_cache;
} BatchNormLayer;

/**
 * Create batch normalization layer
 * @param num_features Number of features (channels for Conv2D)
 * @param momentum Momentum for running statistics (default: 0.1)
 * @param epsilon Small constant for numerical stability (default: 1e-5)
 */
BatchNormLayer* batchnorm_create(int num_features, float momentum, float epsilon);

/**
 * Set training mode
 */
void batchnorm_set_training(BatchNormLayer* layer, bool training);

/**
 * Forward pass
 * @param input Input tensor [batch, num_features, ...]
 * @param output Output tensor (same shape as input)
 */
void batchnorm_forward(BatchNormLayer* layer, const Tensor* input, Tensor* output);

/**
 * Backward pass
 */
void batchnorm_backward(BatchNormLayer* layer, const Tensor* grad_output, Tensor* grad_input);

/**
 * Free layer
 */
void batchnorm_free(BatchNormLayer* layer);

#ifdef __cplusplus
}
#endif

#endif /* NN_BATCHNORM_H */
