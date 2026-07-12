/*
 * Casprix Compiler — MIR Optimization Passes
 *
 * SSA-based optimization pipeline operating on MIR.
 * Pass ordering follows standard compiler optimization theory:
 *
 *   1. Constant propagation + folding (SCCP-lite)
 *   2. Copy propagation
 *   3. Dead code elimination
 *   4. Strength reduction
 *   5. Control flow simplification
 *   6. ARC elision (retain/release pair elimination)
 *
 * Each pass is idempotent and can be run multiple times.
 * The pipeline iterates until a fixed point (no changes).
 */

#ifndef MIR_OPT_H
#define MIR_OPT_H

#include "mir.h"

/* Optimization statistics */
typedef struct {
    int constants_folded;
    int copies_propagated;
    int dead_insts_eliminated;
    int branches_simplified;
    int strength_reductions;
    int arc_pairs_elided;
    int blocks_merged;
    int total_passes;
} MirOptStats;

/* Optimization level */
typedef enum {
    MIR_OPT_NONE = 0,    /* -O0: no optimization */
    MIR_OPT_BASIC = 1,   /* -O1: const fold + DCE */
    MIR_OPT_STANDARD = 2,/* -O2: full pipeline */
    MIR_OPT_AGGRESSIVE = 3 /* -O3: + aggressive inlining */
} MirOptLevel;

/* Run optimization pipeline on a module. */
void mir_optimize_module(MirModule* module, MirOptLevel level, MirOptStats* stats);

/* Run optimization pipeline on a single function. */
void mir_optimize_function(MirFunction* func, MirOptLevel level, MirOptStats* stats);

/* Individual passes (can be called standalone) */
int mir_pass_constant_fold(MirFunction* func);
int mir_pass_copy_propagate(MirFunction* func);
int mir_pass_dead_code_eliminate(MirFunction* func);
int mir_pass_strength_reduce(MirFunction* func);
int mir_pass_simplify_cfg(MirFunction* func);
int mir_pass_arc_elision(MirFunction* func);

/* Async transformation pass */
int mir_transform_async(MirFunction* func);

#endif /* MIR_OPT_H */
