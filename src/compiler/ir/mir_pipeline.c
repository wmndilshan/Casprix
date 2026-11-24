#include "compiler/ir/mir_pipeline.h"
#include "compiler/ir/mir_lower.h"
#include "compiler/ir/mir_opt.h"
#include "compiler/ir/mir_borrow.h"
#include "compiler/ir/mir_mem2reg.h"
#include "compiler/ir/mir_inline.h"

#include <stdio.h>
#include <string.h>

void mir_pipeline_options_init(MirPipelineOptions* opts) {
    if (!opts) return;
    memset(opts, 0, sizeof(*opts));
    opts->run_mem2reg = true;
    opts->run_inline = true;
    opts->module_name = "main";
    opts->dump_out = stdout;
    opts->diag_out = stderr;
}

void mir_pipeline_result_destroy(MirPipelineResult* result) {
    if (!result) return;
    if (result->module) {
        mir_module_destroy(result->module);
        result->module = NULL;
    }
}

MirPipelineResult mir_pipeline_prepare(Stmt** statements, int stmt_count,
                                       SymbolTable* symbols,
                                       const MirPipelineOptions* opts) {
    MirPipelineResult result;
    MirPipelineOptions local;
    FILE* dump;
    FILE* diag;

    memset(&result, 0, sizeof(result));
    result.status = MIR_PIPE_ERR_INTERNAL;

    if (!opts) {
        mir_pipeline_options_init(&local);
        opts = &local;
    }
    dump = opts->dump_out ? opts->dump_out : stdout;
    diag = opts->diag_out ? opts->diag_out : stderr;

    result.module = mir_lower_program(statements, stmt_count, symbols,
                                      opts->module_name ? opts->module_name : "main");
    if (!result.module) {
        result.status = MIR_PIPE_ERR_LOWER;
        return result;
    }

    for (MirFunction* f = result.module->func_list; f; f = f->next_func) {
        if (f->is_async)
            result.async_transformed += mir_transform_async(f);
    }

    /* Pre-SSA structural/type checks catch lowering bugs early. */
    result.verify_errors = mir_verify_module(result.module, diag);
    if (result.verify_errors != 0) {
        mir_module_destroy(result.module);
        result.module = NULL;
        result.status = MIR_PIPE_ERR_VERIFY;
        return result;
    }

    /* Borrow check before mem2reg so addressable place identity remains. */
    if (opts->safe_mode) {
        MirBorrowChecker bc;
        mir_borrow_init(&bc, result.module);
        result.borrow_errors = mir_borrow_check_module(&bc, result.module);
        if (result.borrow_errors > 0) {
            mir_borrow_print_errors(&bc, dump);
            mir_borrow_destroy(&bc);
            mir_module_destroy(result.module);
            result.module = NULL;
            result.status = MIR_PIPE_ERR_BORROW;
            return result;
        }
        mir_borrow_destroy(&bc);
    }

    if (opts->run_mem2reg) {
        MirMem2RegStats m2r = {0};
        mir_mem2reg_module(result.module, &m2r);
        result.allocas_promoted = m2r.allocas_promoted;
    }

    if (opts->optimize && opts->run_inline && opts->opt_level >= 2) {
        MirInlineStats inl = {0};
        mir_inline_module(result.module, opts->opt_level, &inl);
        result.calls_inlined = inl.calls_inlined;
        if (opts->dump_mir) {
            fprintf(dump, "\n  === MIR (after inlining) ===\n");
            mir_print_module(result.module, dump);
        }
    }

    if (opts->optimize) {
        MirOptStats opt = {0};
        mir_optimize_module(result.module, (MirOptLevel)opts->opt_level, &opt);
    }

    result.verify_errors = mir_verify_module(result.module, diag);
    if (result.verify_errors != 0) {
        mir_module_destroy(result.module);
        result.module = NULL;
        result.status = MIR_PIPE_ERR_VERIFY;
        return result;
    }

    if (opts->dump_mir) {
        fprintf(dump, "\n  === MIR (prepared) ===\n");
        mir_print_module(result.module, dump);
    }

    result.status = MIR_PIPE_OK;
    return result;
}
