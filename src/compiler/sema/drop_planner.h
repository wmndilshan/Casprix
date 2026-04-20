/**
 * Casperix Compiler - Scope-Based Drop Planner
 *
 * Implements deterministic destructors (RAII) by tracking which variables
 * need cleanup at each scope exit point.  The codegen uses this information
 * to emit arc_release / destructor calls at the correct locations.
 *
 * Drop rules:
 *   - When a scope exits, all owned variables declared in that scope are dropped
 *   - Moved variables are NOT dropped (ownership transferred)
 *   - Borrowed variables are NOT dropped (they don't own the value)
 *   - Primitives are NOT dropped (stack-allocated, no destructor)
 *   - Region-allocated values are NOT individually dropped (region handles bulk free)
 *
 * Integration:
 *   - The semantic analyzer calls drop_planner_register() for each var decl
 *   - At scope exit, drop_planner_get_drops() returns the list of drops needed
 *   - The codegen backend emits the corresponding cleanup code
 */

#ifndef CASPERIX_DROP_PLANNER_H
#define CASPERIX_DROP_PLANNER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Drop classification ─── */

typedef enum {
    DROP_NONE   = 0,   /* No cleanup needed (primitive, moved, borrowed)  */
    DROP_ARC,          /* Call arc_release(ptr)                            */
    DROP_RC,           /* Call rc_release(ptr)                             */
    DROP_DTOR,         /* Call custom destructor then free                 */
    DROP_REGION,       /* Managed by enclosing region — skip              */
    DROP_SCOPE_GUARD,  /* Call scope_guard_drop()                         */
    DROP_LINEAR_VIEW   /* Linear StringView — no codegen drop, but the    */
                       /* planner enforces it does not outlive its parent.*/
} DropKind;

/* Per-variable drop entry */
typedef struct {
    const char* var_name;       /* Variable identifier                    */
    int         scope_level;    /* Scope depth where declared             */
    int         stack_offset;   /* Stack frame offset (for codegen)       */
    DropKind    kind;           /* What kind of cleanup                   */
    const char* dtor_name;      /* Custom destructor symbol (if DROP_DTOR)*/
    bool        is_moved;       /* True if ownership was transferred out  */
    bool        is_borrowed;    /* True if this is a borrow, not owner    */
    bool        is_param;       /* True if function parameter             */

    /* ── Linear Type System (StringView) ───────────────────────────────────
     * `parent_name`     — for DROP_LINEAR_VIEW entries: the borrowed name
     *                     of the owning String the view depends on.       */
    const char* parent_name;
    int         decl_line;      /* Line of the original declaration       */
} DropEntry;

/* ─── Drop planner context ─── */

#define MAX_DROP_ENTRIES    256
#define MAX_DROP_SCOPES      32

typedef struct {
    DropEntry  entries[MAX_DROP_ENTRIES];
    int        count;
    int        current_scope;

    /* Scope bookmarks: entries[scope_start[level]..count) belong to scope */
    int        scope_start[MAX_DROP_SCOPES];
} DropPlanner;

/* ─── API ─── */

/* Lifecycle */
void drop_planner_init(DropPlanner* dp);
void drop_planner_reset(DropPlanner* dp);

/* Scope management */
void drop_planner_enter_scope(DropPlanner* dp);
void drop_planner_exit_scope(DropPlanner* dp);

/* Registration — called when a variable is declared */
void drop_planner_register(DropPlanner* dp, const char* var_name,
                            int stack_offset, DropKind kind,
                            const char* dtor_name,
                            bool is_param);

/* Register a linear `StringView` view-binding with its parent String.
 * The entry is dropped via DROP_LINEAR_VIEW (a no-op at codegen) but is
 * tracked by `drop_planner_check_string_drop_invariants` so any surviving
 * view at the moment its parent is dropped is reported as a hard error. */
void drop_planner_register_linear_view(DropPlanner* dp,
                                       const char* view_name,
                                       const char* parent_string_name,
                                       int decl_line);

/* Mark a variable as moved (skip its drop) */
void drop_planner_mark_moved(DropPlanner* dp, const char* var_name);

/* Mark a variable as a borrow (skip its drop) */
void drop_planner_mark_borrowed(DropPlanner* dp, const char* var_name);

/* Query: get the list of drops needed for the current scope exit.
 * Returns pointer to first drop entry and sets *count.
 * Entries are in REVERSE declaration order (LIFO). */
const DropEntry* drop_planner_get_scope_drops(DropPlanner* dp, int* count);

/* Query: get all pending drops from current scope down to target_scope.
 * Useful for early return statements which must drop all enclosing scopes. */
const DropEntry* drop_planner_get_drops_to_scope(DropPlanner* dp,
                                                   int target_scope,
                                                   int* count);

/* ─── Linear-view safety check (AST→MIR boundary) ─────────────────────────
 *
 * Walks the entries belonging to the *currently exiting* scope and, for
 * every owning String about to be dropped, scans the entire planner for
 * any DROP_LINEAR_VIEW entry whose `parent_name` matches and whose
 * `scope_level` is strictly less than the dropping scope (i.e. the view
 * outlives its parent).  Each such surviving view is reported as a
 * compile-time error at the parent's drop site (`drop_line`).
 *
 * Returns the number of diagnostics emitted (0 == proven safe).
 *
 * This MUST be called before `drop_planner_exit_scope` discards the entries
 * of the exiting scope.                                                    */
int drop_planner_check_string_drop_invariants(DropPlanner* dp,
                                              int drop_line);

/* Debug */
void drop_planner_print(DropPlanner* dp);

#ifdef __cplusplus
}
#endif

#endif /* CASPERIX_DROP_PLANNER_H */
