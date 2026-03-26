/*
 * Casprix Compiler - SIMD Vectorization Implementation
 */

#include "compiler/opt/simd.h"
#include <stdio.h>
#include <string.h>

void init_vectorizer(VectorContext* ctx) {
    // Detect CPU capabilities (simplified - always assume AVX2)
    ctx->avx2_available = true;
    ctx->avx512_available = false;
    ctx->vector_width = 4;  // Process 4 elements at once (256-bit / 64-bit)
    ctx->loops_vectorized = 0;
    ctx->scalar_fallbacks = 0;
}

// Check if loop can be vectorized
bool can_vectorize_loop(Stmt* loop) {
    if (!loop) return false;

    // Requirements for vectorization:
    // 1. Must be a FOR loop with known bounds
    // 2. No loop-carried dependencies
    // 3. Simple array operations
    // 4. No function calls (simplified)

    if (loop->type != STMT_FOR) {
        return false;
    }

    // Check for simple induction variable
    if (!loop->as.for_stmt.variable) {
        return false;
    }

    // For now, assume vectorizable if it's a for loop
    // Full implementation would do dependency analysis
    return true;
}

// Vectorize a loop
bool vectorize_loop(Stmt* loop, VectorContext* ctx) {
    if (!can_vectorize_loop(loop)) {
        ctx->scalar_fallbacks++;
        return false;
    }

    // Mark as vectorized
    ctx->loops_vectorized++;

    // Actual vectorization would:
    // 1. Generate vector load instructions (ymm registers)
    // 2. Generate vector operations
    // 3. Generate vector store instructions
    // 4. Handle remainder loop for non-multiple-of-4 iterations

    return true;
}

// Generate AVX2 code
void emit_avx2_operation(const char* op, const char* dest, const char* src1,
                         const char* src2, FILE* out) {
    // Map operation to AVX2 instruction
    if (strcmp(op, "add") == 0) {
        fprintf(out, "    vaddpd %s, %s, %s  ; AVX2 add (4x double)\n", dest, src1, src2);
    } else if (strcmp(op, "mul") == 0) {
        fprintf(out, "    vmulpd %s, %s, %s  ; AVX2 multiply\n", dest, src1, src2);
    } else if (strcmp(op, "sub") == 0) {
        fprintf(out, "    vsubpd %s, %s, %s  ; AVX2 subtract\n", dest, src1, src2);
    } else if (strcmp(op, "div") == 0) {
        fprintf(out, "    vdivpd %s, %s, %s  ; AVX2 divide\n", dest, src1, src2);
    }
}

// Generate alignment check
void emit_alignment_check(const char* ptr, FILE* out) {
    fprintf(out, "    ; Check %s alignment for AVX2\n", ptr);
    fprintf(out, "    mov rax, %s\n", ptr);
    fprintf(out, "    and rax, 31        ; Check 32-byte alignment\n");
    fprintf(out, "    jnz scalar_fallback ; Use scalar if not aligned\n");
}

// Example: Vectorize array addition
void emit_vector_array_add(const char* dest, const char* src1, const char* src2,
                           int count, FILE* out) {
    fprintf(out, "    ; Vectorized array addition (%d elements)\n", count);
    fprintf(out, "    mov rcx, %d\n", count / 4);  // Process 4 at a time
    fprintf(out, "    xor rdi, rdi\n");
    fprintf(out, "\n");
    fprintf(out, "vector_loop:\n");
    fprintf(out, "    vmovapd ymm0, [%s + rdi*8]  ; Load 4 doubles from src1\n", src1);
    fprintf(out, "    vmovapd ymm1, [%s + rdi*8]  ; Load 4 doubles from src2\n", src2);
    fprintf(out, "    vaddpd ymm2, ymm0, ymm1      ; Add\n");
    fprintf(out, "    vmovapd [%s + rdi*8], ymm2   ; Store result\n", dest);
    fprintf(out, "    add rdi, 4\n");
    fprintf(out, "    loop vector_loop\n");
    fprintf(out, "\n");

    // Handle remainder
    int remainder = count % 4;
    if (remainder > 0) {
        fprintf(out, "    ; Scalar remainder (%d elements)\n", remainder);
        fprintf(out, "scalar_remainder:\n");
        for (int i = 0; i < remainder; i++) {
            fprintf(out, "    movsd xmm0, [%s + rdi*8 + %d]\n", src1, i*8);
            fprintf(out, "    addsd xmm0, [%s + rdi*8 + %d]\n", src2, i*8);
            fprintf(out, "    movsd [%s + rdi*8 + %d], xmm0\n", dest, i*8);
        }
    }
}
