#include "linalg_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ============================================================================
// VECTOR OPERATIONS - Optimized for performance
// ============================================================================

NuwanVector* nuwan_vector_create(int size) {
    NuwanVector* v = (NuwanVector*)malloc(sizeof(NuwanVector));
    if (!v) return NULL;
    
    v->size = size;
    v->capacity = size;
    // Aligned allocation for SIMD operations
    v->data = (double*)aligned_alloc(32, size * sizeof(double));
    if (!v->data) {
        free(v);
        return NULL;
    }
    
    memset(v->data, 0, size * sizeof(double));
    return v;
}

void nuwan_vector_free(NuwanVector* v) {
    if (v) {
        if (v->data) free(v->data);
        free(v);
    }
}

double nuwan_vector_get(NuwanVector* v, int index) {
    if (!v || index < 0 || index >= v->size) return 0.0;
    return v->data[index];
}

void nuwan_vector_set(NuwanVector* v, int index, double value) {
    if (!v || index < 0 || index >= v->size) return;
    v->data[index] = value;
}

void nuwan_vector_fill(NuwanVector* v, double value) {
    if (!v) return;
    for (int i = 0; i < v->size; i++) {
        v->data[i] = value;
    }
}

NuwanVector* nuwan_vector_copy(NuwanVector* v) {
    if (!v) return NULL;
    
    NuwanVector* copy = nuwan_vector_create(v->size);
    if (!copy) return NULL;
    
    memcpy(copy->data, v->data, v->size * sizeof(double));
    return copy;
}

// Dot product - SIMD-accelerated with fallback
double nuwan_vector_dot(NuwanVector* a, NuwanVector* b) {
    if (!a || !b || a->size != b->size) return 0.0;
    
#ifdef HAS_AVX2
    // Use SIMD for large vectors (overhead not worth it for small ones)
    if (a->size >= 16) {
        extern double nuwan_simd_dot_product(const double* a, const double* b, int n);
        return nuwan_simd_dot_product(a->data, b->data, a->size);
    }
#endif
    
    // Fallback: scalar C implementation with manual unrolling
    double sum = 0.0;
    int i;
    
    // Process 4 elements at a time for better performance
    for (i = 0; i <= a->size - 4; i += 4) {
        sum += a->data[i] * b->data[i];
        sum += a->data[i+1] * b->data[i+1];
        sum += a->data[i+2] * b->data[i+2];
        sum += a->data[i+3] * b->data[i+3];
    }
    
    // Handle remaining elements
    for (; i < a->size; i++) {
        sum += a->data[i] * b->data[i];
    }
    
    return sum;
}

NuwanVector* nuwan_vector_add(NuwanVector* a, NuwanVector* b) {
    if (!a || !b || a->size != b->size) return NULL;
    
    NuwanVector* result = nuwan_vector_create(a->size);
    if (!result) return NULL;
    
#ifdef HAS_AVX2
    if (a->size >= 16) {
        extern void nuwan_simd_vector_add(const double* a, const double* b, double* result, int n);
        nuwan_simd_vector_add(a->data, b->data, result->data, a->size);
        return result;
    }
#endif
    
    // Fallback: vectorized addition
    for (int i = 0; i < a->size; i++) {
        result->data[i] = a->data[i] + b->data[i];
    }
    
    return result;
}

NuwanVector* nuwan_vector_subtract(NuwanVector* a, NuwanVector* b) {
    if (!a || !b || a->size != b->size) return NULL;
    
    NuwanVector* result = nuwan_vector_create(a->size);
    if (!result) return NULL;
    
    for (int i = 0; i < a->size; i++) {
        result->data[i] = a->data[i] - b->data[i];
    }
    
    return result;
}

