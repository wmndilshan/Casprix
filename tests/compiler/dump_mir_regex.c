/*
 * dump_mir_regex.c — developer aid. Compiles a handful of regexes into
 * MIR and prints both the raw and post-O2 MIR to stdout so humans can
 * inspect that the DFA → MIR lowering matches the design described in
 * `mir_regex.h`. Not wired into CTest — used as a smoke tool.
 */
#include <stdio.h>
#include <string.h>

#include "compiler/ir/mir.h"
#include "compiler/ir/mir_opt.h"
#include "compiler/ir/mir_regex.h"

int main(int argc, char** argv) {
    const char* pattern = argc > 1 ? argv[1] : "cat|dog";
    MirModule* m = mir_module_create("dump");
    MirRegexResult r = mir_regex_compile(m, "dump_fn", pattern);
    if (!r.ok) {
        fprintf(stderr, "compile failed: %s (pos %d)\n", r.error, r.error_pos);
        return 1;
    }
    printf("/* Pattern: %s */\n", pattern);
    printf("/* DFA states: %d (accepting: %d), NFA states: %d */\n\n",
           r.dfa_states, r.accepting_states, r.nfa_states);

    printf("--- MIR before optimisation ---\n");
    mir_print_function(r.func, stdout);

    MirOptStats s; memset(&s, 0, sizeof s);
    mir_optimize_function(r.func, MIR_OPT_STANDARD, &s);

    printf("\n--- MIR after optimisation (O2) ---\n");
    mir_print_function(r.func, stdout);

    printf("\n/* opt stats: folded=%d copies=%d dead=%d branches=%d */\n",
           s.constants_folded, s.copies_propagated,
           s.dead_insts_eliminated, s.branches_simplified);
    mir_module_destroy(m);
    return 0;
}
