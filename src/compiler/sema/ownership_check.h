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

    /* ── Linear Type System (StringView) ──────────────────────────────────
     * `is_linear_view`  — the symbol is bound to a `StringView` value and
     *                     therefore obeys linear-consume semantics.
     * `parent_string`   — borrowed pointer to the owning String variable's
     *                     name (lives in the AST; do not free).  NULL when
     *                     the parent could not be inferred — such a view is
     *                     pessimistically rejected by the drop planner.
     * `consume_count`   — number of consuming uses observed; >1 implies
     *                     use-after-move and is reported.                  */
    bool        is_linear_view;
    const char* parent_string;
    int         consume_count;
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

/* ─── Linear Type System (StringView) hooks ───────────────────────────────
 *
 * Register a freshly-declared `StringView` binding.  `parent_string` may be
 * NULL when the analyzer could not statically determine which owning
 * `String` the view borrows from; in that case the drop planner will refuse
 * to prove safety and emit a diagnostic at the parent-drop site.
 *
 * `register_linear_view` does NOT replace the regular ownership state — it
 * augments it: the binding is also OWNED, and may be borrowed/moved like
 * any other value, but is additionally subject to linear-consume tracking
 * and to the parent-survival invariant enforced by drop_planner.
 */
void register_linear_view(OwnershipChecker* checker,
                          const char* view_name,
                          const char* parent_string_name,
                          int line);

/* Record a consuming use (call argument, return, assignment RHS).  When the
 * symbol is a linear view, the second consume is reported as
 * "use of consumed StringView".  No-op for non-view symbols. */
void linear_view_consume(OwnershipChecker* checker,
                         const char* var_name,
                         int line);

#endif // CASPERIX_OWNERSHIP_CHECK_H
