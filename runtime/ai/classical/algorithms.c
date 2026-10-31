#include "algorithms.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>

static bool matrix(const Tensor* value) {
    return value && value->data && value->ndim == 2 && tensor_is_contiguous(value);
}

static bool vector(const Tensor* value, int count) {
    return value && value->data && value->ndim == 1 && value->shape[0] == count &&
           tensor_is_contiguous(value);
}

static float dot_row(const Tensor* features, int row, const Tensor* weights) {
    int columns = features->shape[1];
    const float* sample = features->data + (size_t)row * columns;
    double result = 0.0;
    for (int column = 0; column < columns; column++) result += sample[column] * weights->data[column];
    return (float)result;
}

static float sigmoid_stable(float value) {
    if (value >= 0.0f) return 1.0f / (1.0f + expf(-value));
    float exponential = expf(value);
    return exponential / (1.0f + exponential);
}

AiLinearRegression* ai_linear_regression_create(int feature_count) {
    if (feature_count <= 0) return NULL;
    AiLinearRegression* model = (AiLinearRegression*)calloc(1, sizeof(*model));
    i32 shape[] = { feature_count };
    if (model) model->weights = tensor_create(1, shape);
    if (!model || !model->weights) { ai_linear_regression_destroy(model); return NULL; }
    return model;
}

void ai_linear_regression_destroy(AiLinearRegression* model) {
    if (!model) return;
    tensor_destroy(model->weights);
    free(model);
}

AiMlStatus ai_linear_regression_fit(AiLinearRegression* model, const Tensor* features,
                                    const Tensor* targets, float learning_rate, int epochs) {
    if (!model || !matrix(features) || learning_rate <= 0.0f || epochs <= 0)
        return AI_ML_INVALID_ARGUMENT;
    int rows = features->shape[0], columns = features->shape[1];
    if (!vector(targets, rows) || !vector(model->weights, columns)) return AI_ML_SHAPE_MISMATCH;
    double* gradients = (double*)calloc((size_t)columns, sizeof(*gradients));
    if (!gradients) return AI_ML_ALLOCATION_FAILED;
    for (int epoch = 0; epoch < epochs; epoch++) {
        for (int c = 0; c < columns; c++) gradients[c] = 0.0;
        double bias_gradient = 0.0;
        for (int row = 0; row < rows; row++) {
            double error = dot_row(features, row, model->weights) + model->bias - targets->data[row];
            bias_gradient += error;
            for (int c = 0; c < columns; c++)
                gradients[c] += error * features->data[(size_t)row * columns + c];
        }
        float scale = 2.0f * learning_rate / rows;
        for (int c = 0; c < columns; c++) model->weights->data[c] -= scale * (float)gradients[c];
        model->bias -= scale * (float)bias_gradient;
        if (!isfinite(model->bias)) { free(gradients); return AI_ML_NONFINITE; }
    }
    free(gradients);
    return AI_ML_OK;
}

AiMlStatus ai_linear_regression_predict(const AiLinearRegression* model,
                                        const Tensor* features, Tensor* predictions) {
    if (!model || !matrix(features)) return AI_ML_INVALID_ARGUMENT;
    if (!vector(model->weights, features->shape[1]) || !vector(predictions, features->shape[0]))
        return AI_ML_SHAPE_MISMATCH;
    for (int row = 0; row < features->shape[0]; row++)
        predictions->data[row] = dot_row(features, row, model->weights) + model->bias;
    return AI_ML_OK;
}

AiLogisticRegression* ai_logistic_regression_create(int feature_count) {
    if (feature_count <= 0) return NULL;
    AiLogisticRegression* model = (AiLogisticRegression*)calloc(1, sizeof(*model));
    i32 shape[] = { feature_count };
    if (model) model->weights = tensor_create(1, shape);
    if (!model || !model->weights) { ai_logistic_regression_destroy(model); return NULL; }
    return model;
}

void ai_logistic_regression_destroy(AiLogisticRegression* model) {
    if (!model) return;
    tensor_destroy(model->weights);
    free(model);
}

AiMlStatus ai_logistic_regression_fit(AiLogisticRegression* model, const Tensor* features,
                                      const Tensor* labels, float learning_rate, int epochs,
                                      float l2_penalty) {
    if (!model || !matrix(features) || learning_rate <= 0.0f || epochs <= 0 || l2_penalty < 0.0f)
        return AI_ML_INVALID_ARGUMENT;
    int rows = features->shape[0], columns = features->shape[1];
    if (!vector(labels, rows) || !vector(model->weights, columns)) return AI_ML_SHAPE_MISMATCH;
    double* gradients = (double*)calloc((size_t)columns, sizeof(*gradients));
    if (!gradients) return AI_ML_ALLOCATION_FAILED;
    for (int epoch = 0; epoch < epochs; epoch++) {
        for (int c = 0; c < columns; c++) gradients[c] = 0.0;
        double bias_gradient = 0.0;
        for (int row = 0; row < rows; row++) {
            double error = sigmoid_stable(dot_row(features, row, model->weights) + model->bias) -
                           labels->data[row];
            bias_gradient += error;
            for (int c = 0; c < columns; c++)
                gradients[c] += error * features->data[(size_t)row * columns + c];
        }
        for (int c = 0; c < columns; c++) {
            double gradient = gradients[c] / rows + l2_penalty * model->weights->data[c];
            model->weights->data[c] -= learning_rate * (float)gradient;
        }
        model->bias -= learning_rate * (float)(bias_gradient / rows);
        if (!isfinite(model->bias)) { free(gradients); return AI_ML_NONFINITE; }
    }
    free(gradients);
    return AI_ML_OK;
}

