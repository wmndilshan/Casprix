/**
 * cli.c - Command-line argument parsing for the Casprix compiler driver
 */
#include "driver/cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Global configuration instance (zero-initialised, then set by cli_parse_args)
 * ========================================================================== */

CompilerConfig g_config = {
    .verbose                      = false,
    .keep_asm_file                = false,
    .optimize                     = true,
    .output_name                  = NULL,
    .dump_tokens                  = false,
    .dump_ast                     = false,
    .dump_symbols                 = false,
    .dump_mir                     = false,
    .dump_all                     = false,
    .step_mode                    = false,
    .parse_only                   = false,
    .check_only                   = false,
    .use_mir                      = false,
    .output_kind                  = OUTPUT_NATIVE,
    .native_codegen               = NATIVE_CODEGEN_LEGACY_AST,
    .allow_legacy_backend_fallback= false,
    .opt_level                    = 2,
    .safe_mode                    = false,
    .size_opt                     = false,
    .execute                      = false,
    .diag_format                  = DIAG_FMT_HUMAN,
    .max_errors                   = 0,
    .diag_no_color                = false,
    .perf_trace                   = false,
    .compact_output               = true,
    .min_severity                 = DIAG_ERROR,
};

/* ============================================================================
 * Usage
 * ========================================================================== */

void cli_print_usage(const char* program_name) {
    printf("Casprix \xF0\x9F\x91\xBB - High Performance Ghost Language\n");
    printf("Version 1.0.0 | Generates x86-64 assembly from .cpx source\n\n");

    printf("Usage: %s [options] <input.cpx>\n\n", program_name);

    printf("Compilation Options:\n");
    printf("  -o, --output <name>   Specify output executable name\n");
    printf("  -v, --verbose         Enable verbose output\n");
    printf("  -k, --keep-asm        Keep intermediate assembly file\n");
    printf("  -O0                   Disable optimizations\n");
    printf("  -h, --help            Show this help message\n");

    printf("\nDebug/Academic Options:\n");
    printf("  --dump-tokens         Display lexer output (token stream)\n");
    printf("  --dump-ast            Display abstract syntax tree\n");
    printf("  --dump-symbols        Display symbol table after analysis\n");
    printf("  --dump-mir            Display MIR after lowering\n");
    printf("  --dump-all            Display all intermediate representations\n");
    printf("  --step                Step through compilation phases\n");
    printf("  --parse-only          Stop after parsing (no codegen)\n");
    printf("  --check-only          Stop after semantic analysis\n");

    printf("\nBackend Options:\n");
    printf("  --mir                 Run the MIR middle-end before code generation\n");
    printf("  --native, --aot       Use the MIR native backend (experimental)\n");
    printf("  --legacy-backend      Fall back to AST codegen after MIR passes\n");
    printf("  --vm                  Emit VM bytecode via MIR backend\n");
    printf("  --jit                 Emit a JIT package via MIR backend\n");
    printf("  --emit-c              Emit C source via MIR backend\n");
    printf("  --execute             Run the program in-process via the CVM (implies --mir);\n");
    printf("                        main()'s return value becomes the process exit code\n");
    printf("  --opt-level=N         Optimization level 0-3 (default: 2)\n");
    printf("  --safe-mode           Enable borrow checking (implies --mir)\n");
    printf("  --size-opt            Optimize for code size\n");

    printf("\nDiagnostic Options:\n");
    printf("  --diag-format=FMT     Output format: human|json|minimal|verbose\n");
    printf("  --max-errors=N        Stop after N errors (0 = unlimited)\n");
    printf("  --diag-no-color       Disable ANSI color codes in diagnostics\n");
    printf("  --perf-trace          Show pass-level timing report\n");
    printf("  --compact             Show minimalist high-density summary (default)\n");
    printf("  --no-compact          Disable compact summary output\n");
    printf("  --Weverything         Enable all warnings (default: errors only)\n");

    printf("\nExamples:\n");
    printf("  %s hello.cpx                    Compile hello.cpx to hello.exe\n", program_name);
    printf("  %s -o myapp hello.cpx           Compile to myapp.exe\n", program_name);
    printf("  %s --dump-tokens hello.cpx      Show token stream\n", program_name);
    printf("  %s --dump-ast hello.cpx         Show AST structure\n", program_name);
    printf("  %s --dump-all --step hello.cpx  Full debug with stepping\n", program_name);
}

/* ============================================================================
 * Argument parsing
 * ========================================================================== */

