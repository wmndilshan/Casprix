/**
 * cli.h - Command-line interface for the Casprix compiler driver
 *
 * Declares the CompilerConfig struct and cli_parse_args().
 */
#ifndef CASPRIX_DRIVER_CLI_H
#define CASPRIX_DRIVER_CLI_H

#include <stdbool.h>
#include "support/diagnostic.h"

/* Output backend kinds */
typedef enum {
    OUTPUT_NATIVE = 0,
    OUTPUT_VM,
    OUTPUT_JIT,
    OUTPUT_C,
} CompilerOutputKind;

/* Native code-gen path */
typedef enum {
    NATIVE_CODEGEN_LEGACY_AST = 0,
    NATIVE_CODEGEN_MIR,
} NativeCodegenKind;

/* All compiler configuration knobs, populated by cli_parse_args(). */
typedef struct {
    /* Output options */
    bool            verbose;
    bool            keep_asm_file;
    bool            optimize;
    const char*     output_name;

    /* Debug / academic options */
    bool            dump_tokens;
    bool            dump_ast;
    bool            dump_symbols;
    bool            dump_mir;
    bool            dump_all;
    bool            step_mode;
    bool            parse_only;
    bool            check_only;

    /* Backend selection */
    bool                 use_mir;
    CompilerOutputKind   output_kind;
    NativeCodegenKind    native_codegen;
    bool                 allow_legacy_backend_fallback;
    int                  opt_level;
    bool                 safe_mode;
    bool                 size_opt;

    /* Diagnostic options */
    DiagFormat      diag_format;
    int             max_errors;
    bool            diag_no_color;
    bool            perf_trace;
    bool            compact_output;
    DiagSeverity    min_severity;
} CompilerConfig;

/** Global compiler configuration (defined in cli.c). */
extern CompilerConfig g_config;

/**
 * Parse argv and fill g_config.
 *
 * @param argc  Argument count from main().
 * @param argv  Argument vector from main().
 * @param[out] input_file  Set to the positional source file argument.
 * @return 0 on success, non-zero on error or early exit (e.g. --help).
 */
int cli_parse_args(int argc, char* argv[], const char** input_file);

/** Print the help / usage banner. */
void cli_print_usage(const char* program_name);

#endif /* CASPRIX_DRIVER_CLI_H */
