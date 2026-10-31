#ifndef CPX_AI_ML_ALGORITHMS_H
#define CPX_AI_ML_ALGORITHMS_H

#include "../core/tensor.h"

typedef enum AiMlStatus {
    AI_ML_OK = 0,
    AI_ML_INVALID_ARGUMENT,
    AI_ML_SHAPE_MISMATCH,
    AI_ML_ALLOCATION_FAILED,
    AI_ML_NONFINITE
} AiMlStatus;

typedef struct AiLinearRegression {
    Tensor* weights;
    float bias;
} AiLinearRegression;

typedef struct AiLogisticRegression {
    Tensor* weights;
    float bias;
} AiLogisticRegression;

typedef struct AiKMeans {
    Tensor* centroids;
    int clusters;
    float inertia;
} AiKMeans;

AiLinearRegression* ai_linear_regression_create(int feature_count);
void ai_linear_regression_destroy(AiLinearRegression* model);
AiMlStatus ai_linear_regression_fit(AiLinearRegression* model, const Tensor* features,
                                    const Tensor* targets, float learning_rate, int epochs);
AiMlStatus ai_linear_regression_predict(const AiLinearRegression* model,
                                        const Tensor* features, Tensor* predictions);

AiLogisticRegression* ai_logistic_regression_create(int feature_count);
void ai_logistic_regression_destroy(AiLogisticRegression* model);
AiMlStatus ai_logistic_regression_fit(AiLogisticRegression* model, const Tensor* features,
                                      const Tensor* labels, float learning_rate, int epochs,
                                      float l2_penalty);
AiMlStatus ai_logistic_regression_predict_proba(const AiLogisticRegression* model,
                                                const Tensor* features, Tensor* probabilities);

AiKMeans* ai_kmeans_create(int clusters, int feature_count);
void ai_kmeans_destroy(AiKMeans* model);
AiMlStatus ai_kmeans_fit(AiKMeans* model, const Tensor* features, int max_iterations,
                         float tolerance);
int ai_kmeans_predict(const AiKMeans* model, const float* sample);

#endif
