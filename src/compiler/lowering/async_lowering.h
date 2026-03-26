/**
 * Casperix Compiler - Async/Await Lowering
 * Transforms async functions into state machine code
 */

#ifndef ASYNC_LOWERING_H
#define ASYNC_LOWERING_H

#include "compiler/frontend/ast.h"
#include "compiler/sema/semantic.h"
#include <stdbool.h>

// State machine representation for async function
typedef struct {
    char* function_name;
    int state_count;
    Stmt** states;           // Array of statements per state
    int* state_labels;       // Label IDs for each state
    DataType return_type;
    
    // Local variables that need to persist across states
    Symbol** persistent_vars;
    int persistent_var_count;
} AsyncStateMachine;

// Transform an async function into a state machine
AsyncStateMachine* lower_async_function(FunctionStmt* func, SemanticAnalyzer* analyzer);

// Generate state machine stepping function
FunctionStmt* generate_state_machine_step(AsyncStateMachine* sm);

// Generate async entry point wrapper
FunctionStmt* generate_async_entry(AsyncStateMachine* sm);

// Transform await expression into state transition
bool transform_await_expr(Expr* await_expr, AsyncStateMachine* sm, int current_state);

// Free state machine
void free_async_state_machine(AsyncStateMachine* sm);

// Check if expression contains await
bool contains_await(Expr* expr);

// Check if statement contains await
bool stmt_contains_await(Stmt* stmt);

#endif // ASYNC_LOWERING_H
