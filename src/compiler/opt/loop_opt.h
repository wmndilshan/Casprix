/*
 * Casprix Compiler - Loop Optimization
 * Implements loop invariant code motion, unrolling, and strength reduction
 */

#ifndef LOOP_OPT_H
#define LOOP_OPT_H

#include "compiler/frontend/ast.h"
#include <stdbool.h>

// Loop metadata
typedef struct {
    Stmt* loop_stmt;           // The loop statement (FOR or WHILE)
    char* induction_var;       // Primary induction variable (if any)
    int trip_count;            // Number of iterations (-1 if unknown)
    bool has_constant_bound;   // True if loop bound is constant
    Expr** invariant_exprs;    // Loop-invariant expressions
    int invariant_count;
} LoopInfo;

// Loop optimizer context
typedef struct {
    LoopInfo* loops;
    int loop_count;
    int invariants_hoisted;
    int loops_unrolled;
    int strength_reductions;
} LoopOptContext;

// Initialize loop optimizer
void init_loop_optimizer(LoopOptContext* ctx);

// Detect loops in AST
void detect_loops(Stmt** statements, int count, LoopOptContext* ctx);

// Loop invariant code motion
bool hoist_loop_invariants(LoopInfo* loop);

// Loop unrolling
Stmt* unroll_loop(Stmt* loop, int factor);

// Strength reduction in loops
bool reduce_loop_strength(LoopInfo* loop);

// Check if expression is loop-invariant
bool is_loop_invariant(Expr* expr, const char* induction_var);

// Optimize all loops
void optimize_loops(Stmt** statements, int count, LoopOptContext* ctx);

// Free loop optimizer
void free_loop_optimizer(LoopOptContext* ctx);

#endif // LOOP_OPT_H
