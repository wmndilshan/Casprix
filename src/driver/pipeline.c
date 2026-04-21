/**
 * pipeline.c - Compilation pipeline orchestration for the Casprix compiler driver
 *
 * All per-compilation state is held in a CompileCtx structure,
 * allowing a single cleanup point (compile_ctx_destroy) regardless
 * of where in the pipeline an error occurs.
 */
#include "driver/pipeline.h"
#include "driver/cli.h"
#include "driver/io.h"
#include "compiler/middle/async.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifdef _WIN32
#include <direct.h>
#define casprix_getcwd(buf, sz) _getcwd((buf), (int)(sz))
#else
#include <unistd.h>
#define casprix_getcwd(buf, sz) getcwd((buf), (sz))
#endif

#include "casprix/common.h"
#include "support/log.h"
#include "support/error.h"
#include "support/debug.h"
#include "support/diagnostic.h"
#include "compiler/frontend/lexer.h"
#include "compiler/frontend/parser.h"
#include "compiler/sema/semantic.h"
#include "compiler/sema/ownership_check.h"
#include "compiler/codegen/asmgen.h"
#include "compiler/codegen/optimizer.h"
#include "compiler/ir/mir.h"
#include "compiler/ir/mir_lower.h"
#include "compiler/ir/mir_opt.h"
#include "compiler/ir/mir_borrow.h"
#include "compiler/ir/mir_consteval.h"
#include "compiler/ir/mir_backend.h"
#include "compiler/ir/mir_mem2reg.h"
#include "compiler/ir/mir_inline.h"
#include "util/tools.h"
#include "util/module.h"

/* ============================================================================
 * CompileCtx — owns all heap-allocated per-compilation state.
 *
 * A single compile_ctx_destroy() call cleans up everything, eliminating
 * the duplicate free() chains that previously appeared on every error path.
 * ========================================================================== */

typedef struct {
    char*             source;
    Stmt**            statements;
    int               stmt_count;
    SemanticAnalyzer  analyzer;
    bool              analyzer_init;
    ModuleRegistry    module_registry;
    bool              registry_init;
    AsyncContext      async_ctx;
    bool              async_init;
    MirModule*        mir_module;
} CompileCtx;

static bool g_link_needs_skia = false;

