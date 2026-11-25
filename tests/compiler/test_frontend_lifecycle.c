#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/cli.h"
#include "driver/pipeline.h"
#include "support/diagnostic.h"
#include "support/error.h"

extern DiagEngine g_diag;

static int compile_once(const char* source_path, bool expect_success) {
    diag_engine_init(&g_diag);
    diag_engine_set_colors(&g_diag, false);
    diag_engine_set_output(&g_diag, stderr);

    memset(&g_config, 0, sizeof(g_config));
    g_config.check_only = true;
    g_config.compact_output = true;
    g_config.output_kind = OUTPUT_VM;
    g_config.max_errors = 20;
    g_config.min_severity = DIAG_DEBUG;
    reset_error_count();

    int result = pipeline_compile(source_path, NULL);
    bool passed = expect_success ? result == 0 : result != 0;

    diag_engine_destroy(&g_diag);
    return passed ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <valid-source>... <invalid-source>\n", argv[0]);
        return 2;
    }

    const int invalid_index = argc - 1;
    for (int i = 0; i < 25; ++i) {
        for (int source = 1; source < invalid_index; ++source) {
            if (compile_once(argv[source], true) != 0) {
                fprintf(stderr, "valid compile failed for %s in lifecycle iteration %d\n",
                        argv[source], i);
                return 1;
            }
        }
        if (compile_once(argv[invalid_index], false) != 0) {
            fprintf(stderr, "invalid compile unexpectedly succeeded in lifecycle iteration %d\n", i);
            return 1;
        }
    }
    return 0;
}
