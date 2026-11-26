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
#include <errno.h>
#include <stdint.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define casprix_getcwd(buf, sz) _getcwd((buf), (int)(sz))
#else
#include <unistd.h>
#define casprix_getcwd(buf, sz) getcwd((buf), (sz))
#include <sys/types.h>
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
#include "compiler/ir/mir_pipeline.h"
#include "compiler/ir/mir_backend.h"
#include "compiler/ir/mir_backend_caps.h"
#include "util/tools.h"
#include "util/process.h"
#include "util/module.h"

/* CVM interpreter — for `--execute` (in-process run, no bytecode file). */
#include "../../runtime/vm/cvm_engine.h"
#include "../../runtime/vm/jit_bridge.h"

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
static bool g_link_needs_ai_core = false;

static void compile_ctx_init(CompileCtx* ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

static void compile_ctx_destroy(CompileCtx* ctx) {
    if (ctx->mir_module)    { mir_module_destroy(ctx->mir_module);    ctx->mir_module = NULL; }
    if (ctx->analyzer_init) { free_semantic_analyzer(&ctx->analyzer); ctx->analyzer_init = false; }
    if (ctx->async_init)    { free_async(&ctx->async_ctx); ctx->async_init = false; }
    /* Module statement slots are nulled when their nodes move into this
     * combined array. Destroy the registry containers first, then the one
     * authoritative AST root set held here. */
    if (ctx->registry_init) { free_module_registry(&ctx->module_registry); ctx->registry_init = false; }
    if (ctx->statements) {
        for (int i = 0; i < ctx->stmt_count; i++)
            free_stmt(ctx->statements[i]);
    }
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

static bool module_requires_ai_core(const char* module_name) {
    if (!module_name || !*module_name) return false;
    return strcmp(module_name, "lib/std/ai/tensor.cpx") == 0 ||
           strcmp(module_name, "lib/std/ai/tensor") == 0 ||
           strcmp(module_name, "std/ai/tensor") == 0;
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
        .entry_symbol  = g_config.c_entry_symbol,
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

/* Forward decls for helpers defined below pipeline_compile. */
static bool find_source_root_for_runtime(char* out, size_t outsz);
static bool find_prebuilt_lib_path(const char* name, char* out, size_t outsz);

#ifndef CASPRIX_EXTERNAL_SANITIZER_FLAGS
#define CASPRIX_EXTERNAL_SANITIZER_FLAGS ""
#endif
#ifndef CASPRIX_SANITIZER_CONFIG
#define CASPRIX_SANITIZER_CONFIG ""
#endif
#ifndef CASPRIX_RUNTIME_ABI
#define CASPRIX_RUNTIME_ABI "unknown"
#endif

typedef struct {
    const char* args[80];
    size_t count;
} DriverArgv;

static bool argv_add(DriverArgv* args, const char* value) {
    if (!args || !value || args->count + 1 >= sizeof(args->args) / sizeof(args->args[0]))
        return false;
    args->args[args->count++] = value;
    args->args[args->count] = NULL;
    return true;
}

static void log_external_command(const DriverArgv* args) {
    if (!g_config.verbose || !args) return;
    fprintf(stderr, "Command:");
    for (size_t i = 0; i < args->count; ++i) fprintf(stderr, " \"%s\"", args->args[i]);
    fputc('\n', stderr);
}

static bool runtime_archive_matches_build(const char* archive_path) {
    if (!archive_path || !*archive_path) return false;
    size_t n = strlen(archive_path);
    static const char suffix[] = ".casprix-sanitizers";
    char* metadata = (char*)malloc(n + sizeof(suffix));
    if (!metadata) return false;
    memcpy(metadata, archive_path, n);
    memcpy(metadata + n, suffix, sizeof(suffix));
    FILE* file = fopen(metadata, "rb");
    free(metadata);
    if (!file) return CASPRIX_SANITIZER_CONFIG[0] == '\0';

    char line[512];
    bool version_ok = false, sanitizers_ok = false, abi_ok = false;
    if (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        version_ok = strcmp(line, "casprix-runtime-v1") == 0;
    }
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "sanitizers=", 11) == 0)
            sanitizers_ok = strcmp(line + 11, CASPRIX_SANITIZER_CONFIG) == 0;
        else if (strncmp(line, "abi=", 4) == 0)
            abi_ok = strcmp(line + 4, CASPRIX_RUNTIME_ABI) == 0;
    }
    fclose(file);
    return version_ok && sanitizers_ok && abi_ok;
}

