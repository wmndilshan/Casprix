#include "ml_runtime.h"
#include "../math/stats_runtime.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <float.h>

// ============================================================================
// LINEAR REGRESSION - Gradient Descent Implementation
// ============================================================================

LinearRegressionModel* nuwan_ml_linear_regression_create(int features) {
    LinearRegressionModel* model = (LinearRegressionModel*)malloc(sizeof(LinearRegressionModel));
    if (!model) return NULL;
    
    model->features = features;
    model->weights = nuwan_vector_create(features);
    model->bias = 0.0;
    
    // Initialize with small random values
    for (int i = 0; i < features; i++) {
        model->weights->data[i] = (nuwan_stats_random_uniform() - 0.5) * 0.01;
    }
    
    return model;
}

void nuwan_ml_linear_regression_free(LinearRegressionModel* model) {
    if (model) {
        if (model->weights) nuwan_vector_free(model->weights);
        free(model);
    }
}

void nuwan_ml_linear_regression_train(LinearRegressionModel* model, 
                                       NuwanMatrix* X, NuwanVector* y, 
                                       double learning_rate, int epochs) {
    if (!model || !X || !y) return;
    
    int n_samples = X->rows;
    
    for (int epoch = 0; epoch < epochs; epoch++) {
        // Compute predictions
        double total_loss = 0.0;
        
        // Initialize gradients
        NuwanVector* weight_grad = nuwan_vector_create(model->features);
        nuwan_vector_fill(weight_grad, 0.0);
        double bias_grad = 0.0;
        
        // Compute gradients for all samples
        for (int i = 0; i < n_samples; i++) {
            // Forward pass
            double pred = model->bias;
            for (int j = 0; j < model->features; j++) {
                pred += X->data[i * X->cols + j] * model->weights->data[j];
            }
            
            // Compute error
            double error = pred - y->data[i];
            total_loss += error * error;
            
            // Accumulate gradients
            for (int j = 0; j < model->features; j++) {
                weight_grad->data[j] += error * X->data[i * X->cols + j];
            }
            bias_grad += error;
        }
        
        // Update weights and bias
        for (int j = 0; j < model->features; j++) {
            model->weights->data[j] -= learning_rate * weight_grad->data[j] / n_samples;
        }
        model->bias -= learning_rate * bias_grad / n_samples;
        
        nuwan_vector_free(weight_grad);
    }
}

double nuwan_ml_linear_regression_predict(LinearRegressionModel* model, NuwanVector* x) {
    if (!model || !x) return 0.0;
    
    double pred = model->bias;
    for (int i = 0; i < model->features; i++) {
        pred += x->data[i] * model->weights->data[i];
    }
    
    return pred;
}

NuwanVector* nuwan_ml_linear_regression_predict_batch(LinearRegressionModel* model, 
                                                       NuwanMatrix* X) {
    if (!model || !X) return NULL;
    
    NuwanVector* predictions = nuwan_vector_create(X->rows);
    
    for (int i = 0; i < X->rows; i++) {
        double pred = model->bias;
        for (int j = 0; j < model->features; j++) {
            pred += X->data[i * X->cols + j] * model->weights->data[j];
        }
        predictions->data[i] = pred;
    }
    
    return predictions;
}

double nuwan_ml_linear_regression_score(LinearRegressionModel* model, 
                                         NuwanMatrix* X, NuwanVector* y) {
    if (!model || !X || !y) return 0.0;
    
    NuwanVector* predictions = nuwan_ml_linear_regression_predict_batch(model, X);
    if (!predictions) return 0.0;
    
    double ss_res = 0.0;
    double ss_tot = 0.0;
    double mean_y = nuwan_stats_mean(y);
    
    for (int i = 0; i < y->size; i++) {
        double residual = y->data[i] - predictions->data[i];
        ss_res += residual * residual;
        
        double total = y->data[i] - mean_y;
        ss_tot += total * total;
    }
    
    nuwan_vector_free(predictions);
    
    if (ss_tot == 0.0) return 0.0;
    return 1.0 - (ss_res / ss_tot);  // R^2 score
}

