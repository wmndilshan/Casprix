/**
 * AdaGrad Implementation
 */

#include "adagrad.h"
#include <stdlib.h>
#include <math.h>

AdaGradOptimizer* adagrad_create(float lr, float epsilon) {
    AdaGradOptimizer* opt = (AdaGradOptimizer*)calloc(1, sizeof(AdaGradOptimizer));
    
    opt->learning_rate = lr;
    opt->epsilon = epsilon;
    opt->weight_decay = 0.0f;
    opt->num_params = 0;
    
    return opt;
}

void adagrad_init(AdaGradOptimizer* opt, Tensor** params, int num_params) {
    opt->num_params = num_params;
    
    /* Allocate state buffers */
    opt->sum_sq = (Tensor**)calloc(num_params, sizeof(Tensor*));
    
    for (int i = 0; i < num_params; i++) {
        opt->sum_sq[i] = tensor_create(params[i]->ndim, params[i]->shape);
        tensor_zeros(opt->sum_sq[i]);
    }
}

void adagrad_step(AdaGradOptimizer* opt, Tensor** params, Tensor** grads) {
    for (int i = 0; i < opt->num_params; i++) {
        Tensor* param = params[i];
        Tensor* grad = grads[i];
        Tensor* sum_sq = opt->sum_sq[i];
        
        float* p = param->data;
        const float* g = grad->data;
        float* s = sum_sq->data;
        int n = param->size;
        
        /* Apply weight decay */
        if (opt->weight_decay != 0.0f) {
            for (int j = 0; j < n; j++) {
                ((float*)g)[j] += opt->weight_decay * p[j];
            }
        }
        
        /* Accumulate squared gradients */
        for (int j = 0; j < n; j++) {
            s[j] += g[j] * g[j];
        }
        
        /* Update parameters */
        for (int j = 0; j < n; j++) {
            p[j] -= opt->learning_rate * g[j] / (sqrtf(s[j]) + opt->epsilon);
        }
    }
}

void adagrad_free(AdaGradOptimizer* opt) {
    if (!opt) return;
    
    for (int i = 0; i < opt->num_params; i++) {
        tensor_destroy(opt->sum_sq[i]);
    }
    
    free(opt->sum_sq);
    free(opt);
}
