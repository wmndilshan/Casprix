#ifndef ML_RUNTIME_H
#define ML_RUNTIME_H

#include "linalg_runtime.h"

// ============================================================================
// LINEAR REGRESSION
// ============================================================================

typedef struct {
    NuwanVector* weights;
    double bias;
    int features;
} LinearRegressionModel;

LinearRegressionModel* nuwan_ml_linear_regression_create(int features);
void nuwan_ml_linear_regression_free(LinearRegressionModel* model);
void nuwan_ml_linear_regression_train(LinearRegressionModel* model, 
                                       NuwanMatrix* X, NuwanVector* y, 
                                       double learning_rate, int epochs);
double nuwan_ml_linear_regression_predict(LinearRegressionModel* model, 
                                           NuwanVector* x);
NuwanVector* nuwan_ml_linear_regression_predict_batch(LinearRegressionModel* model, 
                                                       NuwanMatrix* X);
double nuwan_ml_linear_regression_score(LinearRegressionModel* model, 
                                         NuwanMatrix* X, NuwanVector* y);

// ============================================================================
// K-MEANS CLUSTERING
// ============================================================================

typedef struct {
    NuwanMatrix* centroids;
    int k;
    int features;
    int* assignments;
    int num_samples;
} KMeansModel;

KMeansModel* nuwan_ml_kmeans_create(int k, int features);
void nuwan_ml_kmeans_free(KMeansModel* model);
void nuwan_ml_kmeans_train(KMeansModel* model, NuwanMatrix* X, int max_iters);
int nuwan_ml_kmeans_predict(KMeansModel* model, NuwanVector* x);
NuwanMatrix* nuwan_ml_kmeans_get_centroids(KMeansModel* model);

// ============================================================================
// NEURAL NETWORK
// ============================================================================

typedef struct {
    NuwanMatrix* weights;
    NuwanVector* bias;
    int input_size;
    int output_size;
} NeuralLayer;

NeuralLayer* nuwan_ml_layer_create(int input_size, int output_size);
void nuwan_ml_layer_free(NeuralLayer* layer);
NuwanVector* nuwan_ml_layer_forward(NeuralLayer* layer, NuwanVector* input);

// Activation functions
NuwanVector* nuwan_ml_relu(NuwanVector* x);
NuwanVector* nuwan_ml_sigmoid(NuwanVector* x);
NuwanVector* nuwan_ml_softmax(NuwanVector* x);

// Loss functions
double nuwan_ml_mse_loss(NuwanVector* predictions, NuwanVector* targets);
double nuwan_ml_cross_entropy_loss(NuwanVector* predictions, NuwanVector* targets);

#endif // ML_RUNTIME_H
