#include "math_lib_runtime.h"
#include <math.h>
#include <float.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef M_E
#define M_E 2.71828182845904523536
#endif

// ============================================================================
// TRIGONOMETRIC FUNCTIONS
// ============================================================================

double nuwan_math_sin(double x) {
    return sin(x);
}

double nuwan_math_cos(double x) {
    return cos(x);
}

double nuwan_math_tan(double x) {
    return tan(x);
}

double nuwan_math_asin(double x) {
    return asin(x);
}

double nuwan_math_acos(double x) {
    return acos(x);
}

double nuwan_math_atan(double x) {
    return atan(x);
}

double nuwan_math_atan2(double y, double x) {
    return atan2(y, x);
}

// ============================================================================
// HYPERBOLIC FUNCTIONS
// ============================================================================

double nuwan_math_sinh(double x) {
    return sinh(x);
}

double nuwan_math_cosh(double x) {
    return cosh(x);
}

double nuwan_math_tanh(double x) {
    return tanh(x);
}

// ============================================================================
// EXPONENTIAL AND LOGARITHMIC
// ============================================================================

double nuwan_math_exp(double x) {
    return exp(x);
}

double nuwan_math_log(double x) {
    return log(x);
}

double nuwan_math_log10(double x) {
    return log10(x);
}

double nuwan_math_log2(double x) {
    return log2(x);
}

double nuwan_math_pow(double base, double exponent) {
    return pow(base, exponent);
}

double nuwan_math_sqrt(double x) {
    return sqrt(x);
}

double nuwan_math_cbrt(double x) {
    return cbrt(x);
}

// ============================================================================
// ROUNDING AND REMAINDER
// ============================================================================

double nuwan_math_ceil(double x) {
    return ceil(x);
}

double nuwan_math_floor(double x) {
    return floor(x);
}

double nuwan_math_round(double x) {
    return round(x);
}

double nuwan_math_trunc(double x) {
    return trunc(x);
}

double nuwan_math_fmod(double x, double y) {
    return fmod(x, y);
}

double nuwan_math_remainder(double x, double y) {
    return remainder(x, y);
}

// ============================================================================
// ABSOLUTE AND SIGN
// ============================================================================

double nuwan_math_abs(double x) {
    return fabs(x);
}

double nuwan_math_fabs(double x) {
    return fabs(x);
}

int nuwan_math_sign(double x) {
    if (x > 0.0) return 1;
    if (x < 0.0) return -1;
    return 0;
}

// ============================================================================
// MIN/MAX
// ============================================================================

double nuwan_math_min(double a, double b) {
    return (a < b) ? a : b;
}

double nuwan_math_max(double a, double b) {
    return (a > b) ? a : b;
}

// ============================================================================
// ADVANCED FUNCTIONS
// ============================================================================

double nuwan_math_hypot(double x, double y) {
    return hypot(x, y);
}

// Factorial using Stirling's approximation for large n
double nuwan_math_factorial(int n) {
    if (n < 0) return 0.0;
    if (n == 0 || n == 1) return 1.0;
    
    // For small n, compute exactly
    if (n <= 20) {
        double result = 1.0;
        for (int i = 2; i <= n; i++) {
            result *= i;
        }
        return result;
    }
    
    // For large n, use Stirling's approximation
    // n! ≈ sqrt(2πn) * (n/e)^n
    return sqrt(2.0 * M_PI * n) * pow(n / M_E, n);
}

// Gamma function using Lanczos approximation
double nuwan_math_gamma(double x) {
    return tgamma(x);
}

// Error function
double nuwan_math_erf(double x) {
    return erf(x);
}

// Complementary error function
double nuwan_math_erfc(double x) {
    return erfc(x);
}

// ============================================================================
// CONSTANTS
// ============================================================================

double nuwan_math_pi(void) {
    return M_PI;
}

double nuwan_math_e(void) {
    return M_E;
}

double nuwan_math_tau(void) {
    return 2.0 * M_PI;
}

double nuwan_math_phi(void) {
    return 1.618033988749894848204586834365638117720;  // Golden ratio
}

// ============================================================================
// ANGLE CONVERSION
// ============================================================================

double nuwan_math_deg_to_rad(double degrees) {
    return degrees * M_PI / 180.0;
}

double nuwan_math_rad_to_deg(double radians) {
    return radians * 180.0 / M_PI;
}

// ============================================================================
// CLAMPING AND INTERPOLATION
// ============================================================================

double nuwan_math_clamp(double value, double min, double max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

double nuwan_math_lerp(double a, double b, double t) {
    return a + t * (b - a);
}

// ============================================================================
// FLOATING POINT CLASSIFICATION
// ============================================================================

int nuwan_math_is_nan(double x) {
    return isnan(x);
}

int nuwan_math_is_inf(double x) {
    return isinf(x);
}

int nuwan_math_is_finite(double x) {
    return isfinite(x);
}

int nuwan_math_approx_equal(double a, double b, double epsilon) {
    return fabs(a - b) < epsilon;
}
