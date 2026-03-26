/**
 * Sequential Model - Container for layer stack
 */

#ifndef NN_SEQUENTIAL_H
#define NN_SEQUENTIAL_H

#include "../../llm/tensor.h"
#include "../layers/dense.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LAYER_DENSE,
    LAYER_CONV2D,
    LAYER_POOL,
    LAYER_ACTIVATION
} LayerType;

typedef struct {
    void** layers;
    LayerType* types;
    int num_layers;
    int capacity;
} SequentialModel;

/**
 * Create sequential model
 */
SequentialModel* sequential_create(void);

/**
 * Add layer to model
 */
void sequential_add_dense(SequentialModel* model, DenseLayer* layer);
void sequential_add_activation(SequentialModel* model, const char* activation);

/**
 * Forward pass through all layers
 * @param input Input tensor
 * @param output Output tensor (allocated by caller)
 * @param intermediates Array to store intermediate activations (optional)
 */
void sequential_forward(SequentialModel* model, const Tensor* input, 
                       Tensor* output, Tensor** intermediates);

/**
 * Collect all parameters for optimization
 * @param params Output array of parameter tensors
 * @param grads Output array of gradient tensors
 * @return Number of parameters
 */
int sequential_get_parameters(SequentialModel* model, Tensor*** params, Tensor*** grads);

/**
 * Print model summary
 */
void sequential_summary(SequentialModel* model);

/**
 * Free model
 */
void sequential_free(SequentialModel* model);

#ifdef __cplusplus
}
#endif

#endif /* NN_SEQUENTIAL_H */
