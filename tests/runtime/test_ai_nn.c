#include "nn/nn.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int nearly_equal(float lhs, float rhs) {
    return fabsf(lhs - rhs) < 1.0e-5f;
}

int main(void) {
    const i32 input_shape[] = {2, 2};
    const i32 output_shape[] = {2, 1};
    const float input_data[] = {1.0f, 2.0f, 3.0f, 4.0f};
    const float target_data[] = {3.0f, 7.0f};

    Tensor* input = tensor_create_from_data(2, input_shape, input_data);
    Tensor* output = tensor_create(2, output_shape);
    Tensor* target = tensor_create_from_data(2, output_shape, target_data);
    Tensor* grad_output = tensor_create(2, output_shape);
    Tensor* grad_input = tensor_create(2, input_shape);
    DenseLayer* dense = dense_create(2, 1, true);
    assert(input && output && target && grad_output && grad_input && dense);

    dense->weights->data[0] = 1.0f;
    dense->weights->data[1] = 1.0f;
    dense->bias->data[0] = 0.0f;
    dense_forward(dense, input, output);
    assert(nearly_equal(output->data[0], 3.0f));
    assert(nearly_equal(output->data[1], 7.0f));
    assert(nearly_equal(mse_loss(output, target), 0.0f));

    output->data[0] += 1.0f;
    output->data[1] += 1.0f;
    mse_backward(output, target, grad_output);
    dense_backward(dense, grad_output, grad_input);
    assert(nearly_equal(dense->grad_weights->data[0], 4.0f));
    assert(nearly_equal(dense->grad_weights->data[1], 6.0f));
    assert(nearly_equal(dense->grad_bias->data[0], 2.0f));

    Tensor* parameters[] = {dense->weights, dense->bias};
    Tensor* gradients[] = {dense->grad_weights, dense->grad_bias};
    AiOptimizerConfig config = ai_optimizer_default_config(AI_OPTIMIZER_SGD, 0.1f);
    AiOptimizer* optimizer = ai_optimizer_create(AI_OPTIMIZER_SGD, 2, config);
    assert(optimizer);
    assert(ai_optimizer_init(optimizer, parameters) == AI_TRAIN_OK);
    assert(ai_optimizer_step(optimizer, parameters, gradients) == AI_TRAIN_OK);
    assert(nearly_equal(dense->weights->data[0], 0.6f));
    assert(nearly_equal(dense->weights->data[1], 0.4f));
    assert(nearly_equal(dense->bias->data[0], -0.2f));
    assert(strcmp(nn_version(), "1.0.0") == 0);

    ai_optimizer_destroy(optimizer);
    dense_free(dense);
    tensor_destroy(grad_input);
    tensor_destroy(grad_output);
    tensor_destroy(target);
    tensor_destroy(output);
    tensor_destroy(input);
    puts("AI neural-network integration test passed");
    return 0;
}
