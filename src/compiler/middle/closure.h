/*
 * Casprix Compiler - Closure Analysis and Code Generation
 * Handles lambda expressions with captured variables
 */

#ifndef CLOSURE_H
#define CLOSURE_H

#include "compiler/frontend/ast.h"
#include "compiler/sema/symtable.h"
#include <stdbool.h>

// Capture mode
typedef enum {
    CAPTURE_BY_VALUE,     // Copy the value
    CAPTURE_BY_REFERENCE, // Reference to original
    CAPTURE_BY_MOVE       // Move ownership into closure
} CaptureMode;

// Captured variable information
typedef struct {
    char* var_name;
    DataType type;
    char* class_name;      // For TYPE_CLASS
    CaptureMode mode;
    int parent_offset;     // Offset in parent's stack frame
    int env_offset;        // Offset in closure environment
    bool is_mutable;
} CapturedVar;

// Closure environment
typedef struct {
    CapturedVar* variables;
    int count;
    int total_size;        // Total size in bytes
    int alignment;         // Required alignment
} ClosureEnv;

// Closure metadata (attached to LambdaExpr)
typedef struct {
    ClosureEnv* environment;
    char* mangled_name;    // Unique name for the lambda function
    int lambda_id;         // Unique ID
    bool needs_heap_alloc; // True if captures exist
} ClosureMeta;

// Initialize closure analyzer
void init_closure_analyzer(void);

// Analyze lambda expression to determine captures
ClosureMeta* analyze_closure(LambdaExpr* lambda, SymbolTable* symtable, SymbolTable* parent_table);

// Calculate closure environment layout
void layout_closure_env(ClosureEnv* env);

// Check if variable is captured
bool is_variable_captured(const char* var_name, ClosureEnv* env);

// Get captured variable info
CapturedVar* get_captured_var(const char* var_name, ClosureEnv* env);

// Generate mangled name for lambda
char* mangle_lambda_name(const char* context, int lambda_id);

// Free closure metadata
void free_closure_meta(ClosureMeta* meta);
void free_closure_env(ClosureEnv* env);

// Code generation helpers
void emit_closure_allocation(ClosureMeta* meta, FILE* out);
void emit_closure_setup(ClosureMeta* meta, FILE* out);
void emit_closure_call(Expr* closure_expr, Expr** args, int arg_count, FILE* out);

#endif // CLOSURE_H
