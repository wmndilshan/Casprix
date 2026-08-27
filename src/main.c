/**
 * main.c — Casprix Compiler entry point
 *
 * This file is intentionally minimal. All logic lives in:
 *   src/driver/cli.c     — argument parsing & configuration
 *   src/driver/io.c      — file I/O and path utilities
 *   src/driver/pipeline.c — compilation, assembly, linking
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "driver/cli.h"
#include "driver/io.h"
#include "driver/pipeline.h"
#include "support/log.h"
#include "support/diagnostic.h"

int main(int argc, char* argv[]) {
    const char* input_file = NULL;

    /* 1. Parse command-line arguments */
    int cli_result = cli_parse_args(argc, argv, &input_file);
    if (cli_result != 0) {
        /* Non-zero means early exit (--help) or parse error */
        return (cli_result == 1) ? 0 : cli_result;
    }

    /* 2. Initialise logging */
    CpxLogLevel log_level = CPX_LOG_INFO;
    if (g_config.verbose) log_level = CPX_LOG_DEBUG;
    else if (g_config.compact_output) log_level = CPX_LOG_OFF;

    cpx_log_init(log_level);

    /* 3. Initialise diagnostic engine */
    diag_engine_init(&g_diag);
    diag_engine_set_format(&g_diag, g_config.diag_format);
    diag_engine_set_colors(&g_diag, !g_config.diag_no_color);
    diag_engine_set_max_errors(&g_diag, g_config.max_errors);
    g_diag.min_severity  = g_config.min_severity;
    g_diag.perf.enabled  = g_config.perf_trace || g_config.compact_output;

    if (!input_file) {
        fprintf(stderr, "Error: No input file specified.\n");
        fprintf(stderr, "Use -h or --help for usage information.\n");
        cpx_log_shutdown();
        return 64;
    }

    /* Warn on unexpected extension */
    size_t len = strlen(input_file);
    if ((len < 4 || strcmp(input_file + len - 4, ".cpx") != 0) &&
        (len < 3 || strcmp(input_file + len - 3, ".nd")  != 0)) {
        fprintf(stderr, "Warning: Input file should have a .cpx extension\n");
    }

    /* 4. Resolve output paths */
    char* output_name      = driver_get_output_name(input_file);
    char* output_file_path = driver_get_output_file_path(output_name);
    char* obj_file_path    = (char*)malloc(strlen(output_name) + 5);
    sprintf(obj_file_path, "%s.o", output_name);

    /* 5. Compile → intermediate artifact (.asm / .c / .cpxbc …) */
    int result = pipeline_compile(input_file, output_file_path);
    if (result != 0 || g_config.parse_only || g_config.check_only ||
        g_config.execute) {
        /* --execute is terminal: pipeline_compile ran the program in-process
         * and `result` is already main()'s exit code — no assemble/link. */
        goto done;
    }

    /* MIR-based backends handle their own output — skip NASM + GCC */
    if (g_config.output_kind != OUTPUT_NATIVE) {
        if (!g_config.compact_output) {
            printf("\n================================================================================\n");
            printf("  COMPILATION SUCCESSFUL (%s)\n", driver_selected_output_name());
            printf("================================================================================\n\n");
        }
        goto print_diag;
    }

    /* 6. Assemble → object file */
    result = pipeline_assemble(output_file_path, obj_file_path);
    if (result != 0) goto done;

    /* 7. Link → executable */
    result = pipeline_link(obj_file_path, output_file_path, output_name);
    if (result == 0 && !g_config.compact_output) {
        printf("\n================================================================================\n");
        printf("  COMPILATION SUCCESSFUL\n");
        printf("================================================================================\n\n");
    }

print_diag:
    if (g_config.compact_output && result == 0) {
        perf_print_compact(&g_diag.perf, stdout, input_file, !g_config.diag_no_color);
    } else if (g_config.perf_trace) {
        if (g_config.diag_format == DIAG_FMT_JSON)
            perf_print_json(&g_diag.perf, stdout);
        else
            perf_print_report(&g_diag.perf, stdout, !g_config.diag_no_color);
    }
    diag_engine_print_summary(&g_diag, stderr);

done:
    free(output_name);
    free(output_file_path);
    free(obj_file_path);
    diag_engine_destroy(&g_diag);
    cpx_log_shutdown();
    return result;
}
