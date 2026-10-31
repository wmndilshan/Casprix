/**
 * Evaluation Metrics Implementation
 */

#include "metrics.h"
#include <string.h>
#include <math.h>

float metrics_accuracy(const float* predictions, const float* targets, int batch) {
    if (!predictions || !targets || batch <= 0) return NAN;
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
    int tp = 0, fp = 0, fn = 0;
    if (precision) *precision = 0.0f;
    if (recall) *recall = 0.0f;
    if (f1) *f1 = 0.0f;
    if (!predictions || !targets || batch <= 0 || !precision || !recall || !f1) return;
    
    for (int i = 0; i < batch; i++) {
        int pred = predictions[i] >= threshold ? 1 : 0;
        int true_val = (int)targets[i];
        
        if (pred == 1 && true_val == 1) tp++;
        else if (pred == 1 && true_val == 0) fp++;
        else if (pred == 0 && true_val == 1) fn++;
    }
    
    *precision = (tp + fp) > 0 ? (float)tp / (tp + fp) : 0.0f;
    *recall = (tp + fn) > 0 ? (float)tp / (tp + fn) : 0.0f;
    *f1 = (*precision + *recall) > 0 ? 2 * (*precision) * (*recall) / (*precision + *recall) : 0.0f;
}

void metrics_confusion_matrix(const float* predictions, const float* targets,
                             int batch, int num_classes,
                             int* confusion_matrix) {
    if (!confusion_matrix || num_classes <= 0) return;
    /* Initialize */
    memset(confusion_matrix, 0, num_classes * num_classes * sizeof(int));
    
    /* Fill matrix */
    if (!predictions || !targets || batch <= 0) return;
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
    if (!logits || !targets || !logits->data || batch <= 0 || num_classes <= 0 ||
        k <= 0 || logits->size < (size_t)batch * (size_t)num_classes) return NAN;
    if (k > num_classes) k = num_classes;
    int correct = 0;
    
    for (int b = 0; b < batch; b++) {
        const float* logit_row = logits->data + b * num_classes;
        int true_class = (int)targets[b];
        
        float true_logit = (true_class >= 0 && true_class < num_classes)
                               ? logit_row[true_class] : -INFINITY;
        int strictly_greater = 0;
        for (int c = 0; c < num_classes; c++)
            if (logit_row[c] > true_logit) strictly_greater++;
        if (strictly_greater < k) correct++;
    }
    
    return (float)correct / batch;
}

float metrics_mean_squared_error(const float* predictions, const float* targets,
                                 int count) {
    if (!predictions || !targets || count <= 0) return NAN;
    double sum = 0.0;
    for (int i = 0; i < count; i++) {
        double d = (double)predictions[i] - targets[i];
        sum += d * d;
    }
    return (float)(sum / count);
}

float metrics_mean_absolute_error(const float* predictions, const float* targets,
                                  int count) {
    if (!predictions || !targets || count <= 0) return NAN;
    double sum = 0.0;
    for (int i = 0; i < count; i++) sum += fabs((double)predictions[i] - targets[i]);
    return (float)(sum / count);
}

float metrics_r2_score(const float* predictions, const float* targets, int count) {
    if (!predictions || !targets || count <= 0) return NAN;
    double mean = 0.0;
    for (int i = 0; i < count; i++) mean += targets[i];
    mean /= count;
    double residual = 0.0, total = 0.0;
    for (int i = 0; i < count; i++) {
        double d = (double)predictions[i] - targets[i];
        double centered = targets[i] - mean;
        residual += d * d;
        total += centered * centered;
    }
    if (total == 0.0) return residual == 0.0 ? 1.0f : 0.0f;
    return (float)(1.0 - residual / total);
}

void metrics_running_reset(RunningMetric* metric) {
    if (!metric) return;
    metric->mean = 0.0;
    metric->weight = 0.0;
}

void metrics_running_update(RunningMetric* metric, double value, double weight) {
    if (!metric || weight <= 0.0 || !isfinite(value) || !isfinite(weight)) return;
    double next_weight = metric->weight + weight;
    metric->mean += (value - metric->mean) * weight / next_weight;
    metric->weight = next_weight;
}

double metrics_running_value(const RunningMetric* metric) {
    return (!metric || metric->weight == 0.0) ? NAN : metric->mean;
}
