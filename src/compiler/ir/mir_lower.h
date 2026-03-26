/*
 * Casprix Compiler — AST → MIR Lowering Pass
 *
 * Transforms the high-level typed AST into SSA-form MIR.
 *
 * Strategy:
 *   - Variables become alloca + load/store (mem2reg promotion happens later).
 *   - Control flow (if/while/for/match) → basic blocks + branch instructions.
 *   - Closures → struct init (environment) + function pointer.
 *   - Method calls → virtual dispatch or direct call based on class info.
 *   - Ownership annotations preserved as MIR_BORROW / MIR_MOVE / MIR_DROP.
 *
 * This pass produces "pre-SSA" MIR: variables are in alloca form.
 * A subsequent mem2reg pass will promote to true SSA with phi nodes.
 */

#ifndef MIR_LOWER_H
#define MIR_LOWER_H

#include "mir.h"
#include "compiler/frontend/ast.h"
#include "compiler/sema/symtable.h"

/* ────────────────────────────────────────────────
 * Lowering context — tracks state during AST walk
 * ──────────────────────────────────────────────── */
typedef struct {
    MirModule*      module;
    MirBuilder      builder;
    SymbolTable*    symtable;

    /* Variable name → alloca MirValueId mapping (per-function) */
    struct {
        const char*  name;
        MirValueId   alloca_id;
        MirType*     type;
    }*              var_map;
    int             var_map_count;
    int             var_map_capacity;

    /* Loop break/continue targets */
    struct {
        MirBlock*   break_bb;
        MirBlock*   continue_bb;
    }               loop_stack[32];
    int             loop_depth;

    /* Lambda/closure counter */
    int             lambda_counter;

    /* True while lowering synthetic top-level entry statements. */
    bool            lowering_toplevel;

    /* Error tracking */
    int             error_count;
} MirLowerCtx;

/* ────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────── */

/* Lower an entire program (array of top-level statements) into a MIR module. */
MirModule* mir_lower_program(Stmt** statements, int stmt_count,
                              SymbolTable* symtable, const char* module_name);

/* Lower a single function into the given module. */
MirFunction* mir_lower_function(MirLowerCtx* ctx, Stmt* func_stmt);

#endif /* MIR_LOWER_H */
