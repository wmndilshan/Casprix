/**
 * Loss Functions Implementation
 */

#include "loss.h"
#include "../../llm/ops.h"
#include <math.h>
#include <string.h>

/* ========================================================================
 * Mean Squared Error
 * ======================================================================== */

float mse_loss(const Tensor* predictions, const Tensor* targets) {
    int n = predictions->size;
    const float* pred = predictions->data;
    const float* targ = targets->data;
    
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        float diff = pred[i] - targ[i];
        sum += diff * diff;
    }
    
    return sum / n;
}

void mse_backward(const Tensor* predictions, const Tensor* targets, Tensor* grad) {
    int n = predictions->size;
    const float* pred = predictions->data;
    const float* targ = targets->data;
    float* g = grad->data;
    
    float scale = 2.0f / n;
    for (int i = 0; i < n; i++) {
        g[i] = scale * (pred[i] - targ[i]);
    }
}

/* ========================================================================
 * Binary Cross-Entropy
 * ======================================================================== */

float binary_cross_entropy_loss(const Tensor* predictions, const Tensor* targets) {
    int n = predictions->size;
    const float* pred = predictions->data;
    const float* targ = targets->data;
    
    float sum = 0.0f;
    const float eps = 1e-7f;  /* Numerical stability */
    
    for (int i = 0; i < n; i++) {
        /* Clamp predictions to [eps, 1-eps] */
        float p = fmaxf(eps, fminf(1.0f - eps, pred[i]));
        sum -= targ[i] * logf(p) + (1.0f - targ[i]) * logf(1.0f - p);
    }
    
    return sum / n;
}

void binary_cross_entropy_backward(const Tensor* predictions, const Tensor* targets, 
                                  Tensor* grad) {
    int n = predictions->size;
    const float* pred = predictions->data;
    const float* targ = targets->data;
    float* g = grad->data;
    
    const float eps = 1e-7f;
    float scale = 1.0f / n;
    
    for (int i = 0; i < n; i++) {
        float p = fmaxf(eps, fminf(1.0f - eps, pred[i]));
        g[i] = scale * ((p - targ[i]) / (p * (1.0f - p)));
    }
}

/* ========================================================================
 * Categorical Cross-Entropy (with Softmax)
 * ======================================================================== */

float categorical_cross_entropy_loss(const Tensor* logits, const Tensor* targets) {
    /* logits: [batch, num_classes] */
    /* targets: [batch] - class indices as floats */
    
    int batch = logits->shape[0];
    int num_classes = logits->shape[1];
    
    const float* logit_data = logits->data;
    const float* target_data = targets->data;
    
    float total_loss = 0.0f;
    
    for (int b = 0; b < batch; b++) {
        const float* logit_row = logit_data + b * num_classes;
        int target_class = (int)target_data[b];
        
        /* Compute softmax (numerically stable) */
        float max_logit = logit_row[0];
        for (int c = 1; c < num_classes; c++) {
            if (logit_row[c] > max_logit) max_logit = logit_row[c];
        }
        
        float sum_exp = 0.0f;
        for (int c = 0; c < num_classes; c++) {
            sum_exp += expf(logit_row[c] - max_logit);
        }
        
        float log_sum_exp = max_logit + logf(sum_exp);
        total_loss -= (logit_row[target_class] - log_sum_exp);
    }
    
    return total_loss / batch;
}

void categorical_cross_entropy_backward(const Tensor* logits, const Tensor* targets, 
                                       Tensor* grad) {
    /* Fused softmax + cross-entropy gradient */
    /* grad[i] = softmax[i] - 1 if i == target else softmax[i] */
    
    int batch = logits->shape[0];
    int num_classes = logits->shape[1];
    
    const float* logit_data = logits->data;
    const float* target_data = targets->data;
    float* grad_data = grad->data;
    
    for (int b = 0; b < batch; b++) {
        const float* logit_row = logit_data + b * num_classes;
        float* grad_row = grad_data + b * num_classes;
        int target_class = (int)target_data[b];
        
        /* Compute softmax */
        float max_logit = logit_row[0];
        for (int c = 1; c < num_classes; c++) {
            if (logit_row[c] > max_logit) max_logit = logit_row[c];
        }
        
        float sum_exp = 0.0f;
        for (int c = 0; c < num_classes; c++) {
            grad_row[c] = expf(logit_row[c] - max_logit);
            sum_exp += grad_row[c];
        }
        
        /* Normalize and subtract 1 from target class */
        float scale = 1.0f / (batch * sum_exp);
        for (int c = 0; c < num_classes; c++) {
            grad_row[c] *= scale;
        }
        grad_row[target_class] -= 1.0f / batch;
    }
}

/* ========================================================================
 * L1 Loss
 * ======================================================================== */

float l1_loss(const Tensor* predictions, const Tensor* targets) {
    int n = predictions->size;
    const float* pred = predictions->data;
    const float* targ = targets->data;
    
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        sum += fabsf(pred[i] - targ[i]);
    }
    
    return sum / n;
}

void l1_backward(const Tensor* predictions, const Tensor* targets, Tensor* grad) {
    int n = predictions->size;
    const float* pred = predictions->data;
    const float* targ = targets->data;
    float* g = grad->data;
    
    float scale = 1.0f / n;
    for (int i = 0; i < n; i++) {
        float diff = pred[i] - targ[i];
        g[i] = scale * (diff > 0.0f ? 1.0f : (diff < 0.0f ? -1.0f : 0.0f));
    }
}

/* ========================================================================
 * Huber Loss
 * ======================================================================== */

float huber_loss(const Tensor* predictions, const Tensor* targets, float delta) {
    int n = predictions->size;
    const float* pred = predictions->data;
    const float* targ = targets->data;
    
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        float diff = fabsf(pred[i] - targ[i]);
        if (diff <= delta) {
            sum += 0.5f * diff * diff;
        } else {
            sum += delta * (diff - 0.5f * delta);
        }
    }
    
    return sum / n;
}

void huber_backward(const Tensor* predictions, const Tensor* targets, 
                   Tensor* grad, float delta) {
    int n = predictions->size;
    const float* pred = predictions->data;
    const float* targ = targets->data;
    float* g = grad->data;
    
    float scale = 1.0f / n;
    for (int i = 0; i < n; i++) {
        float diff = pred[i] - targ[i];
        float abs_diff = fabsf(diff);
        
        if (abs_diff <= delta) {
            g[i] = scale * diff;
        } else {
            g[i] = scale * delta * (diff > 0.0f ? 1.0f : -1.0f);
        }
    }
}
