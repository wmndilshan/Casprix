/**
 * Casprix Compiler — Linear Type System for `StringView`
 *
 * A `StringView` is a non-owning, linear borrow into an owning `String`.
 * Layout (enforced by the runtime ABI shim, not by this header):
 *
 *     struct StringView {
 *         data: const char*       // borrowed pointer into parent String storage
 *         len:  size_t            // byte length of the view
 *     }
 *
 * Linear semantics (compile-time only — zero runtime overhead):
 *
 *   1. A `StringView` has exactly ONE owning binding at a time.
 *      Passing it to a callee, returning it, or assigning it to another
 *      binding consumes the source.  A second use after consumption is a
 *      compile-time error (use-after-move).
 *
 *   2. A `StringView` is dependent on its *parent* `String`.  At the AST→MIR
 *      lowering boundary (after the semantic pass completes and before MIR
 *      construction begins), the drop planner walks every scope being
 *      destroyed and verifies that no linear view referring to a String in
 *      that scope is alive in any outer scope.  Any surviving view at the
 *      moment its parent is freed is a compile-time error — this prevents
 *      dangling-pointer use-after-free without a single byte of runtime
 *      overhead.
 *
 *   3. A `StringView` cannot escape farther than its parent.  Escape
 *      analysis runs a fixpoint pass that intersects the view's escape set
 *      with the parent's, and rejects any escape (return / closure capture
 *      / global store / heap-field store) that the parent does not also
 *      satisfy.
 *
 * This module is the small glue layer used by:
 *   - sema/ownership_check.[ch]    -- linear-consume tracking
 *   - sema/escape_analysis.[ch]    -- parent ↔ view escape propagation
 *   - sema/drop_planner.[ch]       -- surviving-view-at-parent-drop check
 *
 * The hooks themselves are invoked from sema/semantic.c at variable
 * declaration and at block-scope exit, which is the natural pre-MIR gate.
 */

#ifndef CASPERIX_LINEAR_VIEW_H
#define CASPERIX_LINEAR_VIEW_H

#include <stdbool.h>
#include "compiler/frontend/ast.h"
#include "compiler/sema/semantic.h"
#include "compiler/sema/escape_analysis.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The single, well-known type name recognised as a linear view.            */
#define LINEAR_VIEW_TYPE_NAME "StringView"

/* True when a declaration is typed as `StringView`, regardless of whether
 * the user spelled it as a struct, class, or named type.  We deliberately
 * key off the type name rather than introducing a TYPE_STRINGVIEW enum so
 * the parser, semantic analyzer, and existing sema passes stay untouched. */
bool linear_view_decl_is_view(const DeclarationStmt* decl);

/* True when an expression's resolved type-name is `StringView`.            */
bool linear_view_expr_is_view(const Expr* expr);

/* True when a declared variable's type-name is plain `String`.  The drop
 * planner uses this to know which entries are candidate parents during the
 * surviving-view check. */
bool linear_view_decl_is_string(const DeclarationStmt* decl);

/* Walk the initialiser of a `StringView` declaration to find the parent
 * `String` it borrows from.  We use the conservative rule: the first
 * variable reference (depth-first, left-to-right) whose resolved Symbol
 * has TYPE_STRING is the parent.  Returns the borrowed name pointer
 * (interned in the AST — caller must NOT free) or NULL when no parent can
 * be inferred (in which case the view is treated as "unknown parent" and
 * the planner cannot prove its safety; an error is emitted at the
 * declaration site). */
const char* linear_view_infer_parent(SemanticAnalyzer* analyzer,
                                     Expr* init_expr);

/* ─────────────────────────────────────────────────────────────────────────
 * Linear-view log
 *
 * The ownership checker stores per-view state on `Symbol.ownership_data`,
 * which is freed as soon as the declaring scope exits — well before the
 * escape analyser runs its fixpoint.  To bridge that gap we keep a tiny
 * per-function log of every `(view, parent, line)` triple seen during the
 * `analyze_stmt` walk.  The log survives scope pops and is cleared at each
 * new function entry.
 * ───────────────────────────────────────────────────────────────────────── */
typedef struct LinearViewLogEntry {
    char*  view_name;       /* owned copy of the view's binding name */
    char*  parent_name;     /* owned copy of the parent String name, or NULL */
    int    line;
} LinearViewLogEntry;

typedef struct LinearViewLog {
    LinearViewLogEntry* entries;
    int                 count;
    int                 capacity;
} LinearViewLog;

/* Drop and zero the log, freeing owned strings.                            */
void linear_view_log_reset(LinearViewLog* log);

/* Append a new view record.  `view_name` and `parent_name` are copied — the
 * caller retains ownership of its own strings.  NULL parent means "unknown
 * parent — treat as unsafe at drop time". */
void linear_view_log_add(LinearViewLog* log,
                         const char* view_name,
                         const char* parent_name,
                         int line);

/* Promote escape-analyzer entries for linear views from the log.
 *
 * Call this AFTER `escape_analyze_function` (which rebuilds the escape
 * table from scratch) and BEFORE `escape_propagate_view_links`.  For every
 * log entry, the matching escape entry is flipped to `is_string_view` and
 * linked to its parent by name. */
void linear_view_promote_from_log(EscapeAnalyzer* ea,
                                  const LinearViewLog* log);

#ifdef __cplusplus
}
#endif

#endif /* CASPERIX_LINEAR_VIEW_H */
