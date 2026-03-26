/**
 * Casperix Compiler - Async/Await Lowering Implementation
 */

#include "async_lowering.h"
#include "support/log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Helper to check if expression is an await
static bool is_await_expr(Expr* expr) {
    // TODO: Check for EXPR_AWAIT type when added to AST
    return false;  // Placeholder
}

bool contains_await(Expr* expr) {
    if (!expr) return false;
    
    if (is_await_expr(expr)) return true;
    
    // Recursively check sub-expressions
    switch (expr->type) {
        case EXPR_BINARY:
            return contains_await(expr->as.binary.left) || 
                   contains_await(expr->as.binary.right);
        
        case EXPR_UNARY:
            return contains_await(expr->as.unary.operand);
        
        case EXPR_CALL:
            for (int i = 0; i < expr->as.call.arg_count; i++) {
                if (contains_await(expr->as.call.args[i])) return true;
            }
            return false;
        
        default:
            return false;
    }
}

bool stmt_contains_await(Stmt* stmt) {
    if (!stmt) return false;
    
    switch (stmt->type) {
        case STMT_EXPR:
            return contains_await(stmt->as.expr.expression);
        
        case STMT_VAR_DECL:
            return contains_await(stmt->as.var_decl.initializer);
        
        case STMT_IF:
            return contains_await(stmt->as.if_stmt.condition) ||
                   stmt_contains_await(stmt->as.if_stmt.then_branch) ||
                   stmt_contains_await(stmt->as.if_stmt.else_branch);
        
        case STMT_WHILE:
            return contains_await(stmt->as.while_stmt.condition) ||
                   stmt_contains_await(stmt->as.while_stmt.body);
        
        case STMT_RETURN:
            return contains_await(stmt->as.return_stmt.value);
        
        case STMT_BLOCK:
            for (int i = 0; i < stmt->as.block.stmt_count; i++) {
                if (stmt_contains_await(stmt->as.block.stmts[i])) return true;
            }
            return false;
        
        default:
            return false;
    }
}

AsyncStateMachine* lower_async_function(FunctionStmt* func, SemanticAnalyzer* analyzer) {
    if (!func || !func->is_async) {
        return NULL;
    }
    
    CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_SEMANTIC, "Lowering async function: %s", func->name);
    
    AsyncStateMachine* sm = (AsyncStateMachine*)calloc(1, sizeof(AsyncStateMachine));
    if (!sm) return NULL;
    
    sm->function_name = strdup(func->name);
    sm->return_type = func->return_type;
    sm->state_count = 0;
    sm->states = NULL;
    sm->state_labels = NULL;
    sm->persistent_vars = NULL;
    sm->persistent_var_count = 0;
    
    // Analyze function body to identify await points
    // Each await point creates a new state
    
    // For now, create a simple single-state machine
    // TODO: Implement full state splitting at await points
    sm->state_count = 1;
    sm->states = (Stmt**)malloc(sizeof(Stmt*));
    sm->states[0] = func->body;
    sm->state_labels = (int*)malloc(sizeof(int));
    sm->state_labels[0] = 0;
    
    CPX_LOG(CPX_LOG_DEBUG, CPX_LOG_CAT_SEMANTIC, "Created state machine with %d states", sm->state_count);
    
    return sm;
}

FunctionStmt* generate_state_machine_step(AsyncStateMachine* sm) {
    if (!sm) return NULL;
    
    // Generate stepping function name: <original_name>_step
    char step_name[256];
    snprintf(step_name, sizeof(step_name), "%s_step", sm->function_name);
    
    CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_SEMANTIC, "Generating step function: %s", step_name);
    
    // Create new function statement
    FunctionStmt* step_func = (FunctionStmt*)calloc(1, sizeof(FunctionStmt));
    step_func->name = strdup(step_name);
    step_func->return_type = TYPE_VOID;
    step_func->is_async = false;
    
    // Parameters: (state_struct*, future*, prev_result)
    step_func->param_count = 3;
    
    // Function body: switch on state
    // TODO: Generate actual switch statement
    step_func->body = NULL;  // Placeholder
    
    return step_func;
}

FunctionStmt* generate_async_entry(AsyncStateMachine* sm) {
    if (!sm) return NULL;
    
    CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_SEMANTIC, "Generating async entry: %s_async", sm->function_name);
    
    // Create wrapper function that:
    // 1. Allocates state structure
    // 2. Creates future
    // 3. Calls step function
    // 4. Returns future
    
    FunctionStmt* entry_func = (FunctionStmt*)calloc(1, sizeof(FunctionStmt));
    
    char entry_name[256];
    snprintf(entry_name, sizeof(entry_name), "%s_async", sm->function_name);
    entry_func->name = strdup(entry_name);
    entry_func->return_type = TYPE_VOID;  // TODO: Should return Future*
    entry_func->is_async = false;
    
    // TODO: Generate actual implementation
    entry_func->body = NULL;
    
    return entry_func;
}

bool transform_await_expr(Expr* await_expr, AsyncStateMachine* sm, int current_state) {
    if (!await_expr || !sm) return false;
    
    CPX_LOG(CPX_LOG_DEBUG, CPX_LOG_CAT_SEMANTIC, "Transforming await in state %d", current_state);
    
    // Transform:
    //   let x = await foo()
    // Into:
    //   state++;
    //   future = foo();
    //   future_then(future, step_function, state_struct);
    //   return;
    
    // TODO: Implement actual transformation
    
    return true;
}

void free_async_state_machine(AsyncStateMachine* sm) {
    if (!sm) return;
    
    free(sm->function_name);
    free(sm->states);
    free(sm->state_labels);
    free(sm->persistent_vars);
    free(sm);
}
