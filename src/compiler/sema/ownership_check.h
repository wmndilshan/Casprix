/**
 * Casperix Compiler - Ownership & Borrowing Checker
 * 
 * Implements compile-time ownership tracking to prevent:
 * - Use-after-move errors
 * - Double-free errors
 * - Data races through exclusive mutable access
 */

#ifndef CASPERIX_OWNERSHIP_CHECK_H
#define CASPERIX_OWNERSHIP_CHECK_H

#include <stdbool.h>
#include "compiler/sema/semantic.h"
/* OwnershipMode enum is defined in compiler/frontend/ast.h which is included
 * transitively via semantic.h -> symtable.h -> ast.h */

// Lifetime information
typedef struct {
    int scope_level;        // Scope depth where value lives
    const char* name;       // Variable name
} Lifetime;

// Enhanced symbol with ownership tracking
typedef struct OwnershipInfo {
    OwnershipMode state;       /* Uses OwnershipMode from ast.h */
    Lifetime lifetime;
    int borrow_count;       // Number of active immutable borrows
    bool has_mut_borrow;    // Has active mutable borrow
    int move_location;      // Line where value was moved
} OwnershipInfo;

// Ownership checker context
typedef struct OwnershipChecker {
    SemanticAnalyzer* analyzer;
    int current_scope;
    bool in_unsafe_block;
} OwnershipChecker;

// Initialize ownership checker
void ownership_checker_init(OwnershipChecker* checker, SemanticAnalyzer* analyzer);

// Check if a variable access is valid
bool check_ownership_valid(OwnershipChecker* checker, const char* var_name, int line);

// Mark a variable as moved
void mark_moved(OwnershipChecker* checker, const char* var_name, int line);

// Add immutable borrow
bool add_borrow(OwnershipChecker* checker, const char* var_name, int line);

// Add mutable borrow
bool add_mut_borrow(OwnershipChecker* checker, const char* var_name, int line);

// Release borrow
void release_borrow(OwnershipChecker* checker, const char* var_name);

// Check if expression involves a move
bool is_move_expr(Expr* expr);

// Validate ownership at end of scope
void validate_scope_end(OwnershipChecker* checker, int scope_level);

#endif // CASPERIX_OWNERSHIP_CHECK_H
