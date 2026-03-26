/**
 * Pooling Layer Implementation
 */

#include "pool.h"
#include <stdlib.h>
#include <string.h>
#include <float.h>

void pool_output_size(int in_h, int in_w, int kernel_h, int kernel_w,
                     int stride_h, int stride_w, int pad_h, int pad_w,
                     int* out_h, int* out_w) {
    *out_h = (in_h + 2 * pad_h - kernel_h) / stride_h + 1;
    *out_w = (in_w + 2 * pad_w - kernel_w) / stride_w + 1;
}

PoolLayer* pool_create(PoolType type, int kernel_h, int kernel_w,
                       int stride_h, int stride_w,
                       int pad_h, int pad_w) {
    PoolLayer* layer = (PoolLayer*)calloc(1, sizeof(PoolLayer));
    
    layer->type = type;
    layer->kernel_h = kernel_h;
    layer->kernel_w = kernel_w;
    layer->stride_h = stride_h;
    layer->stride_w = stride_w;
    layer->pad_h = pad_h;
    layer->pad_w = pad_w;
    
    return layer;
}

void pool_forward(PoolLayer* layer, const Tensor* input, Tensor* output) {
    /* input: [batch, channels, height, width] */
    /* output: [batch, channels, out_height, out_width] */
    
    int batch = input->shape[0];
    int channels = input->shape[1];
    int in_h = input->shape[2];
    int in_w = input->shape[3];
    
    int out_h, out_w;
    pool_output_size(in_h, in_w, layer->kernel_h, layer->kernel_w,
                    layer->stride_h, layer->stride_w,
                    layer->pad_h, layer->pad_w,
                    &out_h, &out_w);
    
    const float* input_data = input->data;
    float* output_data = output->data;
    
    for (int b = 0; b < batch; b++) {
        for (int c = 0; c < channels; c++) {
            const float* input_channel = input_data + (b * channels + c) * in_h * in_w;
            float* output_channel = output_data + (b * channels + c) * out_h * out_w;
            
            for (int oh = 0; oh < out_h; oh++) {
                for (int ow = 0; ow < out_w; ow++) {
                    int h_start = oh * layer->stride_h - layer->pad_h;
                    int w_start = ow * layer->stride_w - layer->pad_w;
                    int h_end = h_start + layer->kernel_h;
                    int w_end = w_start + layer->kernel_w;
                    
                    /* Clamp to valid range */
                    h_start = h_start < 0 ? 0 : h_start;
                    w_start = w_start < 0 ? 0 : w_start;
                    h_end = h_end > in_h ? in_h : h_end;
                    w_end = w_end > in_w ? in_w : w_end;
                    
                    if (layer->type == POOL_MAX) {
                        float max_val = -FLT_MAX;
                        
                        for (int h = h_start; h < h_end; h++) {
                            for (int w = w_start; w < w_end; w++) {
                                float val = input_channel[h * in_w + w];
                                if (val > max_val) {
                                    max_val = val;
                                }
                            }
                        }
                        
                        output_channel[oh * out_w + ow] = max_val;
                    }
                    else if (layer->type == POOL_AVG) {
                        float sum = 0.0f;
                        int count = 0;
                        
                        for (int h = h_start; h < h_end; h++) {
                            for (int w = w_start; w < w_end; w++) {
                                sum += input_channel[h * in_w + w];
                                count++;
                            }
                        }
                        
                        output_channel[oh * out_w + ow] = sum / count;
                    }
                }
            }
        }
    }
}

void pool_backward(PoolLayer* layer, const Tensor* grad_output, Tensor* grad_input) {
    /* Simplified - full implementation would route gradients based on max indices */
    /* For avg pool, distribute gradient equally */
    /* Omitted for brevity */
    (void)layer;
    (void)grad_output;
    (void)grad_input;
}

void pool_free(PoolLayer* layer) {
    if (!layer) return;
    
    if (layer->indices) {
        tensor_destroy(layer->indices);
    }
    
    free(layer);
}