// ============================================================================
// K-MEANS CLUSTERING
// ============================================================================

KMeansModel* nuwan_ml_kmeans_create(int k, int features) {
    KMeansModel* model = (KMeansModel*)malloc(sizeof(KMeansModel));
    if (!model) return NULL;
    
    model->k = k;
    model->features = features;
    model->centroids = nuwan_matrix_create(k, features);
    model->assignments = NULL;
    model->num_samples = 0;
    
    return model;
}

void nuwan_ml_kmeans_free(KMeansModel* model) {
    if (model) {
        if (model->centroids) nuwan_matrix_free(model->centroids);
        if (model->assignments) free(model->assignments);
        free(model);
    }
}

static double euclidean_distance(double* a, double* b, int size) {
#ifdef HAS_AVX2
    if (size >= 16) {
        extern double nuwan_simd_euclidean_dist_sq(const double* a, const double* b, int n);
        double dist_sq = nuwan_simd_euclidean_dist_sq(a, b, size);
        return sqrt(dist_sq);
    }
#endif
    
    // Fallback: scalar implementation
    double sum = 0.0;
    for (int i = 0; i < size; i++) {
        double diff = a[i] - b[i];
        sum += diff * diff;
    }
    return sqrt(sum);
}

void nuwan_ml_kmeans_train(KMeansModel* model, NuwanMatrix* X, int max_iters) {
    if (!model || !X) return;
    
    int n_samples = X->rows;
    model->num_samples = n_samples;
    
    // Allocate assignments
    if (model->assignments) free(model->assignments);
    model->assignments = (int*)malloc(n_samples * sizeof(int));
    
    // Initialize centroids randomly from data points
    for (int i = 0; i < model->k; i++) {
        int idx = nuwan_stats_random_int(0, n_samples - 1);
        for (int j = 0; j < model->features; j++) {
            model->centroids->data[i * model->features + j] = 
                X->data[idx * X->cols + j];
        }
    }
    
    // K-means iterations
    for (int iter = 0; iter < max_iters; iter++) {
        int changed = 0;
        
        // Assignment step
        for (int i = 0; i < n_samples; i++) {
            double min_dist = DBL_MAX;
            int best_cluster = 0;
            
            for (int k = 0; k < model->k; k++) {
                double dist = euclidean_distance(
                    &X->data[i * X->cols],
                    &model->centroids->data[k * model->features],
                    model->features
                );
                
                if (dist < min_dist) {
                    min_dist = dist;
                    best_cluster = k;
                }
            }
            
            if (model->assignments[i] != best_cluster) {
                model->assignments[i] = best_cluster;
                changed = 1;
            }
        }
        
        // Update step
        int* counts = (int*)calloc(model->k, sizeof(int));
        NuwanMatrix* new_centroids = nuwan_matrix_create(model->k, model->features);
        nuwan_matrix_fill(new_centroids, 0.0);
        
        for (int i = 0; i < n_samples; i++) {
            int cluster = model->assignments[i];
            counts[cluster]++;
            
            for (int j = 0; j < model->features; j++) {
                new_centroids->data[cluster * model->features + j] += 
                    X->data[i * X->cols + j];
            }
        }
        
        for (int k = 0; k < model->k; k++) {
            if (counts[k] > 0) {
                for (int j = 0; j < model->features; j++) {
                    model->centroids->data[k * model->features + j] = 
                        new_centroids->data[k * model->features + j] / counts[k];
                }
            }
        }
        
        free(counts);
        nuwan_matrix_free(new_centroids);
        
        if (!changed) break;  // Converged
    }
}

int nuwan_ml_kmeans_predict(KMeansModel* model, NuwanVector* x) {
    if (!model || !x) return -1;
    
    double min_dist = DBL_MAX;
    int best_cluster = 0;
    
    for (int k = 0; k < model->k; k++) {
        double dist = euclidean_distance(
            x->data,
            &model->centroids->data[k * model->features],
            model->features
        );
        
        if (dist < min_dist) {
            min_dist = dist;
            best_cluster = k;
        }
    }
    
    return best_cluster;
}

