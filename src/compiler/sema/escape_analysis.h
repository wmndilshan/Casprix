/**
 * Casperix Compiler - Escape Analysis
 *
 * Determines whether a value escapes its defining scope.
 * Values that do NOT escape can be stack-allocated (zero heap overhead).
 *
 * Escape conditions:
 *   1. Returned from function
 *   2. Stored in a global or closure-captured variable
 *   3. Passed to a function that stores it (non-borrow parameter)
 *   4. Assigned to a field of an object that outlives the scope
 *   5. Stored in a collection that escapes
 *
 * Non-escape (stack-eligible):
 *   - Local variable only used within function body
 *   - Passed by borrow to called functions
 *   - Temporary values consumed within expression
 */

#ifndef CASPERIX_ESCAPE_ANALYSIS_H
#define CASPERIX_ESCAPE_ANALYSIS_H

#include <stdbool.h>
#include <stdint.h>
#include "compiler/frontend/ast.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Escape classification ─── */

typedef enum {
    ESCAPE_NONE       = 0x00,  /* Does not escape — stack-allocatable       */
    ESCAPE_RETURN     = 0x01,  /* Returned from the enclosing function      */
    ESCAPE_CLOSURE    = 0x02,  /* Captured by a closure environment         */
    ESCAPE_GLOBAL     = 0x04,  /* Stored in a global / static variable      */
    ESCAPE_FIELD      = 0x08,  /* Written into a heap-object field          */
    ESCAPE_CALL_ARG   = 0x10,  /* Passed as non-borrow arg to unknown func  */
    ESCAPE_COLLECTION = 0x20,  /* Inserted into a collection that escapes   */
    ESCAPE_UNKNOWN    = 0x40   /* Conservative: cannot determine            */
} EscapeFlags;

/* Per-variable escape information */
typedef struct {
    const char* var_name;      /* Variable identifier                       */
    int         scope_level;   /* Scope depth of the defining block         */
    EscapeFlags flags;         /* Bitwise OR of all observed escape paths   */
    bool        is_parameter;  /* True if function parameter                */
    bool        analyzed;      /* True if analysis has run for this var     */

    /* ── Linear Type System (StringView) ───────────────────────────────────
     * `is_string_view`  — entry models a StringView binding.
     * `parent_name`     — borrowed name of the owning String (AST-lived).
     *
     * The propagation pass (`escape_propagate_view_links`) intersects each
     * view's escape-set with its parent's: a view that escapes via RETURN /
     * CLOSURE / GLOBAL / FIELD where the parent does NOT also escape that
     * way is rejected — the borrowed pointer would outlive the storage.   */
    bool        is_string_view;
    const char* parent_name;
} EscapeInfo;

/* ─── Analysis context ─── */

#define MAX_ESCAPE_ENTRIES 512

typedef struct EscapeAnalyzer {
    EscapeInfo   entries[MAX_ESCAPE_ENTRIES];
    int          count;
    int          current_scope;
    bool         in_closure;   /* True when inside a closure body           */
} EscapeAnalyzer;

/* ─── API ─── */

/* Lifecycle */
void escape_analyzer_init(EscapeAnalyzer* ea);
void escape_analyzer_reset(EscapeAnalyzer* ea);

/* Scope tracking */
void escape_enter_scope(EscapeAnalyzer* ea);
void escape_exit_scope(EscapeAnalyzer* ea);

/* Registration */
EscapeInfo* escape_register_var(EscapeAnalyzer* ea, const char* name,
                                 bool is_parameter);

/* Register a StringView and its parent String name (parent may be NULL).
 * The view participates in the regular escape walk like any other variable
 * AND is linked to its parent for the post-walk propagation pass.          */
EscapeInfo* escape_register_string_view(EscapeAnalyzer* ea,
                                         const char* name,
                                         const char* parent_name);

/* Propagate escape flags between linked parent ↔ view pairs and emit a
 * diagnostic when the view escapes farther than the parent allows.  Run at
 * the end of `escape_analyze_function` (or any time the analyser has a
 * complete view of a function body).  Returns the number of diagnostics
 * emitted (0 == no escape violations found).                                */
int escape_propagate_view_links(EscapeAnalyzer* ea, int line_for_diag);

/* Mark escape paths (called during AST walk) */
void escape_mark_return(EscapeAnalyzer* ea, const char* var_name);
void escape_mark_closure_capture(EscapeAnalyzer* ea, const char* var_name);
void escape_mark_global_store(EscapeAnalyzer* ea, const char* var_name);
void escape_mark_field_store(EscapeAnalyzer* ea, const char* var_name);
void escape_mark_call_arg(EscapeAnalyzer* ea, const char* var_name,
                           bool is_borrow);
void escape_mark_collection_insert(EscapeAnalyzer* ea, const char* var_name);

/* Query */
EscapeInfo* escape_lookup(EscapeAnalyzer* ea, const char* var_name);
bool        escape_can_stack_alloc(EscapeAnalyzer* ea, const char* var_name);
EscapeFlags escape_get_flags(EscapeAnalyzer* ea, const char* var_name);

/* Full function analysis — walk function body AST */
void escape_analyze_function(EscapeAnalyzer* ea, Stmt* func_stmt);

/* Walk helpers (recursive AST traversal) */
void escape_analyze_stmt(EscapeAnalyzer* ea, Stmt* stmt,
                          const char* enclosing_func);
void escape_analyze_expr(EscapeAnalyzer* ea, Expr* expr,
                          const char* enclosing_func,
                          EscapeFlags context_flags);

/* Debug */
void escape_print_report(EscapeAnalyzer* ea);

#ifdef __cplusplus
}
#endif

#endif /* CASPERIX_ESCAPE_ANALYSIS_H */
