#ifndef STATS_RUNTIME_H
#define STATS_RUNTIME_H

#include "linalg_runtime.h"

// Descriptive statistics
double nuwan_stats_mean(NuwanVector* data);
double nuwan_stats_median(NuwanVector* data);
double nuwan_stats_variance(NuwanVector* data);
double nuwan_stats_stddev(NuwanVector* data);
double nuwan_stats_min(NuwanVector* data);
double nuwan_stats_max(NuwanVector* data);
double nuwan_stats_sum(NuwanVector* data);

// Probability distributions
double nuwan_stats_normal_pdf(double x, double mean, double stddev);
double nuwan_stats_normal_cdf(double x, double mean, double stddev);

// Random number generation
void nuwan_stats_seed(unsigned int seed);
double nuwan_stats_random_uniform(void);  // [0, 1)
double nuwan_stats_random_normal(double mean, double stddev);
int nuwan_stats_random_int(int min, int max);

// Correlation and covariance
double nuwan_stats_correlation(NuwanVector* x, NuwanVector* y);
double nuwan_stats_covariance(NuwanVector* x, NuwanVector* y);

// Linear regression
typedef struct {
    double slope;
    double intercept;
    double r_squared;
} LinearRegressionResult;

LinearRegressionResult nuwan_stats_linear_regression(NuwanVector* x, NuwanVector* y);

#endif // STATS_RUNTIME_H