NuwanVector* nuwan_vector_scale(NuwanVector* v, double scalar) {
    if (!v) return NULL;
    
    NuwanVector* result = nuwan_vector_create(v->size);
    if (!result) return NULL;
    
#ifdef HAS_AVX2
    if (v->size >= 16) {
        extern void nuwan_simd_vector_scale(const double* x, double scalar, double* result, int n);
        nuwan_simd_vector_scale(v->data, scalar, result->data, v->size);
        return result;
    }
#endif
    
    for (int i = 0; i < v->size; i++) {
        result->data[i] = v->data[i] * scalar;
    }
    
    return result;
}

double nuwan_vector_magnitude(NuwanVector* v) {
    if (!v) return 0.0;
    
    double sum_sq = 0.0;
    
    // Loop unrolling for performance
    int i;
    for (i = 0; i <= v->size - 4; i += 4) {
        sum_sq += v->data[i] * v->data[i];
        sum_sq += v->data[i+1] * v->data[i+1];
        sum_sq += v->data[i+2] * v->data[i+2];
        sum_sq += v->data[i+3] * v->data[i+3];
    }
    
    for (; i < v->size; i++) {
        sum_sq += v->data[i] * v->data[i];
    }
    
    return sqrt(sum_sq);
}

NuwanVector* nuwan_vector_normalize(NuwanVector* v) {
    if (!v) return NULL;
    
    double mag = nuwan_vector_magnitude(v);
    if (mag == 0.0) return NULL;
    
    return nuwan_vector_scale(v, 1.0 / mag);
}

// ============================================================================
// MATRIX OPERATIONS - Cache-optimized row-major layout
// ============================================================================

NuwanMatrix* nuwan_matrix_create(int rows, int cols) {
    NuwanMatrix* m = (NuwanMatrix*)malloc(sizeof(NuwanMatrix));
    if (!m) return NULL;
    
    m->rows = rows;
    m->cols = cols;
    m->capacity = rows * cols;
    
    // Aligned allocation for better cache performance
    m->data = (double*)aligned_alloc(32, rows * cols * sizeof(double));
    if (!m->data) {
        free(m);
        return NULL;
    }
    
    memset(m->data, 0, rows * cols * sizeof(double));
    return m;
}

void nuwan_matrix_free(NuwanMatrix* m) {
    if (m) {
        if (m->data) free(m->data);
        free(m);
    }
}

// Inline helper for row-major indexing
static inline int matrix_index(int row, int col, int cols) {
    return row * cols + col;
}

double nuwan_matrix_get(NuwanMatrix* m, int row, int col) {
    if (!m || row < 0 || row >= m->rows || col < 0 || col >= m->cols) {
        return 0.0;
    }
    return m->data[matrix_index(row, col, m->cols)];
}

void nuwan_matrix_set(NuwanMatrix* m, int row, int col, double value) {
    if (!m || row < 0 || row >= m->rows || col < 0 || col >= m->cols) {
        return;
    }
    m->data[matrix_index(row, col, m->cols)] = value;
}

void nuwan_matrix_fill(NuwanMatrix* m, double value) {
    if (!m) return;
    for (int i = 0; i < m->rows * m->cols; i++) {
        m->data[i] = value;
    }
}

NuwanMatrix* nuwan_matrix_copy(NuwanMatrix* m) {
    if (!m) return NULL;
    
    NuwanMatrix* copy = nuwan_matrix_create(m->rows, m->cols);
    if (!copy) return NULL;
    
    memcpy(copy->data, m->data, m->rows * m->cols * sizeof(double));
    return copy;
}

