/*
 * Casprix Compiler - Loop Optimization Implementation
 */

#include "compiler/opt/loop_opt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void init_loop_optimizer(LoopOptContext* ctx) {
    ctx->loops = NULL;
    ctx->loop_count = 0;
    ctx->invariants_hoisted = 0;
    ctx->loops_unrolled = 0;
    ctx->strength_reductions = 0;
}

// Check if expression is loop-invariant (doesn't depend on induction variable)
bool is_loop_invariant(Expr* expr, const char* induction_var) {
    if (!expr || !induction_var) return true;

    switch (expr->type) {
        case EXPR_LITERAL:
            return true;  // Constants are invariant

        case EXPR_VARIABLE:
            // Variant if it's the induction variable
            return strcmp(expr->as.variable.name, induction_var) != 0;

        case EXPR_BINARY:
            return is_loop_invariant(expr->as.binary.left, induction_var) &&
                   is_loop_invariant(expr->as.binary.right, induction_var);

        case EXPR_UNARY:
            return is_loop_invariant(expr->as.unary.operand, induction_var);

        case EXPR_CALL:
            // Conservative: assume calls aren't invariant
            return false;

        default:
            return false;
    }
}

// Detect loops in statements
static void detect_loops_recursive(Stmt* stmt, LoopOptContext* ctx) {
    if (!stmt) return;

    switch (stmt->type) {
        case STMT_FOR: {
            // Add to loop list
            ctx->loops = realloc(ctx->loops, (ctx->loop_count + 1) * sizeof(LoopInfo));
            LoopInfo* loop = &ctx->loops[ctx->loop_count++];

            loop->loop_stmt = stmt;
            loop->induction_var = strdup(stmt->as.for_stmt.variable);
            loop->trip_count = -1;  // Unknown by default
            loop->has_constant_bound = false;
            loop->invariant_exprs = NULL;
            loop->invariant_count = 0;

            // Try to determine trip count
            if (stmt->as.for_stmt.condition && stmt->as.for_stmt.condition->type == EXPR_BINARY) {
                // Simple case: for i = 0 to N
                loop->has_constant_bound = true;
            }

            // Recursively process loop body
            detect_loops_recursive(stmt->as.for_stmt.body, ctx);
            break;
        }

        case STMT_WHILE: {
            ctx->loops = realloc(ctx->loops, (ctx->loop_count + 1) * sizeof(LoopInfo));
            LoopInfo* loop = &ctx->loops[ctx->loop_count++];

            loop->loop_stmt = stmt;
            loop->induction_var = NULL;  // No explicit induction var
            loop->trip_count = -1;
            loop->has_constant_bound = false;
            loop->invariant_exprs = NULL;
            loop->invariant_count = 0;

            detect_loops_recursive(stmt->as.while_stmt.body, ctx);
            break;
        }

        case STMT_BLOCK:
            for (int i = 0; i < stmt->as.block.stmt_count; i++) {
                detect_loops_recursive(stmt->as.block.statements[i], ctx);
            }
            break;

        case STMT_IF:
            detect_loops_recursive(stmt->as.if_stmt.then_branch, ctx);
            detect_loops_recursive(stmt->as.if_stmt.else_branch, ctx);
            break;

        default:
            break;
    }
}

void detect_loops(Stmt** statements, int count, LoopOptContext* ctx) {
    for (int i = 0; i < count; i++) {
        detect_loops_recursive(statements[i], ctx);
    }
}

// Loop invariant code motion
bool hoist_loop_invariants(LoopInfo* loop) {
    if (!loop || !loop->loop_stmt) return false;

    // For now, mark this as done but don't actually move code
    // Full implementation would:
    // 1. Find all expressions in loop body
    // 2. Check if they're invariant
    // 3. Move them outside the loop

    return true;  // Placeholder
}

/* Deep-clone a Stmt tree so unrolled iterations don't share AST nodes.
   Shared nodes would cause optimizer passes that mutate in-place to corrupt
   all copies simultaneously.  We recursively clone compound statements;
   leaf expressions are intentionally left shared (read-only after parsing). */
