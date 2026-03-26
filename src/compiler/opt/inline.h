/*
 * Casprix Compiler - Function Inlining
 * Automatic inline expansion for small functions
 */

#ifndef INLINE_H
#define INLINE_H

#include "compiler/frontend/ast.h"
#include <stdbool.h>

// Inline candidate
typedef struct {
    FunctionStmt* function;
    int size_estimate;      // Estimated code size
    int call_count;         // Number of call sites
    bool is_recursive;
    bool should_inline;
} InlineCandidate;

// Inlining context
typedef struct {
    InlineCandidate* candidates;
    int candidate_count;
    int functions_inlined;
    int call_sites_inlined;
} InlineContext;

// Initialize inliner
void init_inliner(InlineContext* ctx);

// Analyze function for inlining
InlineCandidate* analyze_for_inlining(FunctionStmt* func);

// Check if function should be inlined
bool should_inline_function(FunctionStmt* func, int call_count);

// Perform inline expansion
Expr* inline_function_call(Expr* call_expr, FunctionStmt* func);

// Inline all suitable functions
void perform_inlining(Stmt** statements, int count, InlineContext* ctx);

// Free inliner
void free_inliner(InlineContext* ctx);

#endif // INLINE_H
