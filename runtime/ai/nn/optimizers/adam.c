/**
 * Adam Optimizer Implementation
 */

#include "adam.h"
#include "../../llm/ops.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

AdamOptimizer* adam_create(float lr, float beta1, float beta2, float epsilon) {
    AdamOptimizer* opt = (AdamOptimizer*)calloc(1, sizeof(AdamOptimizer));
    
    opt->learning_rate = lr;
    opt->beta1 = beta1;
    opt->beta2 = beta2;
    opt->epsilon = epsilon;
    opt->weight_decay = 0.0f;
    opt->amsgrad = false;
    opt->timestep = 0;
    opt->num_params = 0;
    
    return opt;
}

void adam_init(AdamOptimizer* opt, Tensor** params, int num_params) {
    opt->num_params = num_params;
    
    /* Allocate moment buffers */
    opt->m = (Tensor**)calloc(num_params, sizeof(Tensor*));
    opt->v = (Tensor**)calloc(num_params, sizeof(Tensor*));
    
    if (opt->amsgrad) {
        opt->v_max = (Tensor**)calloc(num_params, sizeof(Tensor*));
    }
    
    for (int i = 0; i < num_params; i++) {
        opt->m[i] = tensor_create(params[i]->ndim, params[i]->shape);
        opt->v[i] = tensor_create(params[i]->ndim, params[i]->shape);
        
        tensor_zeros(opt->m[i]);
        tensor_zeros(opt->v[i]);
        
        if (opt->amsgrad) {
            opt->v_max[i] = tensor_create(params[i]->ndim, params[i]->shape);
            tensor_zeros(opt->v_max[i]);
        }
    }
}

void adam_step(AdamOptimizer* opt, Tensor** params, Tensor** grads) {
    opt->timestep++;
    
    /* Bias correction terms */
    float beta1_t = 1.0f - powf(opt->beta1, opt->timestep);
    float beta2_t = 1.0f - powf(opt->beta2, opt->timestep);
    
    for (int i = 0; i < opt->num_params; i++) {
        Tensor* param = params[i];
        Tensor* grad = grads[i];
        Tensor* m = opt->m[i];
        Tensor* v = opt->v[i];
        
        float* p = param->data;
        const float* g = grad->data;
        float* mt = m->data;
        float* vt = v->data;
        int n = param->size;
        
        /* Apply weight decay if needed */
        if (opt->weight_decay != 0.0f) {
            for (int j = 0; j < n; j++) {
                ((float*)g)[j] += opt->weight_decay * p[j];
            }
        }
        
        /* Update biased first moment: m = beta1 * m + (1 - beta1) * grad */
        for (int j = 0; j < n; j++) {
            mt[j] = opt->beta1 * mt[j] + (1.0f - opt->beta1) * g[j];
        }
        
        /* Update biased second moment: v = beta2 * v + (1 - beta2) * grad^2 */
        for (int j = 0; j < n; j++) {
            vt[j] = opt->beta2 * vt[j] + (1.0f - opt->beta2) * g[j] * g[j];
        }
        
        /* Bias-corrected moments */
        /* m_hat = m / (1 - beta1^t) */
        /* v_hat = v / (1 - beta2^t) */
        
        /* Update parameters: p -= lr * m_hat / (sqrt(v_hat) + eps) */
        if (opt->amsgrad) {
            float* v_max = opt->v_max[i]->data;
            for (int j = 0; j < n; j++) {
                float m_hat = mt[j] / beta1_t;
                float v_hat = vt[j] / beta2_t;
                v_max[j] = fmaxf(v_max[j], v_hat);
                p[j] -= opt->learning_rate * m_hat / (sqrtf(v_max[j]) + opt->epsilon);
            }
        } else {
            for (int j = 0; j < n; j++) {
                float m_hat = mt[j] / beta1_t;
                float v_hat = vt[j] / beta2_t;
                p[j] -= opt->learning_rate * m_hat / (sqrtf(v_hat) + opt->epsilon);
            }
        }
    }
}

void adam_free(AdamOptimizer* opt) {
    if (!opt) return;
    
    for (int i = 0; i < opt->num_params; i++) {
        tensor_destroy(opt->m[i]);
        tensor_destroy(opt->v[i]);
        if (opt->amsgrad) {
            tensor_destroy(opt->v_max[i]);
        }
    }
    
    free(opt->m);
    free(opt->v);
    if (opt->amsgrad) {
        free(opt->v_max);
    }
    
    free(opt);
}
