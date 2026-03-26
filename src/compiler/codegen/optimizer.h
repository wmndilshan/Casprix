// Casprix Compiler Optimizer
// Constant folding, dead code elimination, and other optimizations

#ifndef OPTIMIZER_H
#define OPTIMIZER_H

#include "compiler/frontend/ast.h"
#include "compiler/sema/symtable.h"
#include <stdbool.h>

// Optimization context
typedef struct {
    int optimizations_performed;
    int constants_folded;
    int dead_code_eliminated;
    int expressions_simplified;
    bool enable_constant_folding;
    bool enable_dead_code_elimination;
    bool enable_common_subexpression;
    bool enable_inline_expansion;
} OptimizerContext;

// Initialize optimizer
void init_optimizer(OptimizerContext* ctx);

// Main optimization pass
void optimize_program(Stmt** statements, int count, OptimizerContext* ctx);

// Constant folding
Expr* fold_constants(Expr* expr, OptimizerContext* ctx);
bool is_constant_expr(Expr* expr);
int64_t evaluate_constant_int(Expr* expr);
double evaluate_constant_float(Expr* expr);

// Dead code elimination
void eliminate_dead_code(Stmt** statements, int* count, OptimizerContext* ctx);
bool is_unreachable_code(Stmt* stmt);

// Common subexpression elimination
void eliminate_common_subexpressions(Stmt** statements, int count, OptimizerContext* ctx);

// Algebraic simplification
Expr* simplify_expr(Expr* expr, OptimizerContext* ctx);

// Strength reduction (replace expensive ops with cheaper ones)
Expr* reduce_strength(Expr* expr, OptimizerContext* ctx);

// Inline expansion (for small functions)
bool inline_function_call(Expr* call_expr, OptimizerContext* ctx);

// Loop optimizations
void optimize_loops(Stmt* stmt, OptimizerContext* ctx);

// Print optimization statistics
void print_optimizer_stats(OptimizerContext* ctx);

#endif // OPTIMIZER_H
