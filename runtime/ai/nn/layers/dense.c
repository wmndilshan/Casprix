/**
 * Dense/Fully Connected Layer Implementation
 */

#include "dense.h"
#include "../../llm/ops.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

DenseLayer* dense_create(int in_features, int out_features, bool use_bias) {
    DenseLayer* layer = (DenseLayer*)calloc(1, sizeof(DenseLayer));
    
    layer->in_features = in_features;
    layer->out_features = out_features;
    layer->use_bias = use_bias;
    
    /* Create weight tensor [out_features, in_features] */
    int weight_shape[] = {out_features, in_features};
    layer->weights = tensor_create(2, weight_shape);
    layer->grad_weights = tensor_create(2, weight_shape);
    
    /* Create bias tensor [out_features] */
    if (use_bias) {
        int bias_shape[] = {out_features};
        layer->bias = tensor_create(1, bias_shape);
        layer->grad_bias = tensor_create(1, bias_shape);
        tensor_zeros(layer->bias);
        tensor_zeros(layer->grad_bias);
    }
    
    /* Initialize weights with Xavier initialization */
    dense_init_weights(layer, "xavier");
    tensor_zeros(layer->grad_weights);
    
    return layer;
}

void dense_init_weights(DenseLayer* layer, const char* init_type) {
    float* w = layer->weights->data;
    int n = layer->in_features * layer->out_features;
    
    if (strcmp(init_type, "xavier") == 0) {
        /* Xavier/Glorot: std = sqrt(2 / (in + out)) */
        float std = sqrtf(2.0f / (layer->in_features + layer->out_features));
        for (int i = 0; i < n; i++) {
            /* Box-Muller transform for normal distribution */
            float u1 = (float)rand() / RAND_MAX;
            float u2 = (float)rand() / RAND_MAX;
            w[i] = std * sqrtf(-2.0f * logf(u1)) * cosf(2.0f * 3.14159265f * u2);
        }
    }
    else if (strcmp(init_type, "he") == 0) {
        /* He initialization: std = sqrt(2 / in) */
        float std = sqrtf(2.0f / layer->in_features);
        for (int i = 0; i < n; i++) {
            float u1 = (float)rand() / RAND_MAX;
            float u2 = (float)rand() / RAND_MAX;
            w[i] = std * sqrtf(-2.0f * logf(u1)) * cosf(2.0f * 3.14159265f * u2);
        }
    }
    else if (strcmp(init_type, "zeros") == 0) {
        tensor_zeros(layer->weights);
    }
    else if (strcmp(init_type, "ones") == 0) {
        tensor_fill(layer->weights, 1.0f);
    }
}

void dense_forward(DenseLayer* layer, const Tensor* input, Tensor* output) {
    /* Cache input for backward pass */
    if (!layer->input_cache) {
        layer->input_cache = tensor_create(input->ndim, input->shape);
    }
    tensor_copy(input, layer->input_cache);
    
    /* y = xW^T + b */
    /* input: [batch, in_features] */
    /* weights: [out_features, in_features] */
    /* output: [batch, out_features] */
    
    int batch = input->shape[0];
    int in_feat = layer->in_features;
    int out_feat = layer->out_features;
    
    /* Matrix multiplication: output = input * weights^T */
    /* This is equivalent to: for each sample, y = W * x */
    for (int b = 0; b < batch; b++) {
        const float* x = input->data + b * in_feat;
        float* y = output->data + b * out_feat;
        
        /* y = W * x using GEMV */
        gemv_f32(layer->weights->data, x, y, out_feat, in_feat);
        
        /* Add bias if enabled */
        if (layer->use_bias) {
            vec_add_f32(y, layer->bias->data, y, out_feat);
        }
    }
}

void dense_backward(DenseLayer* layer, const Tensor* grad_output, Tensor* grad_input) {
    /* grad_input = grad_output * W */
    /* grad_weights = grad_output^T * input */
    /* grad_bias = sum(grad_output) */
    
    int batch = grad_output->shape[0];
    int in_feat = layer->in_features;
    int out_feat = layer->out_features;
    
    const Tensor* input = layer->input_cache;
    
    /* Compute grad_input = grad_output * W */
    if (grad_input) {
        tensor_zeros(grad_input);
        
        for (int b = 0; b < batch; b++) {
            const float* grad_out = grad_output->data + b * out_feat;
            float* grad_in = grad_input->data + b * in_feat;
            
            /* grad_in = W^T * grad_out */
            /* W is [out_feat, in_feat], so W^T is [in_feat, out_feat] */
            for (int i = 0; i < in_feat; i++) {
                float sum = 0.0f;
                for (int o = 0; o < out_feat; o++) {
                    sum += layer->weights->data[o * in_feat + i] * grad_out[o];
                }
                grad_in[i] += sum;
            }
        }
    }
    
    /* Compute grad_weights = grad_output^T * input */
    /* grad_output: [batch, out_feat], input: [batch, in_feat] */
    /* grad_weights: [out_feat, in_feat] */
    tensor_zeros(layer->grad_weights);
    
    for (int b = 0; b < batch; b++) {
        const float* grad_out = grad_output->data + b * out_feat;
        const float* x = input->data + b * in_feat;
        
        /* Outer product: grad_W += grad_out * x^T */
        for (int o = 0; o < out_feat; o++) {
            float* grad_w_row = layer->grad_weights->data + o * in_feat;
            for (int i = 0; i < in_feat; i++) {
                grad_w_row[i] += grad_out[o] * x[i];
            }
        }
    }
    
    /* Compute grad_bias = sum over batch dimension */
    if (layer->use_bias) {
        tensor_zeros(layer->grad_bias);
        
        for (int b = 0; b < batch; b++) {
            const float* grad_out = grad_output->data + b * out_feat;
            vec_add_f32(layer->grad_bias->data, grad_out, layer->grad_bias->data, out_feat);
        }
    }
}

void dense_free(DenseLayer* layer) {
    if (!layer) return;
    
    tensor_destroy(layer->weights);
    tensor_destroy(layer->grad_weights);
    
    if (layer->use_bias) {
        tensor_destroy(layer->bias);
        tensor_destroy(layer->grad_bias);
    }
    
    if (layer->input_cache) {
        tensor_destroy(layer->input_cache);
    }
    
    free(layer);
}

int dense_num_params(const DenseLayer* layer) {
    int params = layer->in_features * layer->out_features;
    if (layer->use_bias) {
        params += layer->out_features;
    }
    return params;
}