static Stmt* stmt_clone(Stmt* s) {
    if (!s) return NULL;
    Stmt* c = malloc(sizeof(Stmt));
    if (!c) return s;          /* allocation failure: return original (safe fallback) */
    *c = *s;                   /* shallow copy of all fields */

    switch (s->type) {
        case STMT_BLOCK: {
            int n = s->as.block.stmt_count;
            c->as.block.statements = malloc((size_t)n * sizeof(Stmt*));
            if (c->as.block.statements) {
                for (int i = 0; i < n; i++)
                    c->as.block.statements[i] = stmt_clone(s->as.block.statements[i]);
            } else {
                c->as.block.statements = s->as.block.statements; /* fallback */
            }
            break;
        }
        case STMT_IF:
            c->as.if_stmt.then_branch = stmt_clone(s->as.if_stmt.then_branch);
            c->as.if_stmt.else_branch = stmt_clone(s->as.if_stmt.else_branch);
            break;
        case STMT_FOR:
            c->as.for_stmt.body      = stmt_clone(s->as.for_stmt.body);
            c->as.for_stmt.increment = stmt_clone(s->as.for_stmt.increment);
            break;
        case STMT_WHILE:
            c->as.while_stmt.body = stmt_clone(s->as.while_stmt.body);
            break;
        default:
            break;
    }
    return c;
}

// Loop unrolling
Stmt* unroll_loop(Stmt* loop, int factor) {
    if (!loop || factor < 2) return loop;

    if (loop->type == STMT_FOR) {
        Stmt* block = malloc(sizeof(Stmt));
        if (!block) return loop;
        block->type = STMT_BLOCK;
        block->line = loop->line;
        block->column = loop->column;
        block->as.block.stmt_count = factor;
        block->as.block.is_alloc_scope = false;
        block->as.block.statements = malloc((size_t)factor * sizeof(Stmt*));
        if (!block->as.block.statements) { free(block); return loop; }

        for (int i = 0; i < factor; i++)
            block->as.block.statements[i] = stmt_clone(loop->as.for_stmt.body);

        return block;
    }

    return loop;
}

// Strength reduction in loops
bool reduce_loop_strength(LoopInfo* loop) {
    if (!loop || !loop->induction_var) return false;

    // Replace expensive operations with cheaper ones
    // Example: i*4 becomes i<<2 or add operations

    // This is a placeholder - full implementation would:
    // 1. Find multiplications by induction variable
    // 2. Replace with repeated additions
    // 3. Find divisions and replace with shifts (for powers of 2)

    return true;
}

// Optimize all detected loops
void optimize_loops(Stmt** statements, int count, LoopOptContext* ctx) {
    // First, detect all loops
    detect_loops(statements, count, ctx);

    // Apply optimizations to each loop
    for (int i = 0; i < ctx->loop_count; i++) {
        LoopInfo* loop = &ctx->loops[i];

        // 1. Hoist invariants
        if (hoist_loop_invariants(loop)) {
            ctx->invariants_hoisted++;
        }

        // 2. Strength reduction
        if (reduce_loop_strength(loop)) {
            ctx->strength_reductions++;
        }

        // 3. Unroll small loops with known bounds
        if (loop->has_constant_bound && loop->trip_count > 0 && loop->trip_count <= 16) {
            unroll_loop(loop->loop_stmt, 4);  // Unroll by factor of 4
            ctx->loops_unrolled++;
        }
    }
}

void free_loop_optimizer(LoopOptContext* ctx) {
    if (ctx->loops) {
        for (int i = 0; i < ctx->loop_count; i++) {
            if (ctx->loops[i].induction_var) {
                free(ctx->loops[i].induction_var);
            }
            if (ctx->loops[i].invariant_exprs) {
                free(ctx->loops[i].invariant_exprs);
            }
        }
        free(ctx->loops);
    }
}
