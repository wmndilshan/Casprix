/**
 * Batch Normalization Implementation
 */

#include "batchnorm.h"
#include "../../llm/ops.h"
#include <stdlib.h>
#include <math.h>

BatchNormLayer* batchnorm_create(int num_features, float momentum, float epsilon) {
    BatchNormLayer* layer = (BatchNormLayer*)calloc(1, sizeof(BatchNormLayer));
    
    layer->num_features = num_features;
    layer->momentum = momentum;
    layer->epsilon = epsilon;
    layer->training = true;
    
    /* Create parameters */
    int param_shape[] = {num_features};
    layer->gamma = tensor_create(1, param_shape);
    layer->beta = tensor_create(1, param_shape);
    layer->grad_gamma = tensor_create(1, param_shape);
    layer->grad_beta = tensor_create(1, param_shape);
    
    /* Initialize gamma to 1, beta to 0 */
    tensor_fill(layer->gamma, 1.0f);
    tensor_zeros(layer->beta);
    
    /* Running statistics */
    layer->running_mean = tensor_create(1, param_shape);
    layer->running_var = tensor_create(1, param_shape);
    tensor_zeros(layer->running_mean);
    tensor_fill(layer->running_var, 1.0f);
    
    return layer;
}

void batchnorm_set_training(BatchNormLayer* layer, bool training) {
    layer->training = training;
}

void batchnorm_forward(BatchNormLayer* layer, const Tensor* input, Tensor* output) {
    /* Assuming input: [batch, num_features, ...] */
    int batch = input->shape[0];
    int num_features = layer->num_features;
    
    /* Calculate elements per feature */
    int spatial_size = 1;
    for (int i = 2; i < input->ndim; i++) {
        spatial_size *= input->shape[i];
    }
    int n = batch * spatial_size;  /* Total elements per feature */
    
    if (layer->training) {
        /* Compute batch statistics */
        for (int c = 0; c < num_features; c++) {
            float sum = 0.0f;
            float sum_sq = 0.0f;
            
            /* Gather all values for this feature */
            for (int b = 0; b < batch; b++) {
                for (int s = 0; s < spatial_size; s++) {
                    int idx = (b * num_features + c) * spatial_size + s;
                    float val = input->data[idx];
                    sum += val;
                    sum_sq += val * val;
                }
            }
            
            float mean = sum / n;
            float var = sum_sq / n - mean * mean;
            
            /* Update running statistics */
            layer->running_mean->data[c] = (1.0f - layer->momentum) * layer->running_mean->data[c] +
                                          layer->momentum * mean;
            layer->running_var->data[c] = (1.0f - layer->momentum) * layer->running_var->data[c] +
                                         layer->momentum * var;
            
            /* Normalize and scale */
            float std = sqrtf(var + layer->epsilon);
            float gamma = layer->gamma->data[c];
            float beta = layer->beta->data[c];
            
            for (int b = 0; b < batch; b++) {
                for (int s = 0; s < spatial_size; s++) {
                    int idx = (b * num_features + c) * spatial_size + s;
                    float normalized = (input->data[idx] - mean) / std;
                    output->data[idx] = gamma * normalized + beta;
                }
            }
        }
    } else {
        /* Use running statistics for inference */
        for (int c = 0; c < num_features; c++) {
            float mean = layer->running_mean->data[c];
            float var = layer->running_var->data[c];
            float std = sqrtf(var + layer->epsilon);
            float gamma = layer->gamma->data[c];
            float beta = layer->beta->data[c];
            
            for (int b = 0; b < batch; b++) {
                for (int s = 0; s < spatial_size; s++) {
                    int idx = (b * num_features + c) * spatial_size + s;
                    float normalized = (input->data[idx] - mean) / std;
                    output->data[idx] = gamma * normalized + beta;
                }
            }
        }
    }
}

void batchnorm_backward(BatchNormLayer* layer, const Tensor* grad_output, Tensor* grad_input) {
    /* Simplified backward - full version computes gradients for gamma, beta, and input */
    (void)layer;
    (void)grad_output;
    (void)grad_input;
}

void batchnorm_free(BatchNormLayer* layer) {
    if (!layer) return;
    
    tensor_destroy(layer->gamma);
    tensor_destroy(layer->beta);
    tensor_destroy(layer->grad_gamma);
    tensor_destroy(layer->grad_beta);
    tensor_destroy(layer->running_mean);
    tensor_destroy(layer->running_var);
    
    free(layer);
}