int cli_parse_args(int argc, char* argv[], const char** input_file) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            cli_print_usage(argv[0]);
            return 1; /* early exit, not an error */
        }
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_config.verbose = true;
        }
        else if (strcmp(argv[i], "-k") == 0 || strcmp(argv[i], "--keep-asm") == 0) {
            g_config.keep_asm_file = true;
        }
        else if (strcmp(argv[i], "-O0") == 0) {
            g_config.optimize = false;
        }
        else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 < argc) {
                g_config.output_name = argv[++i];
            } else {
                fprintf(stderr, "Error: -o requires an output name.\n");
                return 64;
            }
        }
        /* Debug / academic */
        else if (strcmp(argv[i], "--dump-tokens")  == 0) { g_config.dump_tokens  = true; }
        else if (strcmp(argv[i], "--dump-ast")     == 0) { g_config.dump_ast     = true; }
        else if (strcmp(argv[i], "--dump-symbols") == 0) { g_config.dump_symbols = true; }
        else if (strcmp(argv[i], "--dump-all")     == 0) { g_config.dump_all     = true; }
        else if (strcmp(argv[i], "--step")         == 0) { g_config.step_mode    = true; }
        else if (strcmp(argv[i], "--parse-only")   == 0) { g_config.parse_only   = true; }
        else if (strcmp(argv[i], "--check-only")   == 0) { g_config.check_only   = true; }
        /* MIR / backend */
        else if (strcmp(argv[i], "--mir") == 0) {
            g_config.use_mir = true;
        }
        else if (strcmp(argv[i], "--dump-mir") == 0) {
            g_config.dump_mir = true;
            g_config.use_mir  = true;
        }
        else if (strcmp(argv[i], "--native") == 0 || strcmp(argv[i], "--aot") == 0) {
            g_config.output_kind    = OUTPUT_NATIVE;
            g_config.native_codegen = NATIVE_CODEGEN_MIR;
            g_config.use_mir        = true;
        }
        else if (strcmp(argv[i], "--legacy-backend") == 0) {
            g_config.allow_legacy_backend_fallback = true;
        }
        else if (strcmp(argv[i], "--vm") == 0) {
            g_config.output_kind = OUTPUT_VM;
            g_config.use_mir     = true;
        }
        else if (strcmp(argv[i], "--jit") == 0) {
            g_config.output_kind = OUTPUT_JIT;
            g_config.use_mir     = true;
        }
        else if (strcmp(argv[i], "--emit-c") == 0) {
            g_config.output_kind = OUTPUT_C;
            g_config.use_mir     = true;
        }
        else if (strcmp(argv[i], "--execute") == 0) {
            /* Run the program in-process via the CVM. Standalone (no --vm
             * needed) and terminal: the driver runs the program and exits
             * with main()'s return value, emitting no output artifact even
             * if combined with --vm/--jit. --vm/--jit *without* --execute
             * are unchanged (serialize-and-exit). */
            g_config.execute = true;
            g_config.use_mir = true;
        }
        else if (strncmp(argv[i], "--opt-level=", 12) == 0) {
            g_config.opt_level = atoi(argv[i] + 12);
            if (g_config.opt_level < 0) g_config.opt_level = 0;
            if (g_config.opt_level > 3) g_config.opt_level = 3;
        }
        else if (strcmp(argv[i], "--safe-mode") == 0) {
            g_config.safe_mode = true;
            g_config.use_mir   = true;
        }
        else if (strcmp(argv[i], "--size-opt") == 0) {
            g_config.size_opt = true;
        }
        /* Diagnostics */
        else if (strncmp(argv[i], "--diag-format=", 14) == 0) {
            const char* fmt = argv[i] + 14;
            if      (strcmp(fmt, "json")    == 0) g_config.diag_format = DIAG_FMT_JSON;
            else if (strcmp(fmt, "minimal") == 0) g_config.diag_format = DIAG_FMT_MINIMAL;
            else if (strcmp(fmt, "verbose") == 0) g_config.diag_format = DIAG_FMT_VERBOSE;
            else                                  g_config.diag_format = DIAG_FMT_HUMAN;
        }
        else if (strncmp(argv[i], "--max-errors=", 13) == 0) {
            g_config.max_errors = atoi(argv[i] + 13);
        }
        else if (strcmp(argv[i], "--diag-no-color") == 0) { g_config.diag_no_color = true; }
        else if (strcmp(argv[i], "--perf-trace")    == 0) { g_config.perf_trace    = true; }
        else if (strcmp(argv[i], "--compact")       == 0) { g_config.compact_output = true; }
        else if (strcmp(argv[i], "--no-compact")    == 0) { g_config.compact_output = false; }
        else if (strcmp(argv[i], "--Weverything")   == 0) { g_config.min_severity  = DIAG_DEBUG; }
        else if (argv[i][0] != '-') {
            *input_file = argv[i];
        }
        else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            fprintf(stderr, "Use -h or --help for usage information.\n");
            return 64;
        }
    }
    return 0;
}
