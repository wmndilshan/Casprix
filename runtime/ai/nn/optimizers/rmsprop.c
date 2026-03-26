/**
 * RMSProp Implementation
 */

#include "rmsprop.h"
#include <stdlib.h>
#include <math.h>

RMSPropOptimizer* rmsprop_create(float lr, float alpha, float epsilon, bool centered) {
    RMSPropOptimizer* opt = (RMSPropOptimizer*)calloc(1, sizeof(RMSPropOptimizer));
    
    opt->learning_rate = lr;
    opt->alpha = alpha;
    opt->epsilon = epsilon;
    opt->weight_decay = 0.0f;
    opt->centered = centered;
    opt->num_params = 0;
    
    return opt;
}

void rmsprop_init(RMSPropOptimizer* opt, Tensor** params, int num_params) {
    opt->num_params = num_params;
    
    /* Allocate state buffers */
    opt->v = (Tensor**)calloc(num_params, sizeof(Tensor*));
    if (opt->centered) {
        opt->g = (Tensor**)calloc(num_params, sizeof(Tensor*));
    }
    
    for (int i = 0; i < num_params; i++) {
        opt->v[i] = tensor_create(params[i]->ndim, params[i]->shape);
        tensor_zeros(opt->v[i]);
        
        if (opt->centered) {
            opt->g[i] = tensor_create(params[i]->ndim, params[i]->shape);
            tensor_zeros(opt->g[i]);
        }
    }
}

void rmsprop_step(RMSPropOptimizer* opt, Tensor** params, Tensor** grads) {
    for (int i = 0; i < opt->num_params; i++) {
        Tensor* param = params[i];
        Tensor* grad = grads[i];
        Tensor* v = opt->v[i];
        
        float* p = param->data;
        const float* g = grad->data;
        float* vt = v->data;
        int n = param->size;
        
        /* Apply weight decay */
        if (opt->weight_decay != 0.0f) {
            for (int j = 0; j < n; j++) {
                ((float*)g)[j] += opt->weight_decay * p[j];
            }
        }
        
        if (opt->centered) {
            /* Centered RMSProp */
            Tensor* g_avg = opt->g[i];
            float* gt = g_avg->data;
            
            /* Update running averages */
            for (int j = 0; j < n; j++) {
                gt[j] = opt->alpha * gt[j] + (1.0f - opt->alpha) * g[j];
                vt[j] = opt->alpha * vt[j] + (1.0f - opt->alpha) * g[j] * g[j];
            }
            
            /* Update parameters */
            for (int j = 0; j < n; j++) {
                float variance = vt[j] - gt[j] * gt[j];
                p[j] -= opt->learning_rate * g[j] / (sqrtf(variance) + opt->epsilon);
            }
        } else {
            /* Standard RMSProp */
            /* v = alpha * v + (1 - alpha) * grad^2 */
            for (int j = 0; j < n; j++) {
                vt[j] = opt->alpha * vt[j] + (1.0f - opt->alpha) * g[j] * g[j];
            }
            
            /* param -= lr * grad / (sqrt(v) + eps) */
            for (int j = 0; j < n; j++) {
                p[j] -= opt->learning_rate * g[j] / (sqrtf(vt[j]) + opt->epsilon);
            }
        }
    }
}

void rmsprop_free(RMSPropOptimizer* opt) {
    if (!opt) return;
    
    for (int i = 0; i < opt->num_params; i++) {
        tensor_destroy(opt->v[i]);
        if (opt->centered && opt->g) {
            tensor_destroy(opt->g[i]);
        }
    }
    
    free(opt->v);
    if (opt->g) {
        free(opt->g);
    }
    
    free(opt);
}