// Cache-optimized matrix multiplication using blocking
NuwanMatrix* nuwan_matrix_multiply(NuwanMatrix* a, NuwanMatrix* b) {
    if (!a || !b || a->cols != b->rows) return NULL;
    
    NuwanMatrix* result = nuwan_matrix_create(a->rows, b->cols);
    if (!result) return NULL;
    
    // Block size for cache optimization
    const int BLOCK_SIZE = 64;
    
    // Blocked matrix multiplication for better cache utilization
    for (int ii = 0; ii < a->rows; ii += BLOCK_SIZE) {
        for (int jj = 0; jj < b->cols; jj += BLOCK_SIZE) {
            for (int kk = 0; kk < a->cols; kk += BLOCK_SIZE) {
                // Process block
                for (int i = ii; i < ii + BLOCK_SIZE && i < a->rows; i++) {
                    for (int j = jj; j < jj + BLOCK_SIZE && j < b->cols; j++) {
                        double sum = result->data[matrix_index(i, j, result->cols)];
                        for (int k = kk; k < kk + BLOCK_SIZE && k < a->cols; k++) {
                            sum += a->data[matrix_index(i, k, a->cols)] * 
                                   b->data[matrix_index(k, j, b->cols)];
                        }
                        result->data[matrix_index(i, j, result->cols)] = sum;
                    }
                }
            }
        }
    }
    
    return result;
}

NuwanMatrix* nuwan_matrix_add(NuwanMatrix* a, NuwanMatrix* b) {
    if (!a || !b || a->rows != b->rows || a->cols != b->cols) return NULL;
    
    NuwanMatrix* result = nuwan_matrix_create(a->rows, a->cols);
    if (!result) return NULL;
    
    int size = a->rows * a->cols;
    for (int i = 0; i < size; i++) {
        result->data[i] = a->data[i] + b->data[i];
    }
    
    return result;
}

NuwanMatrix* nuwan_matrix_subtract(NuwanMatrix* a, NuwanMatrix* b) {
    if (!a || !b || a->rows != b->rows || a->cols != b->cols) return NULL;
    
    NuwanMatrix* result = nuwan_matrix_create(a->rows, a->cols);
    if (!result) return NULL;
    
    int size = a->rows * a->cols;
    for (int i = 0; i < size; i++) {
        result->data[i] = a->data[i] - b->data[i];
    }
    
    return result;
}

NuwanMatrix* nuwan_matrix_scale(NuwanMatrix* m, double scalar) {
    if (!m) return NULL;
    
    NuwanMatrix* result = nuwan_matrix_create(m->rows, m->cols);
    if (!result) return NULL;
    
    int size = m->rows * m->cols;
    for (int i = 0; i < size; i++) {
        result->data[i] = m->data[i] * scalar;
    }
    
    return result;
}

NuwanMatrix* nuwan_matrix_transpose(NuwanMatrix* m) {
    if (!m) return NULL;
    
    NuwanMatrix* result = nuwan_matrix_create(m->cols, m->rows);
    if (!result) return NULL;
    
    // Cache-friendly transpose
    for (int i = 0; i < m->rows; i++) {
        for (int j = 0; j < m->cols; j++) {
            result->data[matrix_index(j, i, result->cols)] = 
                m->data[matrix_index(i, j, m->cols)];
        }
    }
    
    return result;
}

NuwanVector* nuwan_matrix_vector_multiply(NuwanMatrix* m, NuwanVector* v) {
    if (!m || !v || m->cols != v->size) return NULL;
    
    NuwanVector* result = nuwan_vector_create(m->rows);
    if (!result) return NULL;
    
    for (int i = 0; i < m->rows; i++) {
        double sum = 0.0;
        for (int j = 0; j < m->cols; j++) {
            sum += m->data[matrix_index(i, j, m->cols)] * v->data[j];
        }
        result->data[i] = sum;
    }
    
    return result;
}

