/**
 * Dense/Fully Connected Layer
 * 
 * Implements: y = xW^T + b
 * where x: [batch, in_features], W: [out_features, in_features], b: [out_features]
 */

#ifndef NN_DENSE_H
#define NN_DENSE_H

#include "../../llm/tensor.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Parameters */
    Tensor* weights;      /* [out_features, in_features] */
    Tensor* bias;         /* [out_features] */
    
    /* Gradients */
    Tensor* grad_weights;
    Tensor* grad_bias;
    
    /* Configuration */
    int in_features;
    int out_features;
    bool use_bias;
    
    /* Cached values for backward pass */
    Tensor* input_cache;  /* Last forward input */
} DenseLayer;

/**
 * Create dense layer
 * @param in_features Input dimension
 * @param out_features Output dimension
 * @param use_bias Whether to use bias term
 */
DenseLayer* dense_create(int in_features, int out_features, bool use_bias);

/**
 * Initialize weights
 * @param layer Dense layer
 * @param init_type "xavier", "he", "zeros", "ones"
 */
void dense_init_weights(DenseLayer* layer, const char* init_type);

/**
 * Forward pass: y = xW^T + b
 * @param layer Dense layer
 * @param input Input tensor [batch, in_features]
 * @param output Output tensor [batch, out_features] (pre-allocated)
 */
void dense_forward(DenseLayer* layer, const Tensor* input, Tensor* output);

/**
 * Backward pass
 * @param layer Dense layer
 * @param grad_output Gradient from next layer [batch, out_features]
 * @param grad_input Gradient to previous layer [batch, in_features] (pre-allocated)
 */
void dense_backward(DenseLayer* layer, const Tensor* grad_output, Tensor* grad_input);

/**
 * Free layer
 */
void dense_free(DenseLayer* layer);

/**
 * Get number of parameters
 */
int dense_num_params(const DenseLayer* layer);

#ifdef __cplusplus
}
#endif

#endif /* NN_DENSE_H */
