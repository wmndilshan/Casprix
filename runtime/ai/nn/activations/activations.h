/**
 * Activation Functions
 * 
 * Provides forward and backward passes for all standard activations
 */

#ifndef NN_ACTIVATIONS_H
#define NN_ACTIVATIONS_H

#include "../../llm/tensor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Activation Functions (Forward)
 * ======================================================================== */

/**
 * Sigmoid: y = 1 / (1 + exp(-x))
 */
void sigmoid_forward(const Tensor* input, Tensor* output);

/**
 * Tanh: y = tanh(x)
 */
void tanh_forward(const Tensor* input, Tensor* output);

/**
 * Leaky ReLU: y = x if x > 0 else alpha * x
 */
void leaky_relu_forward(const Tensor* input, Tensor* output, float alpha);

/**
 * ELU: y = x if x > 0 else alpha * (exp(x) - 1)
 */
void elu_forward(const Tensor* input, Tensor* output, float alpha);

/**
 * Softplus: y = log(1 + exp(x))
 */
void softplus_forward(const Tensor* input, Tensor* output);

/* ========================================================================
 * Activation Functions (Backward)
 * ======================================================================== */

/**
 * Sigmoid backward: grad_input = grad_output * sigmoid(x) * (1 - sigmoid(x))
 */
void sigmoid_backward(const Tensor* grad_output, const Tensor* input, Tensor* grad_input);

/**
 * Tanh backward: grad_input = grad_output * (1 - tanh(x)^2)
 */
void tanh_backward(const Tensor* grad_output, const Tensor* output, Tensor* grad_input);

/**
 * Leaky ReLU backward
 */
void leaky_relu_backward(const Tensor* grad_output, const Tensor* input, 
                        Tensor* grad_input, float alpha);

/**
 * ELU backward
 */
void elu_backward(const Tensor* grad_output, const Tensor* input, 
                 const Tensor* output, Tensor* grad_input, float alpha);

/* ========================================================================
 * Convenience Functions
 * ======================================================================== */

/**
 * Apply activation in-place
 */
void activation_apply(Tensor* tensor, const char* activation_type);

#ifdef __cplusplus
}
#endif

#endif /* NN_ACTIVATIONS_H */