static bool write_sanitizer_probe(const char* path) {
    FILE* file = fopen(path, "wb");
    if (!file) return false;
    static const char source[] =
        "#if defined(__GNUC__)\n"
        "__attribute__((noinline))\n"
        "#endif\n"
        "static int probe(int value) { return value + 1; }\n"
        "int main(int argc, char **argv) { (void)argv; return probe(argc) == -17; }\n";
    bool ok = fwrite(source, 1, sizeof(source) - 1, file) == sizeof(source) - 1;
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool external_sanitizer_toolchain_works(const char* compiler) {
    if (!CASPRIX_EXTERNAL_SANITIZER_FLAGS[0]) return true;
    static unsigned long counter = 0;
    char base[1024], source[1080], output[1120], temp_output[1200];
    const char* temp_dir = getenv(
#ifdef _WIN32
        "TEMP"
#else
        "TMPDIR"
#endif
    );
    if (!temp_dir || !*temp_dir) {
#ifdef _WIN32
        temp_dir = ".";
#else
        temp_dir = "/tmp";
#endif
    }
#ifdef _WIN32
    long pid = (long)_getpid();
    const char* separator = "\\";
#else
    long pid = (long)getpid();
    const char* separator = "/";
#endif
    int n = snprintf(base, sizeof(base), "%s%scasprix-sanitizer-probe-%ld-%lu",
                     temp_dir, separator, pid, ++counter);
    if (n < 0 || (size_t)n >= sizeof(base) ||
        snprintf(source, sizeof(source), "%s.c", base) >= (int)sizeof(source) ||
        snprintf(output, sizeof(output), "%s.out", base) >= (int)sizeof(output))
        return false;
    if (!casprix_reserve_sibling_temp(output, temp_output, sizeof(temp_output))) return false;
    if (!write_sanitizer_probe(source)) {
        remove(temp_output);
        return false;
    }

    DriverArgv args = {0};
    argv_add(&args, compiler);
    argv_add(&args, CASPRIX_EXTERNAL_SANITIZER_FLAGS);
    argv_add(&args, "-fno-omit-frame-pointer");
    argv_add(&args, "-g");
    argv_add(&args, source);
    argv_add(&args, "-o");
    argv_add(&args, temp_output);
#ifndef _WIN32
    argv_add(&args, "-no-pie");
#endif
    log_external_command(&args);
    int result = casprix_run_process(args.args, false);
    remove(source);
    remove(temp_output);
    if (result != 0) {
        fprintf(stderr,
            "Casprix sanitizer capability error: external compiler '%s' cannot compile and link '%s'.\n"
            "Requested configuration: %s. Install a matching sanitizer runtime or use a supported external compiler.\n",
            compiler, CASPRIX_SANITIZER_CONFIG, CASPRIX_SANITIZER_CONFIG);
        return false;
    }
    return true;
}

static bool make_final_executable_path(const char* base, char* final_path, size_t size) {
#ifdef _WIN32
    int n = snprintf(final_path, size, "%s.exe", base);
#else
    int n = snprintf(final_path, size, "%s", base);
#endif
    return n >= 0 && (size_t)n < size;
}

static void cleanup_native_intermediates(const char* asm_path, const char* obj_path) {
    if (obj_path) remove(obj_path);
    if (!g_config.keep_asm_file && asm_path) remove(asm_path);
}

/* ============================================================================
 * pipeline_compile
 * ========================================================================== */

int pipeline_compile(const char* source_path, const char* output_file_path) {
    CompileCtx ctx;
    compile_ctx_init(&ctx);
    int result = 0;
    g_link_needs_skia = false;
    g_link_needs_ai_core = false;

    ctx.source = driver_read_file(source_path);
    if (!ctx.source) {
        result = 66;
        goto done;
    }
    if (source_map_add_file(&g_diag.source_map, source_path,
                            ctx.source, (uint32_t)strlen(ctx.source)) == UINT16_MAX) {
        result = 74;
        goto done;
    }

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
        do {
            t = scan_token(&l);
            count++;
            if (t.type == TOKEN_STRING) free(t.literal.string_value);
        } while (t.type != TOKEN_EOF);
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
    parser_destroy(&parser);
    if (g_debug_config.dump_ast) {
        debug_dump_ast(ctx.statements, ctx.stmt_count);
    }
    if (had_error) {
        if (!g_config.compact_output) CPX_ERROR("Parsing failed.");
        result = 65; goto done;
    }
    perf_set_items(&g_diag.perf, ctx.stmt_count);
    perf_end(&g_diag.perf);
    if (!g_config.compact_output) {
        CPX_INFO("Parsed %d top-level statement(s)", ctx.stmt_count);
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
        CPX_INFO("Analyzing %d statements for modules", ctx.stmt_count);
        for (int i = 0; i < ctx.stmt_count; i++) {
            if (ctx.statements[i]) {
                CPX_DEBUG("Stmt %d: type %d", i, ctx.statements[i]->type);
                if (ctx.statements[i]->type == STMT_INCLUDE) {
                    IncludeStmt* incl = &ctx.statements[i]->as.include;
                    CPX_INFO("Found include/import: %s", incl->module_name);
                    if (module_requires_skia_runtime(incl->module_name))
                        g_link_needs_skia = true;
                    if (module_requires_ai_core(incl->module_name))
                        g_link_needs_ai_core = true;
                    Module* mod = load_module(&ctx.module_registry, incl->module_name, (void*)source_path);
                    if (mod) {
                        modules_loaded++;
                        CPX_INFO("Successfully loaded module: %s", incl->module_name);
                    } else if (!incl->is_import) {
                        CPX_WARN("Module not found: %s", incl->module_name);
                    }
                }
            }
        }
        CPX_INFO("Loaded %d module(s)", modules_loaded);

        for (int i = 0; i < ctx.module_registry.count; i++) {
            if (module_requires_skia_runtime(ctx.module_registry.modules[i].name)) {
                g_link_needs_skia = true;
            }
            if (module_requires_ai_core(ctx.module_registry.modules[i].name))
                g_link_needs_ai_core = true;
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
        /* Keep import nodes in the merged AST.  The semantic and lowering
         * passes intentionally ignore STMT_INCLUDE, but retaining the nodes
         * preserves import metadata (including aliases) for AST/debug users
         * after module resolution. */
        for (int i = 0; i < ctx.stmt_count; i++)
            if (ctx.statements[i])
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

    /* --- Phase 4.5: Ownership (runs inside sema; DEF-003) ---
     * Loan/move checking is performed during analyze_program via
     * g_ownership_ctx. The previous init-only stage is removed. */
    if (!g_config.compact_output) {
        debug_phase_start("OWNERSHIP CHECK");
        if (had_error) {
            CPX_ERROR("Ownership check failed.");
            result = 65; goto done;
        }
        CPX_INFO("Ownership analysis completed during semantic analysis");
        debug_phase_end("Ownership Check");
    } else if (had_error) {
        result = 65; goto done;
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

    /* Plain --check-only stops after frontend validation.
     * --safe-mode / --mir with --check-only must still run MIR verify (DEF-010). */
    if (g_config.check_only && !g_config.use_mir && !g_config.safe_mode) {
        result = 0; goto done;
    }

    /* --- Phase 5: canonical MIR preparation (shared across consumers) --- */
    if (g_config.use_mir || g_config.safe_mode) {
        MirPipelineOptions mir_opts;
        MirPipelineResult mir_res;

        g_config.use_mir = true;
        debug_phase_start("MIR PIPELINE");
        perf_start(&g_diag.perf, "MIR Pipeline", STAGE_MIR);

        mir_pipeline_options_init(&mir_opts);
        mir_opts.optimize = g_config.optimize;
        mir_opts.opt_level = g_config.opt_level;
        mir_opts.run_inline = g_config.optimize && g_config.opt_level >= 2;
        mir_opts.safe_mode = g_config.safe_mode;
        mir_opts.dump_mir = g_config.dump_mir || g_config.dump_all;
        mir_opts.module_name = "main";

        mir_res = mir_pipeline_prepare(ctx.statements, ctx.stmt_count,
                                       ctx.analyzer.symbols, &mir_opts);
        if (mir_res.status != MIR_PIPE_OK) {
            const char* why = "internal error";
            if (mir_res.status == MIR_PIPE_ERR_LOWER) why = "lowering failed";
            else if (mir_res.status == MIR_PIPE_ERR_VERIFY) why = "verification failed";
            else if (mir_res.status == MIR_PIPE_ERR_BORROW) why = "borrow check failed";
            printf("\n  [ERROR] MIR pipeline: %s.\n", why);
            result = 65;
            goto done;
        }
        ctx.mir_module = mir_res.module;
        printf("  Prepared MIR: %d function(s), %d alloca(s) promoted, "
               "%d call(s) inlined, %d async transform(s)\n",
               ctx.mir_module->func_count, mir_res.allocas_promoted,
               mir_res.calls_inlined, mir_res.async_transformed);
        perf_end(&g_diag.perf);
        debug_phase_end("MIR Pipeline");
        debug_step_wait();

        /* --check-only with --safe-mode/--mir: validation complete; no codegen. */
        if (g_config.check_only) {
            result = 0;
            goto done;
        }

        /* --- Phase 5.9: in-process CVM execution (--execute) --- */
        if (g_config.execute) {
            debug_phase_start("EXECUTE (CVM)");

            const char* entry = "main";
            if (!mir_module_find_function(ctx.mir_module, entry)) {
                entry = "__casprix_entry";
            }
            if (!mir_module_find_function(ctx.mir_module, entry)) {
                printf("\n  [ERROR] --execute: no entry point (define `func main()` "
                       "or provide top-level statements).\n");
                result = 65; goto done;
            }

            if (!mir_backend_caps_validate(ctx.mir_module, MIR_CAP_CVM, stderr)) {
                printf("\n  [ERROR] --execute: MIR contains ops unsupported by CVM.\n");
                result = 65; goto done;
            }

            if (!g_config.compact_output)
                CPX_INFO("Executing '%s' via CVM (JIT tier-up enabled)...", entry);

            CvmJitBridge* jit = cjb_create();
            CvmState* vm = cvm_state_create(ctx.mir_module, jit);
            if (!vm) {
                printf("\n  [ERROR] --execute: could not create CVM state.\n");
                if (jit) cjb_destroy(jit);
                result = 74; goto done;
            }

            int64_t rc = cvm_run(vm, entry, /*args=*/NULL, /*n_args=*/0);
            int trap = vm->trap_code;
            if (g_config.verbose) {
                cvm_print_stats(vm, stdout);
                if (jit) cjb_print_stats(jit, stdout);
            }
            cvm_state_destroy(vm);
            if (jit) cjb_destroy(jit);

            debug_phase_end("Execute (CVM)");

            if (trap != 0) {
                printf("\n  [ERROR] --execute: program trapped (code %d).\n", trap);
                result = 70; goto done;
            }

            result = (int)(rc & 0xFF);
            goto done;
        }
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
        MirCapabilityProfile cap;
        char c_path[1024];
        char exe_path[1024];
        const char* emit_path = output_file_path;
        bool mir_c_native = (g_config.output_kind == OUTPUT_NATIVE &&
                             g_config.native_codegen == NATIVE_CODEGEN_MIR &&
                             !g_config.allow_legacy_backend_fallback);

        debug_phase_start("CODE GENERATION (MIR Backend)");
        if (!ctx.mir_module) {
            printf("\n  [ERROR] MIR required but no MIR module produced.\n");
            result = 65; goto done;
        }

        /* Capability gate before emission — never silently omit ops. */
        if (mir_c_native)
            cap = MIR_CAP_MIR_C_NATIVE;
        else if (g_config.output_kind == OUTPUT_C)
            cap = MIR_CAP_EMIT_C;
        else if (g_config.output_kind == OUTPUT_VM)
            cap = MIR_CAP_VM_BYTECODE;
        else if (g_config.output_kind == OUTPUT_JIT)
            cap = MIR_CAP_JIT_PACKAGE;
        else
            cap = MIR_CAP_MIR_X86_ASM;

        if (!mir_backend_caps_validate(ctx.mir_module, cap, stderr)) {
            printf("\n  [ERROR] Selected backend '%s' cannot lower this MIR module.\n",
                   mir_capability_profile_name(cap));
            if (mir_c_native) {
                printf("  Hint: use default native (legacy AST) or --execute (CVM).\n");
            }
            result = 65; goto done;
        }

        /* Transitional MIR-native: MIR → C → host C compiler → executable.
         * This is not direct MIR→assembly; see CLI help for --native/--aot. */
        if (mir_c_native) {
            size_t n = strlen(output_file_path);
            if (n + 8 >= sizeof(c_path)) {
                printf("\n  [ERROR] Output path too long for MIR→C staging.\n");
                result = 74; goto done;
            }
            snprintf(c_path, sizeof(c_path), "%s", output_file_path);
            /* Prefer replacing .asm with .c; otherwise append .c. */
            if (n > 4 && strcmp(c_path + n - 4, ".asm") == 0)
                memcpy(c_path + n - 4, ".c", 3);
            else
                snprintf(c_path, sizeof(c_path), "%s.c", output_file_path);

            /* Derive executable base (without .exe); GCC adds .exe on Windows. */
            snprintf(exe_path, sizeof(exe_path), "%s", output_file_path);
            n = strlen(exe_path);
            if (n > 4 && strcmp(exe_path + n - 4, ".asm") == 0)
                exe_path[n - 4] = '\0';

            printf("  Generating MIR→C→native (%s)...\n", driver_selected_output_name());
            {
                MirBackendConfig cfg = make_mir_backend_config(c_path);
                cfg.output_format = MIR_OUTPUT_C;
                cfg.pic = true;
                MirBackend* backend = mir_backend_create_c(cfg);
                bool ok = backend && mir_backend_emit_module(backend, ctx.mir_module);
                if (backend) backend->destroy(backend);
                if (!ok) {
                    printf("\n  [ERROR] MIR→C emission failed.\n");
                    result = 74; goto done;
                }
            }

            {
                const char* gcc = get_tool_command(TOOL_GCC);
                const char* opt_flags = g_config.optimize ? "-O2" : "-g";
                char rt_path[1024] = "";
                char root[1024] = "";
                char inline_sources[4][1200];
                size_t source_count = 0;
                char final_path[1200], temp_path[1300];
                bool have_rt = find_prebuilt_lib_path("libcasprix_runtime.a",
                                                     rt_path, sizeof(rt_path));
                if (have_rt && !runtime_archive_matches_build(rt_path)) {
                    fprintf(stderr,
                        "Casprix runtime compatibility error: '%s' does not match this compiler's sanitizer/target configuration (%s, %s).\n",
                        rt_path, CASPRIX_SANITIZER_CONFIG, CASPRIX_RUNTIME_ABI);
                    if (!g_config.keep_asm_file) remove(c_path);
                    result = 74; goto done;
                }
                if (!external_sanitizer_toolchain_works(gcc)) {
                    if (!g_config.keep_asm_file) remove(c_path);
                    result = 74; goto done;
                }
                if (!make_final_executable_path(exe_path, final_path, sizeof(final_path)) ||
                    !casprix_reserve_sibling_temp(final_path, temp_path, sizeof(temp_path))) {
                    fprintf(stderr, "Casprix: could not reserve a temporary executable beside '%s'.\n",
                            exe_path);
                    if (!g_config.keep_asm_file) remove(c_path);
                    result = 74; goto done;
                }
                if (!have_rt && find_source_root_for_runtime(root, sizeof(root))) {
                    static const char* suffixes[] = {
                        "runtime/memory/arc.c", "runtime/memory/cycle_gc.c",
                        "runtime/memory/ownership.c", "runtime/object.c"
                    };
                    for (size_t i = 0; i < 4; ++i) {
                        snprintf(inline_sources[i], sizeof(inline_sources[i]),
                                 "%s/%s", root, suffixes[i]);
                        source_count++;
                    }
                }

                DriverArgv args = {0};
                argv_add(&args, gcc);
                argv_add(&args, opt_flags);
                if (CASPRIX_EXTERNAL_SANITIZER_FLAGS[0]) {
                    argv_add(&args, CASPRIX_EXTERNAL_SANITIZER_FLAGS);
                    argv_add(&args, "-fno-omit-frame-pointer");
                }
#ifdef _WIN32
                argv_add(&args, c_path);
                for (size_t i = 0; i < source_count; ++i) argv_add(&args, inline_sources[i]);
                argv_add(&args, "-o");
                argv_add(&args, temp_path);
                if (have_rt) argv_add(&args, rt_path);
                argv_add(&args, "-lws2_32");
                argv_add(&args, "-lpthread");
                argv_add(&args, "-ladvapi32");
#else
                argv_add(&args, "-no-pie");
                argv_add(&args, c_path);
                for (size_t i = 0; i < source_count; ++i) argv_add(&args, inline_sources[i]);
                argv_add(&args, "-o");
                argv_add(&args, temp_path);
                if (have_rt) argv_add(&args, rt_path);
                argv_add(&args, "-lm");
                argv_add(&args, "-lpthread");
#endif
                log_external_command(&args);
                int tool_result = casprix_run_process(args.args, false);
                if (tool_result != 0) {
                    fprintf(stderr,
                        "Casprix: host C compilation/link failed (tool exit %d); no executable was published.\n",
                        tool_result);
                    remove(temp_path);
                    if (!g_config.keep_asm_file) remove(c_path);
                    result = 74; goto done;
                }
                if (!casprix_publish_file(temp_path, final_path)) {
                    fprintf(stderr,
                        "Casprix: could not publish completed executable '%s' (errno %d).\n",
                        final_path, errno);
                    remove(temp_path);
                    if (!g_config.keep_asm_file) remove(c_path);
                    result = 74; goto done;
                }
            }
            if (!g_config.keep_asm_file) remove(c_path);
            g_config.codegen_produced_final_artifact = true;
            {
                char final_path[1200];
                if (make_final_executable_path(exe_path, final_path, sizeof(final_path)))
                    printf("  Output: %s (via MIR→C→host-cc)\n", final_path);
            }
            debug_phase_end("Code Generation (MIR Backend)");
            result = 0; goto done;
        }

        printf("  Generating %s...\n", driver_selected_output_name());
        {
            MirBackend* backend = create_mir_backend(emit_path);
            bool ok = backend && mir_backend_emit_module(backend, ctx.mir_module);
            if (!ok) {
                printf("\n  [ERROR] Backend code generation failed.\n");
                if (backend) backend->destroy(backend);
                result = 74; goto done;
            }
            printf("  Output: %s\n", emit_path);
            backend->destroy(backend);
        }
        /* Non-native MIR artifacts are final; native MIR x86 asm would still
         * need assemble/link, but that path is not selected for --native. */
        if (g_config.output_kind != OUTPUT_NATIVE)
            g_config.codegen_produced_final_artifact = true;
        debug_phase_end("Code Generation (MIR Backend)");
        result = 0; goto done;
    }

    /* Legacy AST → x86-64 code generation (default native path). */
    if (g_config.use_mir && g_config.output_kind == OUTPUT_NATIVE &&
        g_config.native_codegen == NATIVE_CODEGEN_MIR &&
        g_config.allow_legacy_backend_fallback) {
        printf("  Falling back to legacy AST codegen (--legacy-backend).\n");
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
    DriverArgv args = {0};
    argv_add(&args, get_tool_command(TOOL_NASM));
#ifdef _WIN32
    argv_add(&args, "-f");
    argv_add(&args, "win64");
#else
    argv_add(&args, "-f");
    argv_add(&args, "elf64");
#endif
    argv_add(&args, asm_file_path);
    argv_add(&args, "-o");
    argv_add(&args, obj_file_path);
    perf_start(&g_diag.perf, "Assembly", STAGE_NONE);
    if (!g_config.compact_output) debug_phase_start("ASSEMBLY");
    if (!g_config.compact_output) CPX_INFO("Running NASM assembler...");
    log_external_command(&args);
    int result = casprix_run_process(args.args, false);
    perf_end(&g_diag.perf);
    if (result != 0) {
        if (!g_config.compact_output)
            CPX_ERROR("NASM invocation failed (exit %d)", result);
        remove(obj_file_path);
        if (!g_config.keep_asm_file) remove(asm_file_path);
        return 74;
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
        char probe[2048];
        snprintf(probe, sizeof(probe), "%s/runtime/memory/arc.c", env);
        if (file_exists_readable(probe)) {
            snprintf(out, outsz, "%s", env);
            return true;
        }
    }

    char cwd[1024];
    if (!casprix_getcwd(cwd, sizeof(cwd))) return false;

    for (int depth = 0; depth < 10; depth++) {
        char probe[2048];
        snprintf(probe, sizeof(probe), "%s/runtime/memory/arc.c", cwd);
        if (file_exists_readable(probe)) {
            snprintf(out, outsz, "%s", cwd);
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
    snprintf(dir_out, dir_sz, ".");
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

    snprintf(out, outsz, "%s", probe);
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
        snprintf(out, outsz, "%s", env);
        return true;
    }

    static const char* dirs[] = {
        "build",
        "build-gcc",
        "../build",
        "../build-gcc",
        "../../build",
        "../../build-gcc",
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
            snprintf(out, outsz, "%s", path);
            return true;
        }
    }
    return false;
}

int pipeline_link(const char* obj_file_path, const char* asm_file_path,
                  const char* exe_path) {
    const char* gcc       = get_tool_command(TOOL_GCC);
    const char* opt_flags = g_config.optimize ? "-O2" : "-g";

    /* Locate prebuilt runtime library */
    char rt_path[1024] = "";
    char std_path[1024] = "";
    char skia_gui_path[1024] = "";
    char skia_c_path[1024] = "";
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
    bool need_ai_core = g_link_needs_ai_core;
    bool have_skia = have_skia_gui && have_skia_c;

    if (need_skia && !have_skia) {
        printf("  [ERROR] Skia GUI program detected, but libskia_gui.a/libskia_c.a were not found.\n");
        cleanup_native_intermediates(asm_file_path, obj_file_path);
        return 74;
    }
    if (have_rt && !runtime_archive_matches_build(rt_path)) {
        fprintf(stderr,
            "Casprix runtime compatibility error: '%s' does not match this compiler's sanitizer/target configuration (%s, %s).\n",
            rt_path, CASPRIX_SANITIZER_CONFIG, CASPRIX_RUNTIME_ABI);
        cleanup_native_intermediates(asm_file_path, obj_file_path);
        return 74;
    }
    if (!external_sanitizer_toolchain_works(gcc)) {
        cleanup_native_intermediates(asm_file_path, obj_file_path);
        return 74;
    }

    char final_path[1200], temp_path[1300];
    if (!make_final_executable_path(exe_path, final_path, sizeof(final_path)) ||
        !casprix_reserve_sibling_temp(final_path, temp_path, sizeof(temp_path))) {
        fprintf(stderr, "Casprix: could not reserve a temporary executable beside '%s'.\n", exe_path);
        cleanup_native_intermediates(asm_file_path, obj_file_path);
        return 74;
    }

    DriverArgv args = {0};
    argv_add(&args, gcc);
    argv_add(&args, opt_flags);
    if (CASPRIX_EXTERNAL_SANITIZER_FLAGS[0]) {
        argv_add(&args, CASPRIX_EXTERNAL_SANITIZER_FLAGS);
        argv_add(&args, "-fno-omit-frame-pointer");
    }
#ifdef _WIN32
    argv_add(&args, obj_file_path);
#else
    argv_add(&args, "-no-pie");
    argv_add(&args, obj_file_path);
#endif

    char root[1024] = "";
    char inline_sources[7][1200];
    if (!have_rt) {
        bool have_root = find_source_root_for_runtime(root, sizeof(root));
        static const char* basic_sources[] = {
            "runtime/memory/arc.c", "runtime/memory/cycle_gc.c",
            "runtime/memory/ownership.c", "runtime/object.c"
        };
        static const char* ai_sources[] = {
            "runtime/ai/core/tensor.c", "runtime/ai/core/autograd.c",
            "runtime/ai/core/bindings.c"
        };
        size_t basic_count = sizeof(basic_sources) / sizeof(basic_sources[0]);
        for (size_t i = 0; i < basic_count + (need_ai_core ? 3u : 0u); ++i) {
            const char* suffix = i < basic_count ? basic_sources[i] : ai_sources[i - basic_count];
            if (have_root) {
                snprintf(inline_sources[i], sizeof(inline_sources[i]),
                         "%s/%s", root, suffix);
                argv_add(&args, inline_sources[i]);
            } else {
                argv_add(&args, suffix);
            }
        }
    }

    argv_add(&args, "-o");
    argv_add(&args, temp_path);
    if (have_rt) argv_add(&args, rt_path);
    if (need_skia) {
        argv_add(&args, skia_gui_path);
        argv_add(&args, skia_c_path);
    }
    if (have_std) argv_add(&args, std_path);

#ifdef _WIN32
    if (need_skia) {
        argv_add(&args, "-lgdi32"); argv_add(&args, "-luser32");
        argv_add(&args, "-lole32"); argv_add(&args, "-loleaut32");
        argv_add(&args, "-lmsimg32");
    }
    if (have_rt) {
        argv_add(&args, "-lws2_32"); argv_add(&args, "-lpthread");
        argv_add(&args, "-ladvapi32");
    }
#else
    argv_add(&args, "-lm");
    if (have_rt) argv_add(&args, "-lpthread");
#endif
    if (need_ai_core && !have_rt) argv_add(&args, "-lm");

    perf_start(&g_diag.perf, "Linking", STAGE_LINK);
    if (!g_config.compact_output) debug_phase_start("LINKING");
    if (!g_config.compact_output) CPX_INFO("Running GCC linker...");
    log_external_command(&args);
    int result = casprix_run_process(args.args, false);
    perf_end(&g_diag.perf);
    if (result != 0) {
        fprintf(stderr,
            "Casprix: external native link failed (tool exit %d); existing output was preserved.\n",
            result);
        remove(temp_path);
        cleanup_native_intermediates(asm_file_path, obj_file_path);
        return 74;
    }
    if (!casprix_publish_file(temp_path, final_path)) {
        fprintf(stderr, "Casprix: could not publish completed executable '%s' (errno %d).\n",
                final_path, errno);
        remove(temp_path);
        cleanup_native_intermediates(asm_file_path, obj_file_path);
        return 74;
    }
#ifdef _WIN32
    if (!g_config.compact_output) CPX_INFO("Executable created: %s", final_path);
#else
    if (!g_config.compact_output) CPX_INFO("Executable created: %s", final_path);
#endif
    if (!g_config.compact_output) debug_phase_end("Linking");

    if (!g_config.keep_asm_file) remove(asm_file_path);
    remove(obj_file_path);
    return 0;
}
