/**
 * SGD Optimizer Implementation
 */

#include "sgd.h"
#include "../../llm/ops.h"
#include <stdlib.h>
#include <string.h>

SGDOptimizer* sgd_create(float lr, float momentum, float weight_decay, bool nesterov) {
    SGDOptimizer* opt = (SGDOptimizer*)calloc(1, sizeof(SGDOptimizer));
    
    opt->learning_rate = lr;
    opt->momentum = momentum;
    opt->weight_decay = weight_decay;
    opt->dampening = 0.0f;
    opt->nesterov = nesterov;
    opt->num_params = 0;
    opt->velocities = NULL;
    
    return opt;
}

void sgd_init(SGDOptimizer* opt, Tensor** params, int num_params) {
    opt->num_params = num_params;
    
    /* Allocate velocity buffers */
    if (opt->momentum > 0.0f) {
        opt->velocities = (Tensor**)calloc(num_params, sizeof(Tensor*));
        
        for (int i = 0; i < num_params; i++) {
            opt->velocities[i] = tensor_create(params[i]->ndim, params[i]->shape);
            tensor_zeros(opt->velocities[i]);
        }
    }
}

void sgd_step(SGDOptimizer* opt, Tensor** params, Tensor** grads) {
    for (int i = 0; i < opt->num_params; i++) {
        Tensor* param = params[i];
        Tensor* grad = grads[i];
        
        float* p = param->data;
        const float* g = grad->data;
        int n = param->size;
        
        /* Apply weight decay: grad = grad + weight_decay * param */
        if (opt->weight_decay != 0.0f) {
            for (int j = 0; j < n; j++) {
                ((float*)g)[j] += opt->weight_decay * p[j];
            }
        }
        
        /* Apply momentum */
        if (opt->momentum != 0.0f) {
            Tensor* v = opt->velocities[i];
            float* velocity = v->data;
            
            /* v = momentum * v + (1 - dampening) * grad */
            for (int j = 0; j < n; j++) {
                velocity[j] = opt->momentum * velocity[j] + (1.0f - opt->dampening) * g[j];
            }
            
            /* Nesterov momentum */
            if (opt->nesterov) {
                /* grad = grad + momentum * v */
                for (int j = 0; j < n; j++) {
                    ((float*)g)[j] += opt->momentum * velocity[j];
                }
            } else {
                /* Use velocity as gradient */
                g = velocity;
            }
        }
        
        /* Update parameters: param -= learning_rate * grad */
        for (int j = 0; j < n; j++) {
            p[j] -= opt->learning_rate * g[j];
        }
    }
}

void sgd_zero_grad(Tensor** grads, int num_params) {
    for (int i = 0; i < num_params; i++) {
        if (grads[i]) {
            tensor_zeros(grads[i]);
        }
    }
}

void sgd_free(SGDOptimizer* opt) {
    if (!opt) return;
    
    if (opt->velocities) {
        for (int i = 0; i < opt->num_params; i++) {
            tensor_destroy(opt->velocities[i]);
        }
        free(opt->velocities);
    }
    
    free(opt);
}