NuwanMatrix* nuwan_ml_kmeans_get_centroids(KMeansModel* model) {
    if (!model) return NULL;
    return nuwan_matrix_copy(model->centroids);
}

// ============================================================================
// NEURAL NETWORK LAYER
// ============================================================================

NeuralLayer* nuwan_ml_layer_create(int input_size, int output_size) {
    NeuralLayer* layer = (NeuralLayer*)malloc(sizeof(NeuralLayer));
    if (!layer) return NULL;
    
    layer->input_size = input_size;
    layer->output_size = output_size;
    layer->weights = nuwan_matrix_create(output_size, input_size);
    layer->bias = nuwan_vector_create(output_size);
    
    // Xavier initialization
    double std = sqrt(2.0 / (input_size + output_size));
    for (int i = 0; i < output_size * input_size; i++) {
        layer->weights->data[i] = nuwan_stats_random_normal(0.0, std);
    }
    
    nuwan_vector_fill(layer->bias, 0.0);
    
    return layer;
}

void nuwan_ml_layer_free(NeuralLayer* layer) {
    if (layer) {
        if (layer->weights) nuwan_matrix_free(layer->weights);
        if (layer->bias) nuwan_vector_free(layer->bias);
        free(layer);
    }
}

NuwanVector* nuwan_ml_layer_forward(NeuralLayer* layer, NuwanVector* input) {
    if (!layer || !input) return NULL;
    
    NuwanVector* output = nuwan_matrix_vector_multiply(layer->weights, input);
    if (!output) return NULL;
    
    // Add bias
    for (int i = 0; i < layer->output_size; i++) {
        output->data[i] += layer->bias->data[i];
    }
    
    return output;
}

// Activation functions
NuwanVector* nuwan_ml_relu(NuwanVector* x) {
    if (!x) return NULL;
    
    NuwanVector* result = nuwan_vector_create(x->size);
    for (int i = 0; i < x->size; i++) {
        result->data[i] = (x->data[i] > 0.0) ? x->data[i] : 0.0;
    }
    
    return result;
}

NuwanVector* nuwan_ml_sigmoid(NuwanVector* x) {
    if (!x) return NULL;
    
    NuwanVector* result = nuwan_vector_create(x->size);
    for (int i = 0; i < x->size; i++) {
        result->data[i] = 1.0 / (1.0 + exp(-x->data[i]));
    }
    
    return result;
}

NuwanVector* nuwan_ml_softmax(NuwanVector* x) {
    if (!x) return NULL;
    
    NuwanVector* result = nuwan_vector_create(x->size);
    
    // Find max for numerical stability
    double max_val = x->data[0];
    for (int i = 1; i < x->size; i++) {
        if (x->data[i] > max_val) max_val = x->data[i];
    }
    
    // Compute exp and sum
    double sum = 0.0;
    for (int i = 0; i < x->size; i++) {
        result->data[i] = exp(x->data[i] - max_val);
        sum += result->data[i];
    }
    
    // Normalize
    for (int i = 0; i < x->size; i++) {
        result->data[i] /= sum;
    }
    
    return result;
}

// Loss functions
double nuwan_ml_mse_loss(NuwanVector* predictions, NuwanVector* targets) {
    if (!predictions || !targets || predictions->size != targets->size) return 0.0;
    
    double sum = 0.0;
    for (int i = 0; i < predictions->size; i++) {
        double diff = predictions->data[i] - targets->data[i];
        sum += diff * diff;
    }
    
    return sum / predictions->size;
}

double nuwan_ml_cross_entropy_loss(NuwanVector* predictions, NuwanVector* targets) {
    if (!predictions || !targets || predictions->size != targets->size) return 0.0;
    
    double sum = 0.0;
    for (int i = 0; i < predictions->size; i++) {
        // Clip predictions to avoid log(0)
        double p = predictions->data[i];
        if (p < 1e-10) p = 1e-10;
        if (p > 1.0 - 1e-10) p = 1.0 - 1e-10;
        
        sum -= targets->data[i] * log(p);
    }
    
    return sum / predictions->size;
}
