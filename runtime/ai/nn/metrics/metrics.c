/**
 * Evaluation Metrics Implementation
 */

#include "metrics.h"
#include <string.h>
#include <math.h>

float metrics_accuracy(const float* predictions, const float* targets, int batch) {
    int correct = 0;
    
    for (int i = 0; i < batch; i++) {
        if ((int)predictions[i] == (int)targets[i]) {
            correct++;
        }
    }
    
    return (float)correct / batch;
}

void metrics_binary_classification(const float* predictions, const float* targets,
                                   int batch, float threshold,
                                   float* precision, float* recall, float* f1) {
    int tp = 0, fp = 0, fn = 0, tn = 0;
    
    for (int i = 0; i < batch; i++) {
        int pred = predictions[i] >= threshold ? 1 : 0;
        int true_val = (int)targets[i];
        
        if (pred == 1 && true_val == 1) tp++;
        else if (pred == 1 && true_val == 0) fp++;
        else if (pred == 0 && true_val == 1) fn++;
        else tn++;
    }
    
    *precision = (tp + fp) > 0 ? (float)tp / (tp + fp) : 0.0f;
    *recall = (tp + fn) > 0 ? (float)tp / (tp + fn) : 0.0f;
    *f1 = (*precision + *recall) > 0 ? 2 * (*precision) * (*recall) / (*precision + *recall) : 0.0f;
}

void metrics_confusion_matrix(const float* predictions, const float* targets,
                             int batch, int num_classes,
                             int* confusion_matrix) {
    /* Initialize */
    memset(confusion_matrix, 0, num_classes * num_classes * sizeof(int));
    
    /* Fill matrix */
    for (int i = 0; i < batch; i++) {
        int pred = (int)predictions[i];
        int true_class = (int)targets[i];
        
        if (pred >= 0 && pred < num_classes && true_class >= 0 && true_class < num_classes) {
            confusion_matrix[true_class * num_classes + pred]++;
        }
    }
}

float metrics_top_k_accuracy(const Tensor* logits, const float* targets,
                             int batch, int num_classes, int k) {
    int correct = 0;
    
    for (int b = 0; b < batch; b++) {
        const float* logit_row = logits->data + b * num_classes;
        int true_class = (int)targets[b];
        
        /* Find top-k predictions */
        int top_k_indices[10];  /* Assuming k <= 10 */
        for (int i = 0; i < k && i < 10; i++) {
            float max_val = -1e9f;
            int max_idx = 0;
            
            for (int c = 0; c < num_classes; c++) {
                /* Check if already in top-k */
                bool already_selected = false;
                for (int j = 0; j < i; j++) {
                    if (top_k_indices[j] == c) {
                        already_selected = true;
                        break;
                    }
                }
                
                if (!already_selected && logit_row[c] > max_val) {
                    max_val = logit_row[c];
                    max_idx = c;
                }
            }
            
            top_k_indices[i] = max_idx;
        }
        
        /* Check if true class is in top-k */
        for (int i = 0; i < k && i < 10; i++) {
            if (top_k_indices[i] == true_class) {
                correct++;
                break;
            }
        }
    }
    
    return (float)correct / batch;
}
