/*
 * Casprix — Canonical MIR preparation pipeline
 *
 * Shared middle-end entry used by CVM execution, C emission, MIR dumps,
 * safe-mode verification, and experimental native backends. Callers must not
 * invent divergent pass sequences for the same MIR consumers.
 *
 * Stage contract (in order):
 *   1. Lower AST → MIR                         (required)
 *   2. Async MIR transform                     (required when async present)
 *   3. Structural verification (pre-SSA)       (required)
 *   4. Optional MIR borrow check               (safe_mode; before mem2reg)
 *   5. mem2reg / SSA construction              (required for SSA consumers)
 *   6. Optional inlining                       (opt_level >= 2 && optimize)
 *   7. Optional MIR optimization               (optimize)
 *   8. Final MIR verification                  (required)
 *
 * Failure of any required stage aborts; backends must not consume invalid MIR.
 * Borrow check runs before mem2reg so addressable place identity is intact.
 * Address-taken locals that cannot be promoted remain as allocas.
 */

#ifndef CASPRIX_MIR_PIPELINE_H
#define CASPRIX_MIR_PIPELINE_H

#include "compiler/ir/mir.h"
#include "compiler/frontend/ast.h"
#include "compiler/sema/symtable.h"

#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MIR_PIPE_OK = 0,
    MIR_PIPE_ERR_LOWER,
    MIR_PIPE_ERR_VERIFY,
    MIR_PIPE_ERR_BORROW,
    MIR_PIPE_ERR_ALLOC,
    MIR_PIPE_ERR_INTERNAL
} MirPipelineStatus;

typedef struct {
    bool optimize;          /* run mir_optimize when true */
    int  opt_level;         /* 0–3 */
    bool run_inline;        /* allow inlining when optimize && opt_level >= 2 */
    bool safe_mode;         /* run MIR borrow checker */
    bool run_mem2reg;       /* promote allocas (default true) */
    bool dump_mir;          /* print MIR snapshots to dump_out */
    FILE* dump_out;         /* defaults to stdout when dump_mir */
    FILE* diag_out;         /* defaults to stderr */
    const char* module_name;
} MirPipelineOptions;

typedef struct {
    MirPipelineStatus status;
    MirModule* module;          /* owned by caller on success; NULL on failure */
    int verify_errors;
    int borrow_errors;
    int async_transformed;
    int allocas_promoted;
    int calls_inlined;
} MirPipelineResult;

/* Defaults: mem2reg on, optimize/safe_mode off, module name "main". */
void mir_pipeline_options_init(MirPipelineOptions* opts);

/*
 * Lower + prepare MIR for backend or execution consumers.
 * On success, result->module is owned by the caller (mir_module_destroy).
 * On failure, result->module is NULL and no MIR is leaked.
 */
MirPipelineResult mir_pipeline_prepare(Stmt** statements, int stmt_count,
                                       SymbolTable* symbols,
                                       const MirPipelineOptions* opts);

/* Destroy a successful result's module (no-op if NULL). */
void mir_pipeline_result_destroy(MirPipelineResult* result);

#ifdef __cplusplus
}
#endif

#endif /* CASPRIX_MIR_PIPELINE_H */
