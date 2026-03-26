/*
 * Casprix Compiler — MIR mem2reg Pass
 *
 * Promotes stack-allocated variables (alloca + load/store) to proper SSA
 * form with phi nodes.  This is the canonical "SSA construction" pass that
 * removes the pre-SSA alloca/load/store pattern emitted by the lowering
 * phase and replaces it with direct SSA value flow.
 *
 * Algorithm:
 *   1.  Identify promotable allocas (scalar type, no address-taken uses,
 *       only loaded/stored through direct ptr identity).
 *   2.  Compute dominance frontier for every basic block (Cytron et al.).
 *   3.  Insert phi nodes at iterated dominance frontier of each alloca's
 *       defining blocks (i.e. blocks that contain a store to the alloca).
 *   4.  Rename variables via dominator-tree DFS:
 *       – Stores  →  push new SSA value.
 *       – Loads   →  replace with current SSA value from stack.
 *       – Phi incoming edges filled from predecessor rename state.
 *   5.  Delete now-dead alloca / load / store instructions.
 *
 * Prerequisites:
 *   – CFG predecessor/successor lists must be built (done by builder).
 *   – No critical edge splitting required (phi rename handles it).
 *
 * Running this pass before other optimisations dramatically improves
 * their effectiveness because the IR is in true SSA form.
 */

#ifndef MIR_MEM2REG_H
#define MIR_MEM2REG_H

#include "mir.h"

/* Statistics for the mem2reg pass. */
typedef struct {
    int allocas_promoted;       /* number of alloca instructions removed */
    int loads_eliminated;       /* load instructions replaced by SSA values */
    int stores_eliminated;      /* store instructions removed */
    int phis_inserted;          /* phi nodes inserted */
} MirMem2RegStats;

/* Run mem2reg on a single function.  Returns the number of allocas promoted. */
int mir_mem2reg(MirFunction* func, MirMem2RegStats* stats);

/* Run mem2reg on all functions in a module. */
void mir_mem2reg_module(MirModule* module, MirMem2RegStats* stats);

/* Compute dominance tree for a function.  Fills idom, dom_children,
 * dom_frontier fields on every MirBlock.  Uses the Cooper–Harvey–Kennedy
 * iterative algorithm (simple, fast for typical CFGs). */
void mir_compute_dominators(MirFunction* func);

/* Compute dominance frontiers from the dominator tree. */
void mir_compute_dom_frontiers(MirFunction* func);

#endif /* MIR_MEM2REG_H */
