/**
 * Sequential Model Implementation
 */

#include "sequential.h"
#include "../activations/activations.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

SequentialModel* sequential_create(void) {
    SequentialModel* model = (SequentialModel*)calloc(1, sizeof(SequentialModel));
    
    model->capacity = 16;
    model->layers = (void**)calloc(model->capacity, sizeof(void*));
    model->types = (LayerType*)calloc(model->capacity, sizeof(LayerType));
    model->num_layers = 0;
    
    return model;
}

static void sequential_resize(SequentialModel* model) {
    if (model->num_layers >= model->capacity) {
        model->capacity *= 2;
        model->layers = (void**)realloc(model->layers, model->capacity * sizeof(void*));
        model->types = (LayerType*)realloc(model->types, model->capacity * sizeof(LayerType));
    }
}

void sequential_add_dense(SequentialModel* model, DenseLayer* layer) {
    sequential_resize(model);
    model->layers[model->num_layers] = layer;
    model->types[model->num_layers] = LAYER_DENSE;
    model->num_layers++;
}

void sequential_add_activation(SequentialModel* model, const char* activation) {
    sequential_resize(model);
    model->layers[model->num_layers] = strdup(activation);
    model->types[model->num_layers] =LAYER_ACTIVATION;
    model->num_layers++;
}

void sequential_forward(SequentialModel* model, const Tensor* input, 
                       Tensor* output, Tensor** intermediates) {
    /* Alternate between input/output buffers */
    const Tensor* current_input = input;
    Tensor* temp = NULL;
    
    for (int i = 0; i < model->num_layers; i++) {
        LayerType type = model->types[i];
        
        /* Determine output tensor */
        Tensor* current_output = (i == model->num_layers - 1) ? output : 
                                (intermediates ? intermediates[i] : temp);
        
        if (!current_output && i < model->num_layers - 1) {
            /* Create temporary buffer */
            if (type == LAYER_DENSE) {
                DenseLayer* layer = (DenseLayer*)model->layers[i];
                int shape[] = {current_input->shape[0], layer->out_features};
                temp = tensor_create(2, shape);
                current_output = temp;
            }
        }
        
        /* Execute layer */
        if (type == LAYER_DENSE) {
            DenseLayer* layer = (DenseLayer*)model->layers[i];
            dense_forward(layer, current_input, current_output);
        }
        else if (type == LAYER_ACTIVATION) {
            const char* activation = (const char*)model->layers[i];
            /* Apply activation in-place or to output */
            tensor_copy(current_input, current_output);
            activation_apply(current_output, activation);
        }
        
        /* Move to next layer */
        current_input = current_output;
    }
    
    if (temp && !intermediates) {
        tensor_destroy(temp);
    }
}

int sequential_get_parameters(SequentialModel* model, Tensor*** params, Tensor*** grads) {
    /* Count parameters */
    int total_params = 0;
    for (int i = 0; i < model->num_layers; i++) {
        if (model->types[i] == LAYER_DENSE) {
            DenseLayer* layer = (DenseLayer*)model->layers[i];
            total_params += layer->use_bias ? 2 : 1;  /* weights + bias */
        }
    }
    
    /* Allocate arrays */
    *params = (Tensor**)calloc(total_params, sizeof(Tensor*));
    *grads = (Tensor**)calloc(total_params, sizeof(Tensor*));
    
    /* Collect parameters */
    int idx = 0;
    for (int i = 0; i < model->num_layers; i++) {
        if (model->types[i] == LAYER_DENSE) {
            DenseLayer* layer = (DenseLayer*)model->layers[i];
            (*params)[idx] = layer->weights;
            (*grads)[idx] = layer->grad_weights;
            idx++;
            
            if (layer->use_bias) {
                (*params)[idx] = layer->bias;
                (*grads)[idx] = layer->grad_bias;
                idx++;
            }
        }
    }
    
    return total_params;
}

void sequential_summary(SequentialModel* model) {
    printf("Model Summary\n");
    printf("═══════════════════════════════════════════════\n");
    
    int total_params = 0;
    
    for (int i = 0; i < model->num_layers; i++) {
        printf("Layer %d: ", i + 1);
        
        if (model->types[i] == LAYER_DENSE) {
            DenseLayer* layer = (DenseLayer*)model->layers[i];
            int params = dense_num_params(layer);
            total_params += params;
            printf("Dense(%d → %d) - %d params\n", 
                   layer->in_features, layer->out_features, params);
        }
        else if (model->types[i] == LAYER_ACTIVATION) {
            printf("Activation(%s)\n", (const char*)model->layers[i]);
        }
    }
    
    printf("═══════════════════════════════════════════════\n");
    printf("Total parameters: %d\n", total_params);
}

void sequential_free(SequentialModel* model) {
    if (!model) return;
    
    for (int i = 0; i < model->num_layers; i++) {
        if (model->types[i] == LAYER_DENSE) {
            dense_free((DenseLayer*)model->layers[i]);
        }
        else if (model->types[i] == LAYER_ACTIVATION) {
            free(model->layers[i]);
        }
    }
    
    free(model->layers);
    free(model->types);
    free(model);
}
