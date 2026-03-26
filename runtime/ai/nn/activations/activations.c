/**
 * Activation Functions Implementation
 */

#include "activations.h"
#include <math.h>
#include <string.h>

/* ========================================================================
 * Forward Pass Implementations
 * ======================================================================== */

void sigmoid_forward(const Tensor* input, Tensor* output) {
    int n = input->size;
    const float* x = input->data;
    float* y = output->data;
    
    for (int i = 0; i < n; i++) {
        y[i] = 1.0f / (1.0f + expf(-x[i]));
    }
}

void tanh_forward(const Tensor* input, Tensor* output) {
    int n = input->size;
    const float* x = input->data;
    float* y = output->data;
    
    for (int i = 0; i < n; i++) {
        y[i] = tanhf(x[i]);
    }
}

void leaky_relu_forward(const Tensor* input, Tensor* output, float alpha) {
    int n = input->size;
    const float* x = input->data;
    float* y = output->data;
    
    for (int i = 0; i < n; i++) {
        y[i] = x[i] > 0.0f ? x[i] : alpha * x[i];
    }
}

void elu_forward(const Tensor* input, Tensor* output, float alpha) {
    int n = input->size;
    const float* x = input->data;
    float* y = output->data;
    
    for (int i = 0; i < n; i++) {
        y[i] = x[i] > 0.0f ? x[i] : alpha * (expf(x[i]) - 1.0f);
    }
}

void softplus_forward(const Tensor* input, Tensor* output) {
    int n = input->size;
    const float* x = input->data;
    float* y = output->data;
    
    for (int i = 0; i < n; i++) {
        /* Numerically stable softplus */
        if (x[i] > 20.0f) {
            y[i] = x[i];  /* log(1 + exp(x)) ≈ x for large x */
        } else {
            y[i] = logf(1.0f + expf(x[i]));
        }
    }
}

/* ========================================================================
 * Backward Pass Implementations
 * ======================================================================== */

void sigmoid_backward(const Tensor* grad_output, const Tensor* input, Tensor* grad_input) {
    int n = input->size;
    const float* grad_out = grad_output->data;
    const float* x = input->data;
    float* grad_in = grad_input->data;
    
    for (int i = 0; i < n; i++) {
        float sig = 1.0f / (1.0f + expf(-x[i]));
        grad_in[i] = grad_out[i] * sig * (1.0f - sig);
    }
}

void tanh_backward(const Tensor* grad_output, const Tensor* output, Tensor* grad_input) {
    int n = output->size;
    const float* grad_out = grad_output->data;
    const float* y = output->data;  /* tanh(x) from forward pass */
    float* grad_in = grad_input->data;
    
    for (int i = 0; i < n; i++) {
        grad_in[i] = grad_out[i] * (1.0f - y[i] * y[i]);
    }
}

void leaky_relu_backward(const Tensor* grad_output, const Tensor* input, 
                        Tensor* grad_input, float alpha) {
    int n = input->size;
    const float* grad_out = grad_output->data;
    const float* x = input->data;
    float* grad_in = grad_input->data;
    
    for (int i = 0; i < n; i++) {
        grad_in[i] = grad_out[i] * (x[i] > 0.0f ? 1.0f : alpha);
    }
}

void elu_backward(const Tensor* grad_output, const Tensor* input, 
                 const Tensor* output, Tensor* grad_input, float alpha) {
    int n = input->size;
    const float* grad_out = grad_output->data;
    const float* x = input->data;
    const float* y = output->data;
    float* grad_in = grad_input->data;
    
    for (int i = 0; i < n; i++) {
        grad_in[i] = grad_out[i] * (x[i] > 0.0f ? 1.0f : y[i] + alpha);
    }
}

/* ========================================================================
 * Convenience Functions
 * ======================================================================== */

void activation_apply(Tensor* tensor, const char* activation_type) {
    if (strcmp(activation_type, "sigmoid") == 0) {
        sigmoid_forward(tensor, tensor);
    }
    else if (strcmp(activation_type, "tanh") == 0) {
        tanh_forward(tensor, tensor);
    }
    else if (strcmp(activation_type, "leaky_relu") == 0) {
        leaky_relu_forward(tensor, tensor, 0.01f);
    }
    else if (strcmp(activation_type, "elu") == 0) {
        elu_forward(tensor, tensor, 1.0f);
    }
    else if (strcmp(activation_type, "softplus") == 0) {
        softplus_forward(tensor, tensor);
    }
    /* relu, gelu, silu already in ops.h */
}
