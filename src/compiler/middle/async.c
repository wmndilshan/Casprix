/*
 * Casprix Compiler - Async/Await Implementation
 * Transforms async functions into state machines
 */

#include "compiler/middle/async.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void init_async(AsyncContext* ctx) {
    ctx->async_functions = NULL;
    ctx->async_count = 0;
    ctx->state_machines_generated = 0;
}

typedef struct {
    const char* name;
    DataType type;
} LocalVar;

// static void collect_locals_stmt(Stmt* stmt, LocalVar** locals, int* count);

/*
// Collect all local variables (including parameters) that need to be in state struct
static void collect_locals(FunctionStmt* func, LocalVar** locals, int* count) {
    // Add parameters first
    for (int i = 0; i < func->param_count; i++) {
        LocalVar* new_locals = realloc(*locals, (*count + 1) * sizeof(LocalVar));
        if (!new_locals) return;
        *locals = new_locals;
        (*locals)[*count].name = func->parameters[i].name;
        (*locals)[*count].type = func->parameters[i].type;
        (*count)++;
    }

    // Add variables declared in body
    collect_locals_stmt(func->body, locals, count);
}
*/

/*
static void collect_locals_stmt(Stmt* stmt, LocalVar** locals, int* count) {
    if (!stmt) return;
    
    switch (stmt->type) {
        case STMT_DECL: {
            LocalVar* new_locals = realloc(*locals, (*count + 1) * sizeof(LocalVar));
            if (!new_locals) return;
            *locals = new_locals;
            (*locals)[*count].name = stmt->as.decl.name;
            (*locals)[*count].type = stmt->as.decl.type;
            (*count)++;
            break;
        }
        case STMT_BLOCK: {
            Stmt* s = stmt->as.block.first;
            while (s) {
                collect_locals_stmt(s, locals, count);
                s = s->next;
            }
            break;
        }
        case STMT_IF:
            collect_locals_stmt(stmt->as.if_stmt.then_branch, locals, count);
            if (stmt->as.if_stmt.else_branch)
                collect_locals_stmt(stmt->as.if_stmt.else_branch, locals, count);
            break;
        case STMT_WHILE:
            collect_locals_stmt(stmt->as.while_stmt.body, locals, count);
            break;
        case STMT_FOR:
            collect_locals_stmt(stmt->as.for_stmt.body, locals, count);
            break;
        default:
            break;
    }
}
*/

// Mark function as async

// Find all await expressions in statement
static void find_await_points(Stmt* stmt, Expr*** awaits, int* count) {
    if (!stmt) return;

    switch (stmt->type) {
        case STMT_EXPR:
            if (stmt->as.expr_stmt.expression && stmt->as.expr_stmt.expression->type == EXPR_AWAIT) {
                Expr** new_awaits = realloc(*awaits, (*count + 1) * sizeof(Expr*));
                if (!new_awaits) break;
                *awaits = new_awaits;
                (*awaits)[(*count)++] = stmt->as.expr_stmt.expression;
            }
            break;

        case STMT_ASSIGNMENT:
            if (stmt->as.assignment.value && stmt->as.assignment.value->type == EXPR_AWAIT) {
                Expr** new_awaits = realloc(*awaits, (*count + 1) * sizeof(Expr*));
                if (!new_awaits) break;
                *awaits = new_awaits;
                (*awaits)[(*count)++] = stmt->as.assignment.value;
            }
            break;

        case STMT_DECLARATION:
            if (stmt->as.declaration.initializer && stmt->as.declaration.initializer->type == EXPR_AWAIT) {
                Expr** new_awaits = realloc(*awaits, (*count + 1) * sizeof(Expr*));
                if (!new_awaits) break;
                *awaits = new_awaits;
                (*awaits)[(*count)++] = stmt->as.declaration.initializer;
            }
            break;

        case STMT_BLOCK:
            for (int i = 0; i < stmt->as.block.stmt_count; i++) {
                find_await_points(stmt->as.block.statements[i], awaits, count);
            }
            break;

        case STMT_IF:
            find_await_points(stmt->as.if_stmt.then_branch, awaits, count);
            find_await_points(stmt->as.if_stmt.else_branch, awaits, count);
            break;

        case STMT_WHILE:
            find_await_points(stmt->as.while_stmt.body, awaits, count);
            break;

        case STMT_FOR:
            find_await_points(stmt->as.for_stmt.body, awaits, count);
            break;

        case STMT_RETURN:
            if (stmt->as.return_stmt.value && stmt->as.return_stmt.value->type == EXPR_AWAIT) {
                Expr** new_awaits = realloc(*awaits, (*count + 1) * sizeof(Expr*));
                if (!new_awaits) break;
                *awaits = new_awaits;
                (*awaits)[(*count)++] = stmt->as.return_stmt.value;
            }
            break;

        default:
            break;
    }
}

// Transform async function to state machine
FunctionStmt* transform_async_function(FunctionStmt* async_func, AsyncContext* ctx) {
    if (!async_func) return NULL;

    /*
     * Transformation strategy:
     *
     * async Func foo() -> Int
     *     Var x = Await bar()
     *     Var y = Await baz()
     *     Return x + y
     * End
     *
     * Becomes:
     *
     * Func foo_state_machine(state: Ptr, resume_point: Int) -> Promise<Int>
     *     Switch resume_point
     *         Case 0:
     *             state.promise = bar()
     *             Return Pending(1)  // Resume at point 1
     *         Case 1:
     *             state.x = state.promise.result
     *             state.promise = baz()
     *             Return Pending(2)
     *         Case 2:
     *             state.y = state.promise.result
     *             Return Ready(state.x + state.y)
     *     End
     * End
     */

    // Create metadata
    AsyncMeta* meta = malloc(sizeof(AsyncMeta));
    meta->function = async_func;
    meta->is_async = true;
    meta->await_points = NULL;
    meta->await_count = 0;

    // Generate state machine name
    char sm_name[256];
    snprintf(sm_name, sizeof(sm_name), "%s_async_sm", async_func->name);
    meta->state_machine_name = strdup(sm_name);

    // Find await points
    find_await_points(async_func->body, &meta->await_points, &meta->await_count);

    // Add to context
    AsyncMeta** new_funcs = realloc(ctx->async_functions,
                                    (ctx->async_count + 1) * sizeof(AsyncMeta*));
    if (!new_funcs) {
        free(meta->state_machine_name);
        free(meta);
        return async_func;
    }
    ctx->async_functions = new_funcs;
    ctx->async_functions[ctx->async_count++] = meta;
    ctx->state_machines_generated++;

    printf("Transformed %s to state machine with %d await points\n",
           async_func->name, meta->await_count);

    // Return modified function (simplified - would create new AST)
    return async_func;
}

// Generate await handling code
Stmt* generate_await(Expr* awaited_expr, const char* result_var) {
    (void)awaited_expr;
    (void)result_var;

    /*
     * await expr
     *
     * Becomes:
     *
     * promise = expr
     * while !promise.is_ready() do
     *     yield_to_runtime()
     * end
     * result_var = promise.get_result()
     */

    // Would generate actual AST here
    return NULL;  // Placeholder
}

// Check if expression is await
bool is_await_expr(Expr* expr) {
    return expr && expr->type == EXPR_AWAIT;
}

void free_async(AsyncContext* ctx) {
    if (!ctx) return;

    for (int i = 0; i < ctx->async_count; i++) {
        AsyncMeta* meta = ctx->async_functions[i];
        free(meta->state_machine_name);
        if (meta->await_points) {
            free(meta->await_points);
        }
        free(meta);
    }

    free(ctx->async_functions);
}
