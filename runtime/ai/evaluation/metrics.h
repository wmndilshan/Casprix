/**
 * Evaluation Metrics
 * 
 * Common metrics for model evaluation
 */

#ifndef NN_METRICS_H
#define NN_METRICS_H

#include "../core/tensor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Classification accuracy
 * @param predictions Predicted class indices [batch]
 * @param targets True class indices [batch]
 * @param batch Number of samples
 * @return Accuracy (0.0 - 1.0)
 */
float metrics_accuracy(const float* predictions, const float* targets, int batch);

/**
 * Binary classification metrics
 * @param predictions Predicted probabilities [batch]
 * @param targets True labels (0 or 1) [batch]
 * @param threshold Classification threshold (default: 0.5)
 * @param precision Output precision
 * @param recall Output recall
 * @param f1 Output F1 score
 */
void metrics_binary_classification(const float* predictions, const float* targets,
                                   int batch, float threshold,
                                   float* precision, float* recall, float* f1);

/**
 * Confusion matrix for multiclass
 * @param predictions Predicted class indices [batch]
 * @param targets True class indices [batch]
 * @param num_classes Number of classes
 * @param confusion_matrix Output matrix [num_classes, num_classes]
 */
void metrics_confusion_matrix(const float* predictions, const float* targets,
                             int batch, int num_classes,
                             int* confusion_matrix);

/**
 * Top-K accuracy
 * @param logits Raw logits [batch, num_classes]
 * @param targets True class indices [batch]
 * @param batch Batch size
 * @param num_classes Number of classes
 * @param k Top-K (e.g., k=5 for top-5 accuracy)
 * @return Top-K accuracy
 */
float metrics_top_k_accuracy(const Tensor* logits, const float* targets,
                             int batch, int num_classes, int k);

/* Regression metrics. Invalid/empty inputs return NAN. */
float metrics_mean_squared_error(const float* predictions, const float* targets,
                                 int count);
float metrics_mean_absolute_error(const float* predictions, const float* targets,
                                  int count);
float metrics_r2_score(const float* predictions, const float* targets, int count);

/* Numerically stable weighted streaming mean, useful for epoch loss/metrics. */
typedef struct RunningMetric {
    double mean;
    double weight;
} RunningMetric;

void metrics_running_reset(RunningMetric* metric);
void metrics_running_update(RunningMetric* metric, double value, double weight);
double metrics_running_value(const RunningMetric* metric);

#ifdef __cplusplus
}
#endif

#endif /* NN_METRICS_H */
