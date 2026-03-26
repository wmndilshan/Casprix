/**
 * 2D Convolution Layer
 * 
 * Implements Conv2D using im2col + GEMM for efficiency
 */

#ifndef NN_CONV2D_H
#define NN_CONV2D_H

#include "../../llm/tensor.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Parameters */
    Tensor* weights;      /* [out_channels, in_channels, kH, kW] */
    Tensor* bias;         /* [out_channels] */
    
    /* Gradients */
    Tensor* grad_weights;
    Tensor* grad_bias;
    
    /* Configuration */
    int in_channels;
    int out_channels;
    int kernel_h, kernel_w;
    int stride_h, stride_w;
    int pad_h, pad_w;
    bool use_bias;
    
    /* Cached for backward */
    Tensor* input_cache;
    Tensor* col_buffer;   /* im2col buffer */
} Conv2DLayer;

/**
 * Create Conv2D layer
 */
Conv2DLayer* conv2d_create(int in_channels, int out_channels,
                           int kernel_h, int kernel_w,
                           int stride_h, int stride_w,
                           int pad_h, int pad_w,
                           bool use_bias);

/**
 * Initialize weights
 */
void conv2d_init_weights(Conv2DLayer* layer, const char* init_type);

/**
 * Forward pass
 * @param input [batch, in_channels, height, width]
 * @param output [batch, out_channels, out_height, out_width]
 */
void conv2d_forward(Conv2DLayer* layer, const Tensor* input, Tensor* output);

/**
 * Backward pass
 */
void conv2d_backward(Conv2DLayer* layer, const Tensor* grad_output, Tensor* grad_input);

/**
 * Free layer
 */
void conv2d_free(Conv2DLayer* layer);

/**
 * Calculate output dimensions
 */
void conv2d_output_size(int in_h, int in_w, int kernel_h, int kernel_w,
                       int stride_h, int stride_w, int pad_h, int pad_w,
                       int* out_h, int* out_w);

#ifdef __cplusplus
}
#endif

#endif /* NN_CONV2D_H */