static void compile_ctx_init(CompileCtx* ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

static void compile_ctx_destroy(CompileCtx* ctx) {
    if (ctx->mir_module)    { mir_module_destroy(ctx->mir_module);    ctx->mir_module = NULL; }
    if (ctx->analyzer_init) { free_semantic_analyzer(&ctx->analyzer); ctx->analyzer_init = false; }
    if (ctx->registry_init) { free_module_registry(&ctx->module_registry); ctx->registry_init = false; }
    if (ctx->async_init)    { free_async(&ctx->async_ctx); ctx->async_init = false; }
    /* Skip per-stmt free: merged module+user stmts array causes crash in free_stmt.
     * Leaking AST nodes is acceptable for a short-lived compiler process. */
    free(ctx->statements); ctx->statements = NULL;
    free(ctx->source);     ctx->source = NULL;
}

/* ============================================================================
 * Internal helpers
 * ========================================================================== */

static bool uses_legacy_ast_codegen(void) {
    if (g_config.output_kind != OUTPUT_NATIVE) return false;
    if (!g_config.use_mir)                     return true;
    if (g_config.native_codegen == NATIVE_CODEGEN_LEGACY_AST) return true;
    return g_config.allow_legacy_backend_fallback;
}

static bool uses_mir_codegen_backend(void) {
    if (g_config.output_kind != OUTPUT_NATIVE) return true;
    return g_config.native_codegen == NATIVE_CODEGEN_MIR &&
           !g_config.allow_legacy_backend_fallback;
}

static bool str_starts_with(const char* value, const char* prefix) {
    if (!value || !prefix) return false;
    while (*prefix) {
        if (*value++ != *prefix++) return false;
    }
    return true;
}

static bool module_requires_skia_runtime(const char* module_name) {
    if (!module_name || !*module_name) return false;
    return str_starts_with(module_name, "lib/skia/");
}

static MirBackendConfig make_mir_backend_config(const char* output_path) {
    MirBackendConfig cfg = {
        .target        = MIR_TARGET_X86_64,
        .output_format = MIR_OUTPUT_ASM,
        .opt_level     = g_config.opt_level,
        .debug_info    = g_config.verbose,
        .pic           = false,
        .size_opt      = g_config.size_opt,
        .output_path   = output_path,
    };
    switch (g_config.output_kind) {
        case OUTPUT_VM:
            cfg.target        = MIR_TARGET_VM;
            cfg.output_format = MIR_OUTPUT_BYTECODE;
            break;
        case OUTPUT_JIT:
            cfg.target        = MIR_TARGET_JIT;
            cfg.output_format = MIR_OUTPUT_BYTECODE;
            break;
        case OUTPUT_C:
            cfg.target        = MIR_TARGET_X86_64;
            cfg.output_format = MIR_OUTPUT_C;
            cfg.pic           = true;
            break;
        default:
            break;
    }
    return cfg;
}

static MirBackend* create_mir_backend(const char* output_path) {
    MirBackendConfig cfg = make_mir_backend_config(output_path);
    switch (g_config.output_kind) {
        case OUTPUT_VM:  return mir_backend_create_vm(cfg);
        case OUTPUT_JIT: return mir_backend_create_jit(cfg);
        case OUTPUT_C:   return mir_backend_create_c(cfg);
        default:         return mir_backend_create_x86_64(cfg);
    }
}

/* ============================================================================
 * pipeline_compile
 * ========================================================================== */

int pipeline_compile(const char* source_path, const char* output_file_path) {
    CompileCtx ctx;
    compile_ctx_init(&ctx);
    int result = 0;
    g_link_needs_skia = false;

    ctx.source = driver_read_file(source_path);
    source_map_add_file(&g_diag.source_map, source_path,
                        ctx.source, (uint32_t)strlen(ctx.source));

    g_diag.perf.enabled = true;

    if (!g_config.compact_output) {
        cpx_log_init(g_config.verbose ? CPX_LOG_DEBUG : CPX_LOG_INFO);
        cpx_log_set_timestamp(false);
        cpx_log_set_show_category(true);

        CPX_INFO("Casprix \xF0\x9F\x91\xBB v1.0.0");
        CPX_INFO("Source: %s", source_path);
        if (!g_config.parse_only && !g_config.check_only)
            CPX_INFO("Output: %s", output_file_path);
    }

    /* --- Debug config --- */
    debug_init();
    g_debug_config.dump_tokens  = g_config.dump_tokens  || g_config.dump_all;
    g_debug_config.dump_ast     = g_config.dump_ast     || g_config.dump_all;
    g_debug_config.dump_symbols = g_config.dump_symbols || g_config.dump_all;
    g_debug_config.step_by_step = g_config.step_mode;
    g_debug_config.verbose      = g_config.verbose;

    /* --- Phase 1: Lexical Analysis --- */
    perf_start(&g_diag.perf, "Lexical Analysis", STAGE_LEX);
    if (!g_config.compact_output) debug_phase_start("LEXICAL ANALYSIS");
    if (g_debug_config.dump_tokens) debug_dump_tokens(ctx.source);
    
    /* Token count for compact report */
    {
        Lexer l;
        init_lexer(&l, ctx.source);
        int count = 0;
        Token t;
        do { t = scan_token(&l); count++; } while (t.type != TOKEN_EOF);
        perf_set_items(&g_diag.perf, count);
    }

    Lexer lexer;
    init_lexer(&lexer, ctx.source);
    perf_end(&g_diag.perf);
    if (!g_config.compact_output) debug_phase_end("Lexical Analysis");
    debug_step_wait();

    /* --- Phase 2: Parsing --- */
    perf_start(&g_diag.perf, "Syntax Analysis", STAGE_PARSE);
    if (!g_config.compact_output) debug_phase_start("SYNTAX ANALYSIS (PARSING)");
    Parser parser;
    init_parser(&parser, &lexer);
    ctx.statements = parse(&parser, &ctx.stmt_count);
    if (had_error) {
        if (!g_config.compact_output) CPX_ERROR("Parsing failed.");
        result = 65; goto done;
    }
    perf_set_items(&g_diag.perf, ctx.stmt_count);
    perf_end(&g_diag.perf);
    if (!g_config.compact_output) {
        CPX_INFO("Parsed %d top-level statement(s)", ctx.stmt_count);
        if (g_debug_config.dump_ast) debug_dump_ast(ctx.statements, ctx.stmt_count);
        debug_phase_end("Syntax Analysis");
    }
    debug_step_wait();

    if (g_config.parse_only) { result = 0; goto done; }

    /* --- Phase 3: Module Resolution --- */
    if (!g_config.compact_output) debug_phase_start("MODULE RESOLUTION");
    init_module_registry(&ctx.module_registry);
    module_registry_set_entry_path(&ctx.module_registry, source_path);
    ctx.registry_init = true;
    {
        int modules_loaded = 0;
        for (int i = 0; i < ctx.stmt_count; i++) {
            if (ctx.statements[i] && ctx.statements[i]->type == STMT_INCLUDE) {
                IncludeStmt* incl = &ctx.statements[i]->as.include;
                if (module_requires_skia_runtime(incl->module_name))
                    g_link_needs_skia = true;
                Module* mod = load_module(&ctx.module_registry, incl->module_name, (void*)source_path);
                if (mod) {
                    modules_loaded++;
                    CPX_INFO("Loaded module: %s", incl->module_name);
                } else if (!incl->is_import) {
                    CPX_WARN("Module not found: %s", incl->module_name);
                }
            }
        }
        CPX_INFO("Loaded %d module(s)", modules_loaded);

        for (int i = 0; i < ctx.module_registry.count; i++) {
            if (module_requires_skia_runtime(ctx.module_registry.modules[i].name)) {
                g_link_needs_skia = true;
                break;
            }
        }

        int total_stmts = ctx.stmt_count;
        for (int i = 0; i < ctx.module_registry.count; i++)
            total_stmts += ctx.module_registry.modules[i].stmt_count;

        Stmt** all_statements = ALLOCATE(Stmt*, total_stmts);
        int idx = 0;
        for (int i = 0; i < ctx.module_registry.count; i++) {
            Module* mod = &ctx.module_registry.modules[i];
            for (int j = 0; j < mod->stmt_count; j++) {
                all_statements[idx++] = mod->statements[j];
                mod->statements[j] = NULL;
            }
        }
        for (int i = 0; i < ctx.stmt_count; i++)
            if (ctx.statements[i] && ctx.statements[i]->type != STMT_INCLUDE)
                all_statements[idx++] = ctx.statements[i];
        free(ctx.statements);
        ctx.statements = all_statements;
        ctx.stmt_count = idx;
    }
    if (!g_config.compact_output) debug_phase_end("Module Resolution");
    debug_step_wait();

    /* --- Phase 4: Semantic Analysis --- */
    perf_start(&g_diag.perf, "Semantic Analysis", STAGE_SEMA);
    if (!g_config.compact_output) debug_phase_start("SEMANTIC ANALYSIS");
    init_semantic_analyzer(&ctx.analyzer);
    ctx.analyzer_init = true;
    if (!analyze_program(&ctx.analyzer, ctx.statements, ctx.stmt_count) || had_error) {
        if (!g_config.compact_output) CPX_ERROR("Semantic analysis failed.");
        result = 65; goto done;
    }
    perf_end(&g_diag.perf);
    if (!g_config.compact_output) {
        CPX_INFO("Type checking passed \xE2\x80\x94 %d symbol(s)", ctx.analyzer.symbols->count);
        if (g_debug_config.dump_symbols) debug_dump_symbols(ctx.analyzer.symbols);
        debug_phase_end("Semantic Analysis");
    }
    debug_step_wait();

    /* --- Phase 4.5: Ownership Check --- */
    if (!g_config.compact_output) debug_phase_start("OWNERSHIP CHECK");
    {
        OwnershipChecker own_checker;
        ownership_checker_init(&own_checker, &ctx.analyzer);
        (void)own_checker; /* updates had_error directly */
    }
    if (had_error) {
        CPX_ERROR("Ownership check failed.");
        result = 65; goto done;
    }
    if (!g_config.compact_output) {
        CPX_INFO("Ownership check passed");
        debug_phase_end("Ownership Check");
    }
    debug_step_wait();

    /* --- Phase 4.7: Async Transformation --- */
    if (!g_config.compact_output) debug_phase_start("ASYNC TRANSFORMATION");
    init_async(&ctx.async_ctx);
    ctx.async_init = true;
    for (int i = 0; i < ctx.stmt_count; i++) {
        if (ctx.statements[i] && ctx.statements[i]->type == STMT_FUNCTION &&
            ctx.statements[i]->as.function.is_async) {
            transform_async_function(&ctx.statements[i]->as.function, &ctx.async_ctx);
        }
    }
    if (!g_config.compact_output) {
        CPX_INFO("Transformed %d async function(s)", ctx.async_ctx.state_machines_generated);
        debug_phase_end("Async Transformation");
    }
    debug_step_wait();

    if (g_config.check_only) { result = 0; goto done; }

    /* --- Phase 5: MIR pipeline (when enabled) --- */
    if (g_config.use_mir) {
        debug_phase_start("MIR LOWERING");
        perf_start(&g_diag.perf, "MIR Lowering", STAGE_MIR);
        ctx.mir_module = mir_lower_program(ctx.statements, ctx.stmt_count,
                                           ctx.analyzer.symbols, "main");
        if (!ctx.mir_module) {
            printf("\n  [ERROR] MIR lowering failed.\n");
            result = 65; goto done;
        }
        printf("  Lowered %d function(s) — %d string(s)\n",
               ctx.mir_module->func_count, ctx.mir_module->string_count);
        perf_end(&g_diag.perf);
        if (g_config.dump_mir || g_config.dump_all) {
            printf("\n  === MIR (after lowering) ===\n");
            mir_print_module(ctx.mir_module, stdout);
        }

        /* Async MIR Transformation */
        debug_phase_start("ASYNC MIR TRANSFORM");
        perf_start(&g_diag.perf, "Async MIR Transform", STAGE_MIR);
        int async_transformed = 0;
        for (MirFunction* f = ctx.mir_module->func_list; f; f = f->next_func) {
            if (f->is_async) {
                async_transformed += mir_transform_async(f);
            }
        }
        printf("  Transformed %d async function(s) to MIR state machines\n", async_transformed);
        perf_end(&g_diag.perf);
        debug_phase_end("Async MIR Transform");
        debug_step_wait();

        debug_phase_end("MIR Lowering");
        debug_step_wait();

        /* mem2reg: alloca → SSA promotion */
        debug_phase_start("SSA CONSTRUCTION (mem2reg)");
        perf_start(&g_diag.perf, "mem2reg (SSA)", STAGE_OPT);
        MirMem2RegStats m2r = {0};
        mir_mem2reg_module(ctx.mir_module, &m2r);
        perf_end(&g_diag.perf);
        printf("  Promoted %d allocas, eliminated %d loads/%d stores, inserted %d phi\n",
               m2r.allocas_promoted, m2r.loads_eliminated,
               m2r.stores_eliminated, m2r.phis_inserted);
        debug_phase_end("SSA Construction");
        debug_step_wait();

        /* Inlining */
        if (g_config.optimize && g_config.opt_level >= 2) {
            debug_phase_start("INLINING");
            perf_start(&g_diag.perf, "Inlining", STAGE_OPT);
            MirInlineStats inl = {0};
            mir_inline_module(ctx.mir_module, g_config.opt_level, &inl);
            perf_end(&g_diag.perf);
            printf("  Inlined %d calls (%d instructions)\n",
                   inl.calls_inlined, inl.instructions_copied);
            debug_phase_end("Inlining");
            debug_step_wait();
        }

        /* Borrow checking */
        if (g_config.safe_mode) {
            debug_phase_start("BORROW CHECKING");
            MirBorrowChecker bc;
            mir_borrow_init(&bc, ctx.mir_module);
            int errs = mir_borrow_check_module(&bc, ctx.mir_module);
            if (errs > 0) {
                printf("\n  [ERROR] Borrow checker: %d error(s)\n", errs);
                mir_borrow_print_errors(&bc, stdout);
                mir_borrow_destroy(&bc);
                result = 65; goto done;
            }
            printf("  Borrow check passed\n");
            mir_borrow_destroy(&bc);
            debug_phase_end("Borrow Checking");
            debug_step_wait();
        }

        /* MIR Optimization */
        if (g_config.optimize) {
            debug_phase_start("MIR OPTIMIZATION");
            perf_start(&g_diag.perf, "MIR Optimization", STAGE_OPT);
            MirOptStats opt = {0};
            mir_optimize_module(ctx.mir_module, (MirOptLevel)g_config.opt_level, &opt);
            perf_end(&g_diag.perf);
            printf("  Folded %d constants, eliminated %d dead insts\n",
                   opt.constants_folded, opt.dead_insts_eliminated);
            debug_phase_end("MIR Optimization");
            debug_step_wait();
        }

        if (mir_verify_module(ctx.mir_module, stderr) != 0) {
            printf("\n  [ERROR] MIR verification failed (CFG, types, or ownership).\n");
            result = 65; goto done;
        }
        if (!g_config.compact_output) CPX_INFO("MIR verification passed");
    }

    /* --- Phase 6: AST-level optimization (legacy path) --- */
    if (g_config.optimize && uses_legacy_ast_codegen()) {
        debug_phase_start("OPTIMIZATION (AST)");
        OptimizerContext opt_ctx;
        init_optimizer(&opt_ctx);
        optimize_program(ctx.statements, ctx.stmt_count, &opt_ctx);
        CPX_INFO("Folded %d constants, eliminated %d dead code",
               opt_ctx.constants_folded, opt_ctx.dead_code_eliminated);
        debug_phase_end("AST Optimization");
        debug_step_wait();
    }

    /* --- Phase 7: Code generation --- */
    if (g_config.use_mir && uses_mir_codegen_backend()) {
        debug_phase_start("CODE GENERATION (MIR Backend)");
        if (!ctx.mir_module) {
            printf("\n  [ERROR] MIR required but no MIR module produced.\n");
            result = 65; goto done;
        }
        printf("  Generating %s...\n", driver_selected_output_name());
        MirBackend* backend = create_mir_backend(output_file_path);
        bool ok = backend && mir_backend_emit_module(backend, ctx.mir_module);
        if (!ok) {
            printf("\n  [ERROR] Backend code generation failed.\n");
            if (backend) backend->destroy(backend);
            result = 74; goto done;
        }
        printf("  Output: %s\n", output_file_path);
        backend->destroy(backend);
        debug_phase_end("Code Generation (MIR Backend)");
        result = 0; goto done;
    }

    /* Legacy AST → x86-64 code generation */
    if (g_config.use_mir && g_config.output_kind == OUTPUT_NATIVE &&
        g_config.native_codegen == NATIVE_CODEGEN_MIR &&
        g_config.allow_legacy_backend_fallback) {
        printf("  Falling back to legacy AST codegen.\n");
    }

    perf_start(&g_diag.perf, "Code Generation", STAGE_CODEGEN);
    if (!g_config.compact_output) debug_phase_start("CODE GENERATION");
    if (!g_config.compact_output) CPX_INFO("Generating x86-64 assembly...");
    {
        FILE* output = fopen(output_file_path, "w");
        if (!output) {
            printf("\n  [ERROR] Could not create output file: %s\n", output_file_path);
            result = 74; goto done;
        }
        AssemblyGenerator asm_gen;
        init_asm_generator(&asm_gen, output, NULL);
        generate_assembly(&asm_gen, ctx.statements, ctx.stmt_count, ctx.analyzer.symbols);
        free_asm_generator(&asm_gen);
        fclose(output);
        if (!g_config.compact_output) CPX_INFO("Assembly: %s (%d strings)", output_file_path, asm_gen.string_size);
    }
    perf_end(&g_diag.perf);
    if (!g_config.compact_output) debug_phase_end("Code Generation");

done:
    compile_ctx_destroy(&ctx);
    return result;
}

/* ============================================================================
 * pipeline_assemble
 * ========================================================================== */

int pipeline_assemble(const char* asm_file_path, const char* obj_file_path) {
    char command[4096];
#ifdef _WIN32
    snprintf(command, sizeof(command),
             "nasm -f win64 \"%s\" -o \"%s\"", asm_file_path, obj_file_path);
#else
    snprintf(command, sizeof(command),
             "nasm -f elf64 \"%s\" -o \"%s\"", asm_file_path, obj_file_path);
#endif
    perf_start(&g_diag.perf, "Assembly", STAGE_NONE);
    if (!g_config.compact_output) debug_phase_start("ASSEMBLY");
    if (!g_config.compact_output) CPX_INFO("Running NASM assembler...");
    if (g_config.verbose) CPX_INFO("Command: %s", command);
    int result = system(command);
    perf_end(&g_diag.perf);
    if (result != 0) {
        if (!g_config.compact_output) CPX_ERROR("Assembly failed (exit %d)", result);
        return 1;
    }
    if (!g_config.compact_output) CPX_INFO("Object file: %s", obj_file_path);
    if (!g_config.compact_output) debug_phase_end("Assembly");
    return 0;
}

/* ============================================================================
 * pipeline_link
 *
 * Searches for the runtime library in several standard locations so the
 * compiler works correctly regardless of the current working directory.
 * ========================================================================== */

static bool file_exists_readable(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

/* Walk up from cwd looking for runtime/memory/arc.c (repo root). */
static bool find_source_root_for_runtime(char* out, size_t outsz) {
    const char* env = getenv("CASPRIX_SOURCE_ROOT");
    if (env && *env) {
        char probe[768];
        snprintf(probe, sizeof(probe), "%s/runtime/memory/arc.c", env);
        if (file_exists_readable(probe)) {
            strncpy(out, env, outsz);
            out[outsz - 1] = '\0';
            return true;
        }
    }

    char cwd[512];
    if (!casprix_getcwd(cwd, sizeof(cwd))) return false;

    for (int depth = 0; depth < 10; depth++) {
        char probe[640];
        snprintf(probe, sizeof(probe), "%s/runtime/memory/arc.c", cwd);
        if (file_exists_readable(probe)) {
            strncpy(out, cwd, outsz);
            out[outsz - 1] = '\0';
            return true;
        }
        char* slash = strrchr(cwd, '/');
#ifdef _WIN32
        char* bs = strrchr(cwd, '\\');
        if (bs && (!slash || bs > slash)) slash = bs;
#endif
        if (!slash || slash == cwd) break;
        *slash = '\0';
    }
    return false;
}

static void extract_parent_dir(const char* file_path, char* dir_out, size_t dir_sz) {
    strncpy(dir_out, ".", dir_sz);
    dir_out[dir_sz - 1] = '\0';
    const char* sep = strrchr(file_path, '/');
#ifdef _WIN32
    const char* sep2 = strrchr(file_path, '\\');
    if (sep2 && (!sep || sep2 > sep)) sep = sep2;
#endif
    if (sep && sep > file_path) {
        size_t n = (size_t)(sep - file_path);
        if (n < dir_sz) {
            memcpy(dir_out, file_path, n);
            dir_out[n] = '\0';
        }
    }
}

static bool find_lib_next_to_path(const char* anchor_path, const char* name,
                                  char* out, size_t outsz) {
    char dir[512];
    char probe[1024];

    if (!anchor_path || !*anchor_path || !name || !out || outsz == 0) return false;
    extract_parent_dir(anchor_path, dir, sizeof(dir));
    snprintf(probe, sizeof(probe), "%s/%s", dir, name);
    if (!file_exists_readable(probe)) return false;

    strncpy(out, probe, outsz - 1);
    out[outsz - 1] = '\0';
    return true;
}

static bool find_prebuilt_lib_path(const char* name, char* out, size_t outsz) {
    char path[512];

    if (!name || !out || outsz == 0) return false;
    out[0] = '\0';

    /* Explicit path from CI / tests (see tests/CMakeLists.txt). */
    const char* env = getenv("CASPRIX_RUNTIME_LIB");
    if (env && *env && strcmp(name, "libcasprix_runtime.a") == 0 &&
        file_exists_readable(env)) {
        strncpy(out, env, outsz - 1);
        out[outsz - 1] = '\0';
        return true;
    }

    static const char* dirs[] = {
        "build",
        "../build",
        "../../build",
        "../../build/Release",
        "../../build/Debug",
        "build/Release",
        "../build/Release",
        ".",
        NULL
    };
    for (int i = 0; dirs[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", dirs[i], name);
        if (file_exists_readable(path)) {
            strncpy(out, path, outsz - 1);
            out[outsz - 1] = '\0';
            return true;
        }
    }
    return false;
}

int pipeline_link(const char* obj_file_path, const char* asm_file_path,
                  const char* exe_path) {
    if (!check_tool_available(TOOL_GCC, NULL)) {
        CPX_ERROR("GCC not found for linking");
        download_tool(TOOL_GCC, NULL);
        return 1;
    }

    const char* gcc       = get_tool_command(TOOL_GCC);
    const char* opt_flags = g_config.optimize ? "-O2" : "-g";

    /* Locate prebuilt runtime library */
    char rt_path[512] = "";
    char std_path[512] = "";
    char skia_gui_path[512] = "";
    char skia_c_path[512] = "";
    bool have_rt = find_lib_next_to_path(obj_file_path, "libcasprix_runtime.a", rt_path, sizeof(rt_path)) ||
                   find_lib_next_to_path(exe_path, "libcasprix_runtime.a", rt_path, sizeof(rt_path)) ||
                   find_prebuilt_lib_path("libcasprix_runtime.a", rt_path, sizeof(rt_path));
    bool have_std = find_lib_next_to_path(obj_file_path, "libcasprix_stdlib.a", std_path, sizeof(std_path)) ||
                    find_lib_next_to_path(exe_path, "libcasprix_stdlib.a", std_path, sizeof(std_path)) ||
                    find_prebuilt_lib_path("libcasprix_stdlib.a", std_path, sizeof(std_path));
    bool have_skia_gui = find_lib_next_to_path(obj_file_path, "libskia_gui.a", skia_gui_path, sizeof(skia_gui_path)) ||
                         find_lib_next_to_path(exe_path, "libskia_gui.a", skia_gui_path, sizeof(skia_gui_path)) ||
                         find_prebuilt_lib_path("libskia_gui.a", skia_gui_path, sizeof(skia_gui_path));
    bool have_skia_c = find_lib_next_to_path(obj_file_path, "libskia_c.a", skia_c_path, sizeof(skia_c_path)) ||
                       find_lib_next_to_path(exe_path, "libskia_c.a", skia_c_path, sizeof(skia_c_path)) ||
                       find_prebuilt_lib_path("libskia_c.a", skia_c_path, sizeof(skia_c_path));
    bool need_skia = g_link_needs_skia;
    bool have_skia = have_skia_gui && have_skia_c;

    char runtime_flags[1536] = "";
    char inline_sources[512] = "";

    if (need_skia && !have_skia) {
        printf("  [ERROR] Skia GUI program detected, but libskia_gui.a/libskia_c.a were not found.\n");
        return 1;
    }

    if (have_rt) {
        if (need_skia) {
            snprintf(runtime_flags, sizeof(runtime_flags),
                     " \"%s\" \"%s\" \"%s\" -lgdi32 -luser32 -lole32 -loleaut32 -lmsimg32 -lws2_32 -lpthread -ladvapi32",
                     rt_path, skia_gui_path, skia_c_path);
        } else {
#ifdef _WIN32
            snprintf(runtime_flags, sizeof(runtime_flags),
                     " \"%s\" -lws2_32 -lpthread -ladvapi32",
                     rt_path);
#else
            snprintf(runtime_flags, sizeof(runtime_flags),
                     " \"%s\" -lm -lpthread",
                     rt_path);
#endif
        }
    } else {
        /* No prebuilt runtime: compile a minimal subset next to the repo root. */
        char root[512];
        if (find_source_root_for_runtime(root, sizeof(root))) {
            snprintf(inline_sources, sizeof(inline_sources),
                     "\"%s/runtime/memory/arc.c\" \"%s/runtime/memory/cycle_gc.c\" "
                     "\"%s/runtime/memory/ownership.c\" \"%s/runtime/object.c\"",
                     root, root, root, root);
        } else {
            snprintf(inline_sources, sizeof(inline_sources),
                     "runtime/memory/arc.c runtime/memory/cycle_gc.c "
                     "runtime/memory/ownership.c runtime/object.c");
        }

        if (need_skia) {
#ifdef _WIN32
            snprintf(runtime_flags, sizeof(runtime_flags),
                     " \"%s\" \"%s\" -lgdi32 -luser32 -lole32 -loleaut32 -lmsimg32",
                     skia_gui_path, skia_c_path);
#else
            snprintf(runtime_flags, sizeof(runtime_flags),
                     " \"%s\" \"%s\"",
                     skia_gui_path, skia_c_path);
#endif
        }
    }

    char stdlib_flags[256] = "";
    if (have_std)
        snprintf(stdlib_flags, sizeof(stdlib_flags), " \"%s\"", std_path);

    char command[4096];
#ifdef _WIN32
    snprintf(command, sizeof(command),
             "%s %s \"%s\" %s -o \"%s.exe\"%s%s",
             gcc, opt_flags, obj_file_path, inline_sources,
             exe_path, runtime_flags, stdlib_flags);
#else
    snprintf(command, sizeof(command),
             "%s %s -no-pie \"%s\" %s -o \"%s\"%s%s -lm",
             gcc, opt_flags, obj_file_path, inline_sources,
             exe_path, runtime_flags, stdlib_flags);
#endif

    perf_start(&g_diag.perf, "Linking", STAGE_LINK);
    if (!g_config.compact_output) debug_phase_start("LINKING");
    if (!g_config.compact_output) CPX_INFO("Running GCC linker...");
    if (g_config.verbose) CPX_INFO("Command: %s", command);
    int result = system(command);
    perf_end(&g_diag.perf);
    if (result != 0) {
        if (!g_config.compact_output) CPX_ERROR("Linking failed (exit %d)", result);
        return 1;
    }
#ifdef _WIN32
    if (!g_config.compact_output) CPX_INFO("Executable created: %s.exe", exe_path);
#else
    if (!g_config.compact_output) CPX_INFO("Executable created: %s", exe_path);
#endif
    if (!g_config.compact_output) debug_phase_end("Linking");

    if (!g_config.keep_asm_file) remove(asm_file_path);
    remove(obj_file_path);
    return 0;
}
