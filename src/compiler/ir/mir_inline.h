/*
 * Casprix Compiler — MIR Inlining Pass
 *
 * Inlines small non-recursive functions at their call sites.
 * Operates on the SSA-form MIR after mem2reg.
 *
 * Inlining criteria:
 *   – Function body ≤ N instructions (threshold depends on -O level).
 *   – Not recursive (does not call itself).
 *   – Not variadic or extern.
 *   – Only direct calls (MIR_CALL with known callee).
 *
 * Inlining process:
 *   1. Clone the callee's basic blocks into the caller.
 *   2. Map callee parameters → call arguments.
 *   3. Remap all SSA values to fresh values in the caller.
 *   4. Replace MIR_RET in the clone with a branch to a merge block.
 *   5. Replace the MIR_CALL instruction with the cloned body.
 *
 * This pass is gated behind -O2 and above.  At -O3 the threshold
 * is raised significantly for aggressive inlining.
 */

#ifndef MIR_INLINE_H
#define MIR_INLINE_H

#include "mir.h"

/* Inlining statistics. */
typedef struct {
    int calls_inlined;
    int instructions_copied;
    int blocks_copied;
} MirInlineStats;

/* Inlining thresholds. */
typedef struct {
    int max_inst_count;     /* max instructions in callee to inline     */
    int max_block_count;    /* max blocks in callee to inline           */
    int max_call_depth;     /* max recursive inline depth               */
    bool inline_always;     /* inline even large functions marked inline */
} MirInlineConfig;

/* Default thresholds for each optimisation level. */
MirInlineConfig mir_inline_config_for_level(int opt_level);

/* Run inlining on a single function.  Returns number of calls inlined. */
int mir_inline_function(MirModule* module, MirFunction* caller,
                         MirInlineConfig config, MirInlineStats* stats);

/* Run inlining on all functions in a module. */
void mir_inline_module(MirModule* module, int opt_level, MirInlineStats* stats);

#endif /* MIR_INLINE_H */