AiMlStatus ai_logistic_regression_predict_proba(const AiLogisticRegression* model,
                                                const Tensor* features, Tensor* probabilities) {
    if (!model || !matrix(features)) return AI_ML_INVALID_ARGUMENT;
    if (!vector(model->weights, features->shape[1]) || !vector(probabilities, features->shape[0]))
        return AI_ML_SHAPE_MISMATCH;
    for (int row = 0; row < features->shape[0]; row++)
        probabilities->data[row] = sigmoid_stable(dot_row(features, row, model->weights) + model->bias);
    return AI_ML_OK;
}

AiKMeans* ai_kmeans_create(int clusters, int feature_count) {
    if (clusters <= 0 || feature_count <= 0) return NULL;
    AiKMeans* model = (AiKMeans*)calloc(1, sizeof(*model));
    i32 shape[] = { clusters, feature_count };
    if (model) { model->clusters = clusters; model->centroids = tensor_create(2, shape); }
    if (!model || !model->centroids) { ai_kmeans_destroy(model); return NULL; }
    return model;
}

void ai_kmeans_destroy(AiKMeans* model) {
    if (!model) return;
    tensor_destroy(model->centroids);
    free(model);
}

static double distance_squared(const float* left, const float* right, int columns) {
    double distance = 0.0;
    for (int c = 0; c < columns; c++) { double d = left[c] - right[c]; distance += d * d; }
    return distance;
}

int ai_kmeans_predict(const AiKMeans* model, const float* sample) {
    if (!model || !sample || !matrix(model->centroids)) return -1;
    int columns = model->centroids->shape[1], nearest = 0;
    double best = DBL_MAX;
    for (int cluster = 0; cluster < model->clusters; cluster++) {
        double distance = distance_squared(sample, model->centroids->data + (size_t)cluster * columns,
                                           columns);
        if (distance < best) { best = distance; nearest = cluster; }
    }
    return nearest;
}

AiMlStatus ai_kmeans_fit(AiKMeans* model, const Tensor* features, int max_iterations,
                         float tolerance) {
    if (!model || !matrix(features) || max_iterations <= 0 || tolerance < 0.0f)
        return AI_ML_INVALID_ARGUMENT;
    int rows = features->shape[0], columns = features->shape[1];
    if (!matrix(model->centroids) || model->centroids->shape[1] != columns ||
        rows < model->clusters) return AI_ML_SHAPE_MISMATCH;
    double* sums = (double*)calloc(model->centroids->size, sizeof(*sums));
    int* counts = (int*)calloc((size_t)model->clusters, sizeof(*counts));
    if (!sums || !counts) {
        free(sums); free(counts); return AI_ML_ALLOCATION_FAILED;
    }
    /* Deterministic farthest-first initialization avoids random global state. */
    for (int c = 0; c < columns; c++) model->centroids->data[c] = features->data[c];
    for (int cluster = 1; cluster < model->clusters; cluster++) {
        int farthest = 0; double farthest_distance = -1.0;
        for (int row = 0; row < rows; row++) {
            double nearest = DBL_MAX;
            for (int existing = 0; existing < cluster; existing++) {
                double d = distance_squared(features->data + (size_t)row * columns,
                    model->centroids->data + (size_t)existing * columns, columns);
                if (d < nearest) nearest = d;
            }
            if (nearest > farthest_distance) { farthest_distance = nearest; farthest = row; }
        }
        for (int c = 0; c < columns; c++) model->centroids->data[(size_t)cluster * columns + c] =
            features->data[(size_t)farthest * columns + c];
    }
    for (int iteration = 0; iteration < max_iterations; iteration++) {
        for (size_t i = 0; i < model->centroids->size; i++) sums[i] = 0.0;
        for (int cluster = 0; cluster < model->clusters; cluster++) counts[cluster] = 0;
        model->inertia = 0.0f;
        for (int row = 0; row < rows; row++) {
            const float* sample = features->data + (size_t)row * columns;
            int cluster = ai_kmeans_predict(model, sample);
            counts[cluster]++;
            model->inertia += (float)distance_squared(sample,
                model->centroids->data + (size_t)cluster * columns, columns);
            for (int c = 0; c < columns; c++) sums[(size_t)cluster * columns + c] += sample[c];
        }
        double max_shift = 0.0;
        for (int cluster = 0; cluster < model->clusters; cluster++) {
            if (counts[cluster] == 0) continue;
            for (int c = 0; c < columns; c++) {
                size_t index = (size_t)cluster * columns + c;
                float next = (float)(sums[index] / counts[cluster]);
                double shift = fabs(next - model->centroids->data[index]);
                if (shift > max_shift) max_shift = shift;
                model->centroids->data[index] = next;
            }
        }
        if (max_shift <= tolerance) break;
    }
    /* Report inertia for the final centroids, not the pre-update assignment. */
    model->inertia = 0.0f;
    for (int row = 0; row < rows; row++) {
        const float* sample = features->data + (size_t)row * columns;
        int cluster = ai_kmeans_predict(model, sample);
        model->inertia += (float)distance_squared(sample,
            model->centroids->data + (size_t)cluster * columns, columns);
    }
    free(sums); free(counts);
    return AI_ML_OK;
}
