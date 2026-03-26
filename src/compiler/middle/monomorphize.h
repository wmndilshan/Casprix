/*
 * Casprix Compiler - Generic Type Monomorphization
 * Specializes generic classes and functions for concrete types
 */

#ifndef MONOMORPHIZE_H
#define MONOMORPHIZE_H

#include "compiler/frontend/ast.h"
#include "compiler/sema/symtable.h"
#include <stdbool.h>

// Generic instantiation record
typedef struct GenericInstance {
    char* mangled_name;          // e.g., "List_Int"
    char* base_name;             // e.g., "List"
    DataType* type_args;         // Concrete type arguments
    int type_arg_count;
    ClassStmt* specialized_class; // Specialized AST
    struct GenericInstance* next;
} GenericInstance;

// Monomorphization context
typedef struct {
    GenericInstance* instances;
    int instance_count;
    SymbolTable* type_environment;  // Maps type parameters to concrete types
} MonoContext;

// Initialize monomorphization
void init_monomorphization(MonoContext* ctx);

// Monomorphize a generic class
GenericInstance* monomorphize_class(ClassStmt* generic_class,
                                    DataType* type_args,
                                    int arg_count,
                                    MonoContext* ctx);

// Monomorphize a generic function
FunctionStmt* monomorphize_function(FunctionStmt* generic_func,
                                   DataType* type_args,
                                    int arg_count,
                                    MonoContext* ctx);

// Find existing instantiation
GenericInstance* find_instantiation(const char* base_name,
                                    DataType* type_args,
                                    int arg_count,
                                    MonoContext* ctx);

// Type substitution
Expr* substitute_type_in_expr(Expr* expr, TypeParam* params, int param_count,
                              DataType* args, int arg_count);
Stmt* substitute_type_in_stmt(Stmt* stmt, TypeParam* params, int param_count,
                              DataType* args, int arg_count);

// Mangle generic name
char* mangle_generic_name(const char* base, DataType* type_args, int count);

// Free monomorphization context
void free_mono_context(MonoContext* ctx);

#endif // MONOMORPHIZE_H
