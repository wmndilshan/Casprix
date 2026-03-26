#ifndef LINALG_RUNTIME_H
#define LINALG_RUNTIME_H

#include <stddef.h>

// Vector structure - optimized for cache locality
typedef struct {
    double* data;      // Contiguous array for cache efficiency
    int size;
    int capacity;
} NuwanVector;

// Matrix structure - row-major for better cache performance
typedef struct {
    double* data;      // Flattened row-major storage
    int rows;
    int cols;
    int capacity;
} NuwanMatrix;

// Vector operations
NuwanVector* nuwan_vector_create(int size);
void nuwan_vector_free(NuwanVector* v);
double nuwan_vector_get(NuwanVector* v, int index);
void nuwan_vector_set(NuwanVector* v, int index, double value);
void nuwan_vector_fill(NuwanVector* v, double value);
NuwanVector* nuwan_vector_copy(NuwanVector* v);

// Vector arithmetic - SIMD optimized when possible
double nuwan_vector_dot(NuwanVector* a, NuwanVector* b);
NuwanVector* nuwan_vector_add(NuwanVector* a, NuwanVector* b);
NuwanVector* nuwan_vector_subtract(NuwanVector* a, NuwanVector* b);
NuwanVector* nuwan_vector_scale(NuwanVector* v, double scalar);
double nuwan_vector_magnitude(NuwanVector* v);
NuwanVector* nuwan_vector_normalize(NuwanVector* v);

// Matrix operations
NuwanMatrix* nuwan_matrix_create(int rows, int cols);
void nuwan_matrix_free(NuwanMatrix* m);
double nuwan_matrix_get(NuwanMatrix* m, int row, int col);
void nuwan_matrix_set(NuwanMatrix* m, int row, int col, double value);
void nuwan_matrix_fill(NuwanMatrix* m, double value);
NuwanMatrix* nuwan_matrix_copy(NuwanMatrix* m);

// Matrix arithmetic - cache-optimized
NuwanMatrix* nuwan_matrix_multiply(NuwanMatrix* a, NuwanMatrix* b);
NuwanMatrix* nuwan_matrix_add(NuwanMatrix* a, NuwanMatrix* b);
NuwanMatrix* nuwan_matrix_subtract(NuwanMatrix* a, NuwanMatrix* b);
NuwanMatrix* nuwan_matrix_scale(NuwanMatrix* m, double scalar);
NuwanMatrix* nuwan_matrix_transpose(NuwanMatrix* m);

// Matrix-vector operations
NuwanVector* nuwan_matrix_vector_multiply(NuwanMatrix* m, NuwanVector* v);

// Advanced matrix operations
double nuwan_matrix_determinant(NuwanMatrix* m);
NuwanMatrix* nuwan_matrix_inverse(NuwanMatrix* m);

// Factory methods
NuwanMatrix* nuwan_matrix_identity(int size);
NuwanMatrix* nuwan_matrix_zeros(int rows, int cols);
NuwanMatrix* nuwan_matrix_ones(int rows, int cols);

#endif // LINALG_RUNTIME_H
