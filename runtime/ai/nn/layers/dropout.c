/**
 * Dropout Implementation
 */

#include "dropout.h"
#include <stdlib.h>
#include <string.h>

DropoutLayer* dropout_create(float dropout_rate) {
    DropoutLayer* layer = (DropoutLayer*)calloc(1, sizeof(DropoutLayer));
    
    layer->dropout_rate = dropout_rate;
    layer->training = true;
    layer->mask = NULL;
    
    return layer;
}

void dropout_set_training(DropoutLayer* layer, bool training) {
    layer->training = training;
}

void dropout_forward(DropoutLayer* layer, const Tensor* input, Tensor* output) {
    if (!layer->training) {
        /* Inference mode - no dropout */
        tensor_copy(input, output);
        return;
    }
    
    /* Training mode - apply dropout */
    int n = input->size;
    float scale = 1.0f / (1.0f - layer->dropout_rate);
    
    /* Create or reuse mask */
    if (!layer->mask || layer->mask->size != n) {
        if (layer->mask) tensor_destroy(layer->mask);
        layer->mask = tensor_create(input->ndim, input->shape);
    }
    
    /* Generate random mask */
    for (int i = 0; i < n; i++) {
        float rand_val = (float)rand() / RAND_MAX;
        if (rand_val < layer->dropout_rate) {
            layer->mask->data[i] = 0.0f;
            output->data[i] = 0.0f;
        } else {
            layer->mask->data[i] = scale;
            output->data[i] = input->data[i] * scale;
        }
    }
}

void dropout_backward(DropoutLayer* layer, const Tensor* grad_output, Tensor* grad_input) {
    /* Apply same mask to gradient */
    int n = grad_output->size;
    
    for (int i = 0; i < n; i++) {
        grad_input->data[i] = grad_output->data[i] * layer->mask->data[i];
    }
}

void dropout_free(DropoutLayer* layer) {
    if (!layer) return;
    
    if (layer->mask) {
        tensor_destroy(layer->mask);
    }
    
    free(layer);
}
