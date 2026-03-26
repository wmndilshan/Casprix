/*
 * Casprix Compiler - Trait System (Interfaces)
 * Enables compile-time polymorphism
 */

#ifndef TRAIT_H
#define TRAIT_H

#include "compiler/frontend/ast.h"
#include <stdbool.h>

// Trait method signature
typedef struct {
    char* name;
    DataType return_type;
    Parameter* parameters;
    int param_count;
} TraitMethod;

// Trait definition
typedef struct {
    char* name;
    TraitMethod* methods;
    int method_count;
    char** super_traits;  // Trait inheritance
    int super_count;
} Trait;

// Trait implementation
typedef struct {
    Trait* trait;
    ClassStmt* implementing_class;
    MethodDecl** method_impls;  // Actual implementations
    int impl_count;
} TraitImpl;

// Trait context
typedef struct {
    Trait** traits;
    int trait_count;
    TraitImpl** implementations;
    int impl_count;
} TraitContext;

// Initialize trait system
void init_traits(TraitContext* ctx);

// Define new trait
Trait* define_trait(const char* name, TraitMethod* methods, int method_count);

// Implement trait for class
bool implement_trait(TraitContext* ctx, Trait* trait, ClassStmt* class_stmt);

// Check if class implements trait
bool class_implements_trait(TraitContext* ctx, ClassStmt* class_stmt, const char* trait_name);

// Verify trait implementation (all methods present)
bool verify_trait_impl(Trait* trait, ClassStmt* class_stmt);

// Get trait method
TraitMethod* get_trait_method(Trait* trait, const char* method_name);

// Free traits
void free_traits(TraitContext* ctx);

#endif // TRAIT_H
