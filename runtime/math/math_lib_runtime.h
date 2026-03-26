#ifndef MATH_LIB_RUNTIME_H
#define MATH_LIB_RUNTIME_H

// Advanced mathematical functions runtime for Casprix
// High-performance implementations of transcendental and special functions

// Trigonometric functions
double nuwan_math_sin(double x);
double nuwan_math_cos(double x);
double nuwan_math_tan(double x);
double nuwan_math_asin(double x);
double nuwan_math_acos(double x);
double nuwan_math_atan(double x);
double nuwan_math_atan2(double y, double x);

// Hyperbolic functions
double nuwan_math_sinh(double x);
double nuwan_math_cosh(double x);
double nuwan_math_tanh(double x);

// Exponential and logarithmic
double nuwan_math_exp(double x);
double nuwan_math_log(double x);     // Natural logarithm
double nuwan_math_log10(double x);   // Base-10 logarithm
double nuwan_math_log2(double x);    // Base-2 logarithm
double nuwan_math_pow(double base, double exponent);
double nuwan_math_sqrt(double x);
double nuwan_math_cbrt(double x);    // Cube root

// Rounding and remainder
double nuwan_math_ceil(double x);
double nuwan_math_floor(double x);
double nuwan_math_round(double x);
double nuwan_math_trunc(double x);
double nuwan_math_fmod(double x, double y);
double nuwan_math_remainder(double x, double y);

// Absolute and sign
double nuwan_math_abs(double x);
double nuwan_math_fabs(double x);
int nuwan_math_sign(double x);

// Min/Max
double nuwan_math_min(double a, double b);
double nuwan_math_max(double a, double b);

// Advanced functions
double nuwan_math_hypot(double x, double y);  // sqrt(x² + y²)
double nuwan_math_factorial(int n);
double nuwan_math_gamma(double x);
double nuwan_math_erf(double x);              // Error function
double nuwan_math_erfc(double x);             // Complementary error function

// Constants
double nuwan_math_pi(void);
double nuwan_math_e(void);
double nuwan_math_tau(void);                   // 2π
double nuwan_math_phi(void);                   // Golden ratio

// Angle conversion
double nuwan_math_deg_to_rad(double degrees);
double nuwan_math_rad_to_deg(double radians);

// Clamping and lerp
double nuwan_math_clamp(double value, double min, double max);
double nuwan_math_lerp(double a, double b, double t);

// Special comparison for floating point
int nuwan_math_is_nan(double x);
int nuwan_math_is_inf(double x);
int nuwan_math_is_finite(double x);
int nuwan_math_approx_equal(double a, double b, double epsilon);

#endif // MATH_LIB_RUNTIME_H
