/**
 * Pooling Layers (Max and Average)
 */

#ifndef NN_POOL_H
#define NN_POOL_H

#include "../../llm/tensor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POOL_MAX,
    POOL_AVG
} PoolType;

typedef struct {
    PoolType type;
    int kernel_h, kernel_w;
    int stride_h, stride_w;
    int pad_h, pad_w;
    
    /* For max pool backward */
    Tensor* indices;
} PoolLayer;

/**
 * Create pooling layer
 */
PoolLayer* pool_create(PoolType type, int kernel_h, int kernel_w,
                       int stride_h, int stride_w,
                       int pad_h, int pad_w);

/**
 * Forward pass
 * @param input [batch, channels, height, width]
 * @param output [batch, channels, out_height, out_width]
 */
void pool_forward(PoolLayer* layer, const Tensor* input, Tensor* output);

/**
 * Backward pass
 */
void pool_backward(PoolLayer* layer, const Tensor* grad_output, Tensor* grad_input);

/**
 * Free layer
 */
void pool_free(Pool Layer* layer);

/**
 * Calculate output size
 */
void pool_output_size(int in_h, int in_w, int kernel_h, int kernel_w,
                     int stride_h, int stride_w, int pad_h, int pad_w,
                     int* out_h, int* out_w);

#ifdef __cplusplus
}
#endif

#endif /* NN_POOL_H */
