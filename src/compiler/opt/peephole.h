/*
 * Casprix Compiler - Peephole Optimizer
 * Local instruction-level optimizations
 */

#ifndef PEEPHOLE_H
#define PEEPHOLE_H

#include <stdbool.h>
#include <stdio.h>

// Assembly instruction
typedef struct {
    char* opcode;
    char* operand1;
    char* operand2;
    char* comment;
    int line_number;
} AsmInstr;

// Peephole context
typedef struct {
    AsmInstr* instructions;
    int instr_count;
    int instr_capacity;
    int optimizations_applied;
} PeepholeContext;

// Initialize peephole optimizer
void init_peephole(PeepholeContext* ctx);

// Add instruction
void peephole_add_instr(PeepholeContext* ctx, const char* opcode,
                        const char* op1, const char* op2, const char* comment);

// Run peephole optimization passes
void peephole_optimize(PeepholeContext* ctx);

// Specific optimization patterns
bool peephole_remove_redundant_moves(PeepholeContext* ctx);
bool peephole_combine_arithmetic(PeepholeContext* ctx);
bool peephole_eliminate_dead_stores(PeepholeContext* ctx);
bool peephole_use_xor_for_zero(PeepholeContext* ctx);
bool peephole_strength_reduce(PeepholeContext* ctx);

// Emit optimized assembly
void peephole_emit(PeepholeContext* ctx, FILE* out);

// Free peephole context
void free_peephole(PeepholeContext* ctx);

#endif // PEEPHOLE_H
