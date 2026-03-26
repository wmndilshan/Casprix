/*
 * Casprix Compiler - SIMD Vectorization
 * Auto-vectorization for loops using AVX2 instructions
 */

#ifndef SIMD_H
#define SIMD_H

#include "compiler/frontend/ast.h"
#include <stdbool.h>

// Vectorization context
typedef struct {
    bool avx2_available;
    bool avx512_available;
    int vector_width;      // 4 for AVX2 (128-bit), 8 for AVX2 (256-bit)
    int loops_vectorized;
    int scalar_fallbacks;
} VectorContext;

// Initialize vectorizer
void init_vectorizer(VectorContext* ctx);

// Check if loop can be vectorized
bool can_vectorize_loop(Stmt* loop);

// Vectorize a loop
bool vectorize_loop(Stmt* loop, VectorContext* ctx);

// Generate AVX2 code for operation
void emit_avx2_operation(const char* op, const char* dest, const char* src1, 
                         const char* src2, FILE* out);

// Generate alignment check
void emit_alignment_check(const char* ptr, FILE* out);

#endif // SIMD_H
