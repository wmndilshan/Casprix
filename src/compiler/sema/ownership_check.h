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

/* ─── Path-sensitive move state ──────────────────────────────────────────────
 *
 * The move bit for each variable is tracked in a per-path map keyed by Symbol*
 * (stable for a variable's lifetime within a function body), NOT in the
 * per-symbol OwnershipInfo. This lets if/else and loop analysis snapshot the
 * state, analyse each path from a common start, and merge at join points so a
 * move on one path does not leak onto the others.
 *
 * A Symbol present in the map with `moved == true` is "definitely moved on the
 * current path". Absent ⇒ not moved.
 */
typedef struct {
    void* sym;         /* Symbol* — opaque here to avoid a symtable.h cycle */
    bool  moved;
    int   move_line;
} OwnMoveEntry;

typedef struct {
    OwnMoveEntry* entries;
    int count;
    int capacity;
} OwnMoveState;

/* A detached copy of an OwnMoveState, produced by own_state_snapshot(). */
typedef OwnMoveState OwnStateSnapshot;

// Ownership checker context
typedef struct OwnershipChecker {
    SemanticAnalyzer* analyzer;
    int current_scope;
    bool in_unsafe_block;

    /* Path-sensitive move state for the function body currently being
     * analysed. Reset at each function entry. */
    OwnMoveState move_state;

    /* When true, diagnostics from check_ownership_valid / mark_moved are
     * suppressed. Used for the first (priming) pass over a loop body so that
     * a genuine in-body error is only reported once, on the final pass. */
    bool suppress_diagnostics;
} OwnershipChecker;

/* ─── Path-sensitive move-state operations ───────────────────────────────── */

/* Copy the checker's current move state into `out` (caller owns `out` and must
 * pass it to own_state_free). */
void own_state_snapshot(OwnershipChecker* checker, OwnStateSnapshot* out);

/* Replace the checker's current move state with a copy of `snap`. */
void own_state_restore(OwnershipChecker* checker, const OwnStateSnapshot* snap);

/* dst := intersection(dst, src) — a variable stays MOVED only if it is moved
 * in BOTH. Used to merge sibling branches at a join point. */
void own_state_merge_intersect(OwnStateSnapshot* dst, const OwnStateSnapshot* src);

/* dst := union(dst, src) — a variable is MOVED if moved in EITHER. Used to fold
 * a loop back-edge into the loop-head state. */
void own_state_merge_union(OwnStateSnapshot* dst, const OwnStateSnapshot* src);

/* out := deep copy of src (out must be uninitialised or already freed). */
void own_state_copy(OwnStateSnapshot* out, const OwnStateSnapshot* src);

/* Structural equality of two move states (order-independent). */
bool own_state_equal(const OwnStateSnapshot* a, const OwnStateSnapshot* b);

/* Release a snapshot's storage. */
void own_state_free(OwnStateSnapshot* snap);

/* Drop map entries for symbols declared at `scope_level` — call when that
 * scope exits and its Symbols are about to be freed. */
void own_state_forget_scope(OwnershipChecker* checker, int scope_level);

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

// Reset path-sensitive move state (call at function-body entry)
void ownership_reset_function(OwnershipChecker* checker);

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