// Determinant using LU decomposition for efficiency
double nuwan_matrix_determinant(NuwanMatrix* m) {
    if (!m || m->rows != m->cols) return 0.0;
    
    int n = m->rows;
    if (n == 1) return m->data[0];
    if (n == 2) {
        return m->data[0] * m->data[3] - m->data[1] * m->data[2];
    }
    
    // For larger matrices, use LU decomposition (simplified implementation)
    NuwanMatrix* temp = nuwan_matrix_copy(m);
    if (!temp) return 0.0;
    
    double det = 1.0;
    
    // Gaussian elimination
    for (int i = 0; i < n; i++) {
        // Find pivot
        int pivot = i;
        for (int j = i + 1; j < n; j++) {
            if (fabs(temp->data[matrix_index(j, i, n)]) > 
                fabs(temp->data[matrix_index(pivot, i, n)])) {
                pivot = j;
            }
        }
        
        // Swap rows if needed
        if (pivot != i) {
            for (int j = 0; j < n; j++) {
                double tmp = temp->data[matrix_index(i, j, n)];
                temp->data[matrix_index(i, j, n)] = temp->data[matrix_index(pivot, j, n)];
                temp->data[matrix_index(pivot, j, n)] = tmp;
            }
            det = -det;
        }
        
        double pivot_val = temp->data[matrix_index(i, i, n)];
        if (fabs(pivot_val) < 1e-10) {
            nuwan_matrix_free(temp);
            return 0.0;
        }
        
        det *= pivot_val;
        
        // Eliminate column
        for (int j = i + 1; j < n; j++) {
            double factor = temp->data[matrix_index(j, i, n)] / pivot_val;
            for (int k = i; k < n; k++) {
                temp->data[matrix_index(j, k, n)] -= 
                    factor * temp->data[matrix_index(i, k, n)];
            }
        }
    }
    
    nuwan_matrix_free(temp);
    return det;
}

// Matrix inverse using Gauss-Jordan elimination
NuwanMatrix* nuwan_matrix_inverse(NuwanMatrix* m) {
    if (!m || m->rows != m->cols) return NULL;
    
    int n = m->rows;
    
    // Create augmented matrix [A | I]
    NuwanMatrix* aug = nuwan_matrix_create(n, 2 * n);
    if (!aug) return NULL;
    
    // Copy original matrix to left half
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            aug->data[matrix_index(i, j, 2*n)] = m->data[matrix_index(i, j, n)];
        }
    }
    
    // Identity matrix on right half
    for (int i = 0; i < n; i++) {
        aug->data[matrix_index(i, n + i, 2*n)] = 1.0;
    }
    
    // Gauss-Jordan elimination
    for (int i = 0; i < n; i++) {
        // Find pivot
        double pivot = aug->data[matrix_index(i, i, 2*n)];
        if (fabs(pivot) < 1e-10) {
            nuwan_matrix_free(aug);
            return NULL; // Singular matrix
        }
        
        // Scale row
        for (int j = 0; j < 2 * n; j++) {
            aug->data[matrix_index(i, j, 2*n)] /= pivot;
        }
        
        // Eliminate column
        for (int k = 0; k < n; k++) {
            if (k != i) {
                double factor = aug->data[matrix_index(k, i, 2*n)];
                for (int j = 0; j < 2 * n; j++) {
                    aug->data[matrix_index(k, j, 2*n)] -= 
                        factor * aug->data[matrix_index(i, j, 2*n)];
                }
            }
        }
    }
    
    // Extract inverse from right half
    NuwanMatrix* result = nuwan_matrix_create(n, n);
    if (!result) {
        nuwan_matrix_free(aug);
        return NULL;
    }
    
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            result->data[matrix_index(i, j, n)] = 
                aug->data[matrix_index(i, n + j, 2*n)];
        }
    }
    
    nuwan_matrix_free(aug);
    return result;
}

// Factory methods
NuwanMatrix* nuwan_matrix_identity(int size) {
    NuwanMatrix* m = nuwan_matrix_create(size, size);
    if (!m) return NULL;
    
    for (int i = 0; i < size; i++) {
        m->data[matrix_index(i, i, size)] = 1.0;
    }
    
    return m;
}

NuwanMatrix* nuwan_matrix_zeros(int rows, int cols) {
    return nuwan_matrix_create(rows, cols); // Already zeros
}

NuwanMatrix* nuwan_matrix_ones(int rows, int cols) {
    NuwanMatrix* m = nuwan_matrix_create(rows, cols);
    if (!m) return NULL;
    
    nuwan_matrix_fill(m, 1.0);
    return m;
}
