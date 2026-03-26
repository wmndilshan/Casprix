/*
 * Casprix Compiler - Function Inlining Implementation
 */

#include "compiler/opt/inline.h"
#include <stdlib.h>
#include <string.h>

#define MAX_INLINE_SIZE 30      // Max statements for inline
#define MIN_CALL_COUNT 2        // Min calls to consider inlining

void init_inliner(InlineContext* ctx) {
    ctx->candidates = NULL;
    ctx->candidate_count = 0;
    ctx->functions_inlined = 0;
    ctx->call_sites_inlined = 0;
}

// Estimate code size of a statement
static int estimate_stmt_size(Stmt* stmt) {
    if (!stmt) return 0;

    int size = 1;  // Base cost

    switch (stmt->type) {
        case STMT_BLOCK:
            for (int i = 0; i < stmt->as.block.stmt_count; i++) {
                size += estimate_stmt_size(stmt->as.block.statements[i]);
            }
            break;

        case STMT_IF:
            size += estimate_stmt_size(stmt->as.if_stmt.then_branch);
            size += estimate_stmt_size(stmt->as.if_stmt.else_branch);
            size += 2;  // Branch overhead
            break;

        case STMT_WHILE:
            size += estimate_stmt_size(stmt->as.while_stmt.body);
            size += 5;  // Loop overhead
            break;

        case STMT_FOR:
            size += estimate_stmt_size(stmt->as.for_stmt.body);
            size += 10;  // Loops are expensive
            break;

        case STMT_FUNCTION:
            size += 50;  // Don't inline functions with nested functions
            break;

        default:
            size += 1;
            break;
    }

    return size;
}

// Check for recursion
static bool is_recursive(FunctionStmt* func, const char* func_name) {
    (void)func;
    (void)func_name;
    // Simple check - full implementation would traverse call graph
    return false;  // Placeholder
}

// Analyze function
InlineCandidate* analyze_for_inlining(FunctionStmt* func) {
    InlineCandidate* candidate = malloc(sizeof(InlineCandidate));
    candidate->function = func;
    candidate->size_estimate = estimate_stmt_size(func->body);
    candidate->call_count = 0;  // Will be filled by caller
    candidate->is_recursive = is_recursive(func, func->name);
    candidate->should_inline = false;

    return candidate;
}

// Decide if should inline
bool should_inline_function(FunctionStmt* func, int call_count) {
    if (!func || !func->body) return false;

    int size = estimate_stmt_size(func->body);

    // Inline criteria:
    // 1. Small functions (< 30 statements)
    // 2. Called multiple times (>= 2)
    // 3. Not recursive
    // 4. No nested functions

    if (size > MAX_INLINE_SIZE) return false;
    if (call_count < MIN_CALL_COUNT) return false;
    if (is_recursive(func, func->name)) return false;

    return true;
}

// Perform inline expansion (simplified)
Expr* inline_function_call(Expr* call_expr, FunctionStmt* func) {
    (void)func;
    if (!call_expr || call_expr->type != EXPR_CALL) return call_expr;

    // Full implementation would:
    // 1. Create local copies of parameters
    // 2. Substitute parameter references with arguments
    // 3. Insert function body inline
    // 4. Handle return values

    // For now, return original expression
    return call_expr;
}

// Recursive search for function calls
static void find_call_sites(Stmt* stmt, InlineContext* ctx) {
    if (!stmt) return;

    switch (stmt->type) {
        case STMT_EXPR:
            if (stmt->as.expr_stmt.expression && stmt->as.expr_stmt.expression->type == EXPR_CALL) {
                // Found a call - update call count
                // Would need to match against known functions
            }
            break;

        case STMT_BLOCK:
            for (int i = 0; i < stmt->as.block.stmt_count; i++) {
                find_call_sites(stmt->as.block.statements[i], ctx);
            }
            break;

        case STMT_IF:
            find_call_sites(stmt->as.if_stmt.then_branch, ctx);
            find_call_sites(stmt->as.if_stmt.else_branch, ctx);
            break;

        case STMT_WHILE:
            find_call_sites(stmt->as.while_stmt.body, ctx);
            break;

        case STMT_FOR:
            find_call_sites(stmt->as.for_stmt.body, ctx);
            break;

        case STMT_RETURN:
            if (stmt->as.return_stmt.value && stmt->as.return_stmt.value->type == EXPR_CALL) {
                // Found a call in return
            }
            break;

        case STMT_PRINT:
            if (stmt->as.print.expression && stmt->as.print.expression->type == EXPR_CALL) {
                // Found a call in print
            }
            break;

        default:
            break;
    }
}

// Perform all inlining
void perform_inlining(Stmt** statements, int count, InlineContext* ctx) {
    // Phase 1: Identify inline candidates
    for (int i = 0; i < count; i++) {
        if (statements[i]->type == STMT_FUNCTION) {
            InlineCandidate* candidate = analyze_for_inlining(&statements[i]->as.function);

            ctx->candidates = realloc(ctx->candidates,
                                     (ctx->candidate_count + 1) * sizeof(InlineCandidate));
            ctx->candidates[ctx->candidate_count++] = *candidate;
            free(candidate);
        }
    }

    // Phase 2: Count call sites
    for (int i = 0; i < count; i++) {
        find_call_sites(statements[i], ctx);
    }

    // Phase 3: Decide what to inline
    for (int i = 0; i < ctx->candidate_count; i++) {
        InlineCandidate* c = &ctx->candidates[i];
        c->should_inline = should_inline_function(c->function, c->call_count);

        if (c->should_inline) {
            ctx->functions_inlined++;
        }
    }

    // Phase 4: Actually inline (would modify AST)
    // Placeholder - full implementation would rewrite call sites
}

void free_inliner(InlineContext* ctx) {
    if (ctx->candidates) {
        free(ctx->candidates);
    }
}
