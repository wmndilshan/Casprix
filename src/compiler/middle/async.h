/*
 * Casprix Compiler - Async/Await Support
 * Implements asynchronous function syntax and await expressions
 */

#ifndef ASYNC_H
#define ASYNC_H

#include "compiler/frontend/ast.h"
#include <stdbool.h>

// Async function metadata
typedef struct {
    FunctionStmt* function;
    bool is_async;
    Expr** await_points;     // All await expressions in function
    int await_count;
    char* state_machine_name; // Generated state machine function name
} AsyncMeta;

// Async context
typedef struct {
    AsyncMeta** async_functions;
    int async_count;
    int state_machines_generated;
} AsyncContext;

// Initialize async system
void init_async(AsyncContext* ctx);

// Mark function as async
void mark_async_function(FunctionStmt* func);

// Transform async function to state machine
FunctionStmt* transform_async_function(FunctionStmt* async_func, AsyncContext* ctx);

// Generate await point handling
Stmt* generate_await(Expr* awaited_expr, const char* result_var);

// Check if expression is await
bool is_await_expr(Expr* expr);

// Free async context
void free_async(AsyncContext* ctx);

#endif // ASYNC_H
