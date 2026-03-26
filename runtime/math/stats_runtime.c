#include "stats_runtime.h"
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Comparison function for qsort
static int compare_doubles(const void* a, const void* b) {
    double diff = (*(double*)a - *(double*)b);
    return (diff > 0) - (diff < 0);
}

// ============================================================================
// DESCRIPTIVE STATISTICS
// ============================================================================

double nuwan_stats_mean(NuwanVector* data) {
    if (!data || data->size == 0) return 0.0;
    
    double sum = 0.0;
    for (int i = 0; i < data->size; i++) {
        sum += data->data[i];
    }
    
    return sum / data->size;
}

double nuwan_stats_median(NuwanVector* data) {
    if (!data || data->size == 0) return 0.0;
    
    // Create sorted copy
    double* sorted = (double*)malloc(data->size * sizeof(double));
    memcpy(sorted, data->data, data->size * sizeof(double));
    qsort(sorted, data->size, sizeof(double), compare_doubles);
    
    double result;
    if (data->size % 2 == 0) {
        result = (sorted[data->size/2 - 1] + sorted[data->size/2]) / 2.0;
    } else {
        result = sorted[data->size/2];
    }
    
    free(sorted);
    return result;
}

double nuwan_stats_variance(NuwanVector* data) {
    if (!data || data->size < 2) return 0.0;
    
    double mean = nuwan_stats_mean(data);
    double sum_sq_diff = 0.0;
    
    for (int i = 0; i < data->size; i++) {
        double diff = data->data[i] - mean;
        sum_sq_diff += diff * diff;
    }
    
    return sum_sq_diff / (data->size - 1);  // Sample variance
}

double nuwan_stats_stddev(NuwanVector* data) {
    return sqrt(nuwan_stats_variance(data));
}

double nuwan_stats_min(NuwanVector* data) {
    if (!data || data->size == 0) return 0.0;
    
    double min_val = data->data[0];
    for (int i = 1; i < data->size; i++) {
        if (data->data[i] < min_val) {
            min_val = data->data[i];
        }
    }
    
    return min_val;
}

double nuwan_stats_max(NuwanVector* data) {
    if (!data || data->size == 0) return 0.0;
    
    double max_val = data->data[0];
    for (int i = 1; i < data->size; i++) {
        if (data->data[i] > max_val) {
            max_val = data->data[i];
        }
    }
    
    return max_val;
}

double nuwan_stats_sum(NuwanVector* data) {
    if (!data) return 0.0;
    
    double sum = 0.0;
    for (int i = 0; i < data->size; i++) {
        sum += data->data[i];
    }
    
    return sum;
}

// ============================================================================
// PROBABILITY DISTRIBUTIONS
// ============================================================================

// Normal distribution PDF
double nuwan_stats_normal_pdf(double x, double mean, double stddev) {
    if (stddev <= 0.0) return 0.0;
    
    double z = (x - mean) / stddev;
    return (1.0 / (stddev * sqrt(2.0 * M_PI))) * exp(-0.5 * z * z);
}

// Normal distribution CDF (using error function approximation)
double nuwan_stats_normal_cdf(double x, double mean, double stddev) {
    if (stddev <= 0.0) return 0.0;
    
    double z = (x - mean) / (stddev * sqrt(2.0));
    
    // Error function approximation
    double t = 1.0 / (1.0 + 0.3275911 * fabs(z));
    double poly = t * (0.254829592 + t * (-0.284496736 + 
                  t * (1.421413741 + t * (-1.453152027 + t * 1.061405429))));
    double erf_val = 1.0 - poly * exp(-z * z);
    
    if (z < 0) erf_val = -erf_val;
    
    return 0.5 * (1.0 + erf_val);
}

// ============================================================================
// RANDOM NUMBER GENERATION
// ============================================================================

static unsigned int rng_state = 0;
static int rng_initialized = 0;

void nuwan_stats_seed(unsigned int seed) {
    rng_state = seed;
    rng_initialized = 1;
    srand(seed);
}

double nuwan_stats_random_uniform(void) {
    if (!rng_initialized) {
        nuwan_stats_seed((unsigned int)time(NULL));
    }
    
    return (double)rand() / (double)RAND_MAX;
}

// Box-Muller transform for normal distribution
double nuwan_stats_random_normal(double mean, double stddev) {
    static int has_spare = 0;
    static double spare;
    
    if (has_spare) {
        has_spare = 0;
        return mean + stddev * spare;
    }
    
    has_spare = 1;
    
    double u, v, s;
    do {
        u = nuwan_stats_random_uniform() * 2.0 - 1.0;
        v = nuwan_stats_random_uniform() * 2.0 - 1.0;
        s = u * u + v * v;
    } while (s >= 1.0 || s == 0.0);
    
    s = sqrt(-2.0 * log(s) / s);
    spare = v * s;
    
    return mean + stddev * u * s;
}

int nuwan_stats_random_int(int min, int max) {
    if (min >= max) return min;
    
    return min + (int)(nuwan_stats_random_uniform() * (max - min + 1));
}

// ============================================================================
// CORRELATION AND COVARIANCE
// ============================================================================

double nuwan_stats_covariance(NuwanVector* x, NuwanVector* y) {
    if (!x || !y || x->size != y->size || x->size < 2) return 0.0;
    
    double mean_x = nuwan_stats_mean(x);
    double mean_y = nuwan_stats_mean(y);
    
    double sum = 0.0;
    for (int i = 0; i < x->size; i++) {
        sum += (x->data[i] - mean_x) * (y->data[i] - mean_y);
    }
    
    return sum / (x->size - 1);
}

double nuwan_stats_correlation(NuwanVector* x, NuwanVector* y) {
    if (!x || !y || x->size != y->size || x->size < 2) return 0.0;
    
    double cov = nuwan_stats_covariance(x, y);
    double std_x = nuwan_stats_stddev(x);
    double std_y = nuwan_stats_stddev(y);
    
    if (std_x == 0.0 || std_y == 0.0) return 0.0;
    
    return cov / (std_x * std_y);
}

// ============================================================================
// LINEAR REGRESSION
// ============================================================================

LinearRegressionResult nuwan_stats_linear_regression(NuwanVector* x, NuwanVector* y) {
    LinearRegressionResult result = {0.0, 0.0, 0.0};
    
    if (!x || !y || x->size != y->size || x->size < 2) {
        return result;
    }
    
    int n = x->size;
    double mean_x = nuwan_stats_mean(x);
    double mean_y = nuwan_stats_mean(y);
    
    // Calculate slope using least squares
    double numerator = 0.0;
    double denominator = 0.0;
    
    for (int i = 0; i < n; i++) {
        double dx = x->data[i] - mean_x;
        double dy = y->data[i] - mean_y;
        numerator += dx * dy;
        denominator += dx * dx;
    }
    
    if (denominator == 0.0) {
        return result;
    }
    
    result.slope = numerator / denominator;
    result.intercept = mean_y - result.slope * mean_x;
    
    // Calculate R-squared
    double ss_res = 0.0;  // Sum of squared residuals
    double ss_tot = 0.0;  // Total sum of squares
    
    for (int i = 0; i < n; i++) {
        double y_pred = result.slope * x->data[i] + result.intercept;
        double residual = y->data[i] - y_pred;
        ss_res += residual * residual;
        
        double total_dev = y->data[i] - mean_y;
        ss_tot += total_dev * total_dev;
    }
    
    if (ss_tot > 0.0) {
        result.r_squared = 1.0 - (ss_res / ss_tot);
    }
    
    return result;
}
