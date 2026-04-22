/*
 * Casprix Compiler — MIR Optimization Passes Implementation
 *
 * This file implements the core optimization pipeline for the MIR.
 * All passes operate on the SSA-form MIR in-place.
 *
 * Pass ordering rationale:
 *   - Constant folding first: creates dead code.
 *   - Copy propagation: simplifies value chains.
 *   - DCE: removes instructions with unused results.
 *   - Strength reduction: replaces expensive ops (mul→shl, div→shr).
 *   - CFG simplification: merges blocks, removes unreachable code.
 *   - ARC elision: removes redundant retain/release pairs.
 *
 * Fixed-point iteration: the pipeline repeats until no pass makes changes.
 * Typical convergence: 2-3 iterations for well-structured code.
 */

#include "mir_opt.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* ================================================================
 * Helper: find instruction that defines a value
 * ================================================================ */

static MirInst* find_def(MirFunction* func, MirValueId val) {
    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            if (inst->result == val) return inst;
        }
    }
    return NULL;
}

/* ================================================================
 * Helper: count uses of a value
 * ================================================================ */

bool value_used_in_inst(MirInst* inst, MirValueId val) {
    switch (inst->opcode) {
        case MIR_ADD: case MIR_SUB: case MIR_MUL: case MIR_DIV: case MIR_MOD:
        case MIR_FADD: case MIR_FSUB: case MIR_FMUL: case MIR_FDIV:
        case MIR_BAND: case MIR_BOR: case MIR_BXOR: case MIR_SHL: case MIR_SHR:
        case MIR_USHR:
        case MIR_CMP_EQ: case MIR_CMP_NE: case MIR_CMP_LT: case MIR_CMP_LE:
        case MIR_CMP_GT: case MIR_CMP_GE:
        case MIR_LOGIC_AND: case MIR_LOGIC_OR:
            return inst->as.binary.lhs == val || inst->as.binary.rhs == val;

        case MIR_NEG: case MIR_FNEG: case MIR_BNOT: case MIR_LOGIC_NOT:
        case MIR_CAST: case MIR_BITCAST: case MIR_TRUNC:
        case MIR_ZEXT: case MIR_SEXT: case MIR_SITOFP: case MIR_FPTOSI:
            return inst->as.unary.operand == val;

        case MIR_LOAD:
            return inst->as.mem.ptr == val;
        case MIR_STORE:
            return inst->as.mem.ptr == val || inst->as.mem.value == val;
        case MIR_GET_FIELD_PTR:
        case MIR_GET_ELEM_PTR:
            return inst->as.gep.base == val || inst->as.gep.index == val;

        case MIR_CONDBR:
            return inst->as.condbr.cond == val;
        case MIR_RET:
            return inst->as.ret.value == val;
        case MIR_SWITCH:
            return inst->as.sw.discriminant == val;

        case MIR_CALL:
        case MIR_CALL_INDIRECT:
            if (inst->as.call.callee == val) return true;
            for (int i = 0; i < inst->as.call.n_args; i++)
                if (inst->as.call.args[i] == val) return true;
            return false;

        case MIR_CALL_VIRTUAL:
            if (inst->as.vcall.self_obj == val) return true;
            for (int i = 0; i < inst->as.vcall.n_args; i++)
                if (inst->as.vcall.args[i] == val) return true;
            return false;

        case MIR_PHI:
            for (int i = 0; i < inst->as.phi.n_edges; i++)
                if (inst->as.phi.edges[i].value == val) return true;
            return false;

        case MIR_COPY: case MIR_BORROW: case MIR_BORROW_MUT: case MIR_MOVE:
            return inst->as.transfer.source == val;

        case MIR_ARC_RETAIN: case MIR_ARC_RELEASE: case MIR_DROP:
            return inst->as.refop.ptr == val;

        case MIR_STRUCT_INIT:
            for (int i = 0; i < inst->as.struct_init.n_fields; i++)
                if (inst->as.struct_init.fields[i] == val) return true;
            return false;

        case MIR_EXTRACT:
            return inst->as.field_op.aggregate == val;
        case MIR_INSERT:
            return inst->as.field_op.aggregate == val ||
                   inst->as.field_op.insert_val == val;

        /* Generic vector (all share the `vec` variant with up to 3
         * SSA sources; MIR_VALUE_NONE indicates an unused slot). */
        case MIR_VEC_LOAD: case MIR_VEC_LOAD_UNALIGNED:
        case MIR_VEC_STORE: case MIR_VEC_STORE_UNALIGNED:
        case MIR_VEC_BROADCAST: case MIR_VEC_REDUCE_SUM:
        case MIR_VEC_ADD: case MIR_VEC_SUB: case MIR_VEC_MUL: case MIR_VEC_DIV:
        case MIR_VEC_MIN: case MIR_VEC_MAX:
        case MIR_VEC_AND: case MIR_VEC_OR:  case MIR_VEC_XOR:
        case MIR_VEC_FMA: case MIR_VEC_DOT:
        case MIR_VEC_CMP_EQ: case MIR_VEC_CMP_LT: case MIR_VEC_CMP_GT:
        case MIR_VEC_SELECT:
            return inst->as.vec.a == val || inst->as.vec.b == val ||
                   inst->as.vec.c == val;

        default:
            return false;
    }
}

static int count_uses(MirFunction* func, MirValueId val) {
    int count = 0;
    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            if (value_used_in_inst(inst, val)) count++;
        }
    }
    return count;
}

/* ================================================================
 * Helper: replace all uses of a value
 * ================================================================ */

static void replace_value_in_inst(MirInst* inst, MirValueId old_val, MirValueId new_val) {
    switch (inst->opcode) {
        case MIR_ADD: case MIR_SUB: case MIR_MUL: case MIR_DIV: case MIR_MOD:
        case MIR_FADD: case MIR_FSUB: case MIR_FMUL: case MIR_FDIV:
        case MIR_BAND: case MIR_BOR: case MIR_BXOR: case MIR_SHL: case MIR_SHR:
        case MIR_USHR:
        case MIR_CMP_EQ: case MIR_CMP_NE: case MIR_CMP_LT: case MIR_CMP_LE:
        case MIR_CMP_GT: case MIR_CMP_GE:
        case MIR_LOGIC_AND: case MIR_LOGIC_OR:
            if (inst->as.binary.lhs == old_val) inst->as.binary.lhs = new_val;
            if (inst->as.binary.rhs == old_val) inst->as.binary.rhs = new_val;
            break;

        case MIR_NEG: case MIR_FNEG: case MIR_BNOT: case MIR_LOGIC_NOT:
        case MIR_CAST: case MIR_BITCAST: case MIR_TRUNC:
        case MIR_ZEXT: case MIR_SEXT: case MIR_SITOFP: case MIR_FPTOSI:
            if (inst->as.unary.operand == old_val) inst->as.unary.operand = new_val;
            break;

        case MIR_LOAD:
            if (inst->as.mem.ptr == old_val) inst->as.mem.ptr = new_val;
            break;
        case MIR_STORE:
            if (inst->as.mem.ptr == old_val) inst->as.mem.ptr = new_val;
            if (inst->as.mem.value == old_val) inst->as.mem.value = new_val;
            break;
        case MIR_GET_FIELD_PTR:
        case MIR_GET_ELEM_PTR:
            if (inst->as.gep.base == old_val) inst->as.gep.base = new_val;
            if (inst->as.gep.index == old_val) inst->as.gep.index = new_val;
            break;

        case MIR_CONDBR:
            if (inst->as.condbr.cond == old_val) inst->as.condbr.cond = new_val;
            break;
        case MIR_RET:
            if (inst->as.ret.value == old_val) inst->as.ret.value = new_val;
            break;

        case MIR_CALL:
        case MIR_CALL_INDIRECT:
            if (inst->as.call.callee == old_val) inst->as.call.callee = new_val;
            for (int i = 0; i < inst->as.call.n_args; i++)
                if (inst->as.call.args[i] == old_val) inst->as.call.args[i] = new_val;
            break;

        case MIR_PHI:
            for (int i = 0; i < inst->as.phi.n_edges; i++)
                if (inst->as.phi.edges[i].value == old_val) inst->as.phi.edges[i].value = new_val;
            break;

        case MIR_COPY: case MIR_BORROW: case MIR_BORROW_MUT: case MIR_MOVE:
            if (inst->as.transfer.source == old_val) inst->as.transfer.source = new_val;
            break;

        case MIR_ARC_RETAIN: case MIR_ARC_RELEASE: case MIR_DROP:
            if (inst->as.refop.ptr == old_val) inst->as.refop.ptr = new_val;
            break;

        /* Generic vector (up to 3 SSA sources). */
        case MIR_VEC_LOAD: case MIR_VEC_LOAD_UNALIGNED:
        case MIR_VEC_STORE: case MIR_VEC_STORE_UNALIGNED:
        case MIR_VEC_BROADCAST: case MIR_VEC_REDUCE_SUM:
        case MIR_VEC_ADD: case MIR_VEC_SUB: case MIR_VEC_MUL: case MIR_VEC_DIV:
        case MIR_VEC_MIN: case MIR_VEC_MAX:
        case MIR_VEC_AND: case MIR_VEC_OR:  case MIR_VEC_XOR:
        case MIR_VEC_FMA: case MIR_VEC_DOT:
        case MIR_VEC_CMP_EQ: case MIR_VEC_CMP_LT: case MIR_VEC_CMP_GT:
        case MIR_VEC_SELECT:
            if (inst->as.vec.a == old_val) inst->as.vec.a = new_val;
            if (inst->as.vec.b == old_val) inst->as.vec.b = new_val;
            if (inst->as.vec.c == old_val) inst->as.vec.c = new_val;
            break;

        default:
            break;
    }
}

static void replace_all_uses(MirFunction* func, MirValueId old_val, MirValueId new_val) {
    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            replace_value_in_inst(inst, old_val, new_val);
        }
    }
}

/* ================================================================
 * Helper: remove instruction from its block
 * ================================================================ */

static void remove_inst(MirBlock* block, MirInst* inst) {
    if (inst->prev) inst->prev->next = inst->next;
    else block->first = inst->next;

    if (inst->next) inst->next->prev = inst->prev;
    else block->last = inst->prev;

    block->inst_count--;
}

/* ================================================================
 * Pass 1: Constant Folding (SCCP-lite)
 *
 * Evaluates arithmetic on constants at compile time.
 * Handles integer arithmetic, comparisons, and boolean logic.
 * ================================================================ */

int mir_pass_constant_fold(MirFunction* func) {
    int changes = 0;

    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            /* Integer binary constant folding */
            if (inst->opcode >= MIR_ADD && inst->opcode <= MIR_MOD) {
                MirInst* lhs_def = find_def(func, inst->as.binary.lhs);
                MirInst* rhs_def = find_def(func, inst->as.binary.rhs);
                if (lhs_def && lhs_def->opcode == MIR_CONST_INT &&
                    rhs_def && rhs_def->opcode == MIR_CONST_INT) {
                    int64_t l = lhs_def->as.imm_i64;
                    int64_t r = rhs_def->as.imm_i64;
                    int64_t result = 0;
                    bool valid = true;
                    switch (inst->opcode) {
                        case MIR_ADD: result = l + r; break;
                        case MIR_SUB: result = l - r; break;
                        case MIR_MUL: result = l * r; break;
                        case MIR_DIV: if (r != 0) result = l / r; else valid = false; break;
                        case MIR_MOD: if (r != 0) result = l % r; else valid = false; break;
                        default: valid = false;
                    }
                    if (valid) {
                        inst->opcode = MIR_CONST_INT;
                        inst->as.imm_i64 = result;
                        changes++;
                    }
                }
            }

            /* Comparison constant folding */
            if (inst->opcode >= MIR_CMP_EQ && inst->opcode <= MIR_CMP_GE) {
                MirInst* lhs_def = find_def(func, inst->as.binary.lhs);
                MirInst* rhs_def = find_def(func, inst->as.binary.rhs);
                if (lhs_def && lhs_def->opcode == MIR_CONST_INT &&
                    rhs_def && rhs_def->opcode == MIR_CONST_INT) {
                    int64_t l = lhs_def->as.imm_i64;
                    int64_t r = rhs_def->as.imm_i64;
                    bool result = false;
                    switch (inst->opcode) {
                        case MIR_CMP_EQ: result = (l == r); break;
                        case MIR_CMP_NE: result = (l != r); break;
                        case MIR_CMP_LT: result = (l < r);  break;
                        case MIR_CMP_LE: result = (l <= r); break;
                        case MIR_CMP_GT: result = (l > r);  break;
                        case MIR_CMP_GE: result = (l >= r); break;
                        default: break;
                    }
                    inst->opcode = MIR_CONST_BOOL;
                    inst->as.imm_bool = result;
                    changes++;
                }
            }

            /* Algebraic identities:
             *   x + 0 → x,  x * 1 → x,  x * 0 → 0,
             *   x - 0 → x,  x / 1 → x  */
            if (inst->opcode == MIR_ADD || inst->opcode == MIR_SUB) {
                MirInst* rhs_def = find_def(func, inst->as.binary.rhs);
                if (rhs_def && rhs_def->opcode == MIR_CONST_INT &&
                    rhs_def->as.imm_i64 == 0) {
                    /* Replace with copy of lhs */
                    replace_all_uses(func, inst->result, inst->as.binary.lhs);
                    inst->opcode = MIR_NOP;
                    changes++;
                }
            }
            if (inst->opcode == MIR_MUL) {
                MirInst* rhs_def = find_def(func, inst->as.binary.rhs);
                if (rhs_def && rhs_def->opcode == MIR_CONST_INT) {
                    if (rhs_def->as.imm_i64 == 1) {
                        replace_all_uses(func, inst->result, inst->as.binary.lhs);
                        inst->opcode = MIR_NOP;
                        changes++;
                    } else if (rhs_def->as.imm_i64 == 0) {
                        inst->opcode = MIR_CONST_INT;
                        inst->as.imm_i64 = 0;
                        changes++;
                    }
                }
            }
        }
    }
    return changes;
}

/* ================================================================
 * Pass 2: Copy Propagation
 *
 * If %a = copy %b, replace all uses of %a with %b.
 * Also handles phi nodes with all-same operands.
 * ================================================================ */

int mir_pass_copy_propagate(MirFunction* func) {
    int changes = 0;

    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            /* Trivial copy propagation */
            if (inst->opcode == MIR_COPY && inst->result != MIR_VALUE_NONE) {
                replace_all_uses(func, inst->result, inst->as.transfer.source);
                changes++;
            }

            /* Phi with all-same operands → copy */
            if (inst->opcode == MIR_PHI && inst->as.phi.n_edges > 0) {
                MirValueId first = inst->as.phi.edges[0].value;
                bool all_same = true;
                for (int i = 1; i < inst->as.phi.n_edges; i++) {
                    if (inst->as.phi.edges[i].value != first) {
                        all_same = false;
                        break;
                    }
                }
                if (all_same && first != inst->result) {
                    replace_all_uses(func, inst->result, first);
                    inst->opcode = MIR_NOP;
                    changes++;
                }
            }
        }
    }
    return changes;
}

/* ================================================================
 * Pass 3: Dead Code Elimination
 *
 * Removes instructions whose results are never used.
 * Preserves side-effecting instructions (stores, calls, branches,
 * ARC ops, drops).
 * ================================================================ */

static bool has_side_effects(MirOpcode op) {
    switch (op) {
        case MIR_STORE:
        case MIR_CALL: case MIR_CALL_INDIRECT: case MIR_CALL_VIRTUAL:
        case MIR_BR: case MIR_CONDBR: case MIR_SWITCH:
        case MIR_RET: case MIR_RET_VOID: case MIR_UNREACHABLE:
        case MIR_ARC_RETAIN: case MIR_ARC_RELEASE:
        case MIR_DROP: case MIR_OBJ_ALLOC:
        case MIR_DEBUGLOC:
            return true;
        default:
            return false;
    }
}

int mir_pass_dead_code_eliminate(MirFunction* func) {
    int changes = 0;

    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        MirInst* inst = bb->first;
        while (inst) {
            MirInst* next = inst->next;

            if (inst->opcode == MIR_NOP) {
                remove_inst(bb, inst);
                changes++;
            } else if (inst->result != MIR_VALUE_NONE &&
                       !has_side_effects(inst->opcode) &&
                       count_uses(func, inst->result) == 0) {
                remove_inst(bb, inst);
                changes++;
            }

            inst = next;
        }
    }
    return changes;
}

/* ================================================================
 * Pass 4: Strength Reduction
 *
 * Replaces expensive operations with cheaper equivalents:
 *   x * 2^n  →  x << n
 *   x / 2^n  →  x >> n  (unsigned)
 *   x % 2^n  →  x & (2^n - 1)
 * ================================================================ */

static bool is_power_of_two(int64_t v) {
    return v > 0 && (v & (v - 1)) == 0;
}

static int log2_int(int64_t v) {
    int n = 0;
    while (v > 1) { v >>= 1; n++; }
    return n;
}

/* Insert a new instruction immediately before `before` in `bb`. */
static void inst_insert_before(MirBlock* bb, MirInst* before, MirInst* inst) {
    inst->next = before;
    inst->prev = before->prev;
    if (before->prev)
        before->prev->next = inst;
    else
        bb->first = inst;
    before->prev = inst;
    bb->inst_count++;
}

int mir_pass_strength_reduce(MirFunction* func) {
    int changes = 0;
    MirArena* arena = func->parent->arena;

    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            if (inst->opcode == MIR_MUL) {
                MirInst* rhs_def = find_def(func, inst->as.binary.rhs);
                if (rhs_def && rhs_def->opcode == MIR_CONST_INT &&
                    is_power_of_two(rhs_def->as.imm_i64)) {
                    int shift = log2_int(rhs_def->as.imm_i64);
                    /* Emit a fresh constant for the shift amount so we don't
                     * corrupt other uses of rhs_def. */
                    MirInst* sc = (MirInst*)mir_arena_alloc(arena, sizeof(MirInst));
                    memset(sc, 0, sizeof(MirInst));
                    sc->opcode     = MIR_CONST_INT;
                    sc->type       = rhs_def->type;
                    sc->result     = mir_function_new_value(func, rhs_def->type);
                    sc->as.imm_i64 = shift;
                    inst_insert_before(bb, inst, sc);
                    inst->opcode = MIR_SHL;
                    inst->as.binary.rhs = sc->result;
                    changes++;
                }
            }

            if (inst->opcode == MIR_MOD) {
                MirInst* rhs_def = find_def(func, inst->as.binary.rhs);
                if (rhs_def && rhs_def->opcode == MIR_CONST_INT &&
                    is_power_of_two(rhs_def->as.imm_i64)) {
                    /* x % 2^n → x & (2^n - 1): emit fresh mask constant. */
                    int64_t mask = rhs_def->as.imm_i64 - 1;
                    MirInst* mc = (MirInst*)mir_arena_alloc(arena, sizeof(MirInst));
                    memset(mc, 0, sizeof(MirInst));
                    mc->opcode     = MIR_CONST_INT;
                    mc->type       = rhs_def->type;
                    mc->result     = mir_function_new_value(func, rhs_def->type);
                    mc->as.imm_i64 = mask;
                    inst_insert_before(bb, inst, mc);
                    inst->opcode = MIR_BAND;
                    inst->as.binary.rhs = mc->result;
                    changes++;
                }
            }
        }
    }
    return changes;
}

/* ================================================================
 * Pass 5: Control Flow Simplification
 *
 * - Conditional branch on constant → unconditional branch.
 * - Remove empty blocks (just a br to another block).
 * - Merge blocks with single predecessor/successor.
 * ================================================================ */

/* Remove `dead` from `live`'s predecessor list and patch its phis. */
static void cfg_remove_pred(MirBlock* live, MirBlock* dead) {
    /* Remove dead from live->predecessors. */
    for (int i = 0; i < live->pred_count; i++) {
        if (live->predecessors[i] == dead) {
            live->predecessors[i] = live->predecessors[live->pred_count - 1];
            live->pred_count--;
            break;
        }
    }
    /* Patch phi nodes: drop the edge that came from dead. */
    for (MirInst* phi = live->first;
         phi && phi->opcode == MIR_PHI;
         phi = phi->next) {
        for (int i = 0; i < phi->as.phi.n_edges; i++) {
            if (phi->as.phi.edges[i].block == dead) {
                /* Compact by shifting tail left. */
                phi->as.phi.edges[i] =
                    phi->as.phi.edges[phi->as.phi.n_edges - 1];
                phi->as.phi.n_edges--;
                break;
            }
        }
    }
}

/* Remove `succ` from `pred`'s successor list. */
static void cfg_remove_succ(MirBlock* pred, MirBlock* succ) {
    for (int i = 0; i < pred->succ_count; i++) {
        if (pred->successors[i] == succ) {
            pred->successors[i] = pred->successors[pred->succ_count - 1];
            pred->succ_count--;
            return;
        }
    }
}

int mir_pass_simplify_cfg(MirFunction* func) {
    int changes = 0;

    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        if (!bb->last) continue;

        /* condbr on constant → br; fully repair CFG and phi edges. */
        if (bb->last->opcode == MIR_CONDBR) {
            MirInst* cond_def = find_def(func, bb->last->as.condbr.cond);
            if (cond_def && cond_def->opcode == MIR_CONST_BOOL) {
                MirBlock* target   = cond_def->as.imm_bool
                                     ? bb->last->as.condbr.true_bb
                                     : bb->last->as.condbr.false_bb;
                MirBlock* dead_bb  = cond_def->as.imm_bool
                                     ? bb->last->as.condbr.false_bb
                                     : bb->last->as.condbr.true_bb;
                /* Rewrite terminator. */
                bb->last->opcode       = MIR_BR;
                bb->last->as.br.target = target;
                /* Repair CFG: remove the edge to the dead successor. */
                cfg_remove_succ(bb, dead_bb);
                cfg_remove_pred(dead_bb, bb);
                changes++;
            }
        }

        /* Trivial forwarder block (only a br): thread predecessors through.
         * Skip entry block and blocks with phis in the target (phi requires
         * stable pred lists — merging them would need phi-edge rewriting
         * which we defer to a separate CFG canonicalise pass). */
        if (bb->first && bb->first == bb->last &&
            bb->first->opcode == MIR_BR &&
            bb != func->entry_block) {
            MirBlock* target = bb->first->as.br.target;
            /* Only thread if target has no phis (safe heuristic). */
            bool target_has_phi = (target->first &&
                                   target->first->opcode == MIR_PHI);
            if (!target_has_phi) {
                for (int i = 0; i < bb->pred_count; i++) {
                    MirBlock* pred = bb->predecessors[i];
                    /* Redirect pred's terminator from bb → target. */
                    for (MirInst* t = pred->last; t; t = t->prev) {
                        if (t->opcode == MIR_BR && t->as.br.target == bb)
                            t->as.br.target = target;
                        else if (t->opcode == MIR_CONDBR) {
                            if (t->as.condbr.true_bb  == bb)
                                t->as.condbr.true_bb  = target;
                            if (t->as.condbr.false_bb == bb)
                                t->as.condbr.false_bb = target;
                        }
                    }
                    /* Update succ/pred lists. */
                    cfg_remove_succ(pred, bb);
                    if (target != bb) {
                        mir_block_add_successor(pred, target);
                        mir_block_add_predecessor(target, pred);
                    }
                }
                cfg_remove_pred(target, bb);
                bb->pred_count = 0;
                changes++;
            }
        }
    }
    return changes;
}

/* ================================================================
 * Pass 6: ARC Elision
 *
 * Eliminates redundant retain/release pairs.
 * Pattern: arc.retain %x followed by arc.release %x in the same
 * block with no intervening use → remove both.
 * ================================================================ */

int mir_pass_arc_elision(MirFunction* func) {
    int changes = 0;

    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* retain = bb->first; retain; retain = retain->next) {
            if (retain->opcode != MIR_ARC_RETAIN) continue;
            MirValueId ptr = retain->as.refop.ptr;

            /* Search forward for a matching release */
            for (MirInst* release = retain->next; release; release = release->next) {
                if (release->opcode == MIR_ARC_RELEASE &&
                    release->as.refop.ptr == ptr) {
                    /* Check no intervening use of ptr between retain and release */
                    bool intervening_use = false;
                    for (MirInst* mid = retain->next; mid != release; mid = mid->next) {
                        if (value_used_in_inst(mid, ptr)) {
                            intervening_use = true;
                            break;
                        }
                    }
                    if (!intervening_use) {
                        retain->opcode = MIR_NOP;
                        release->opcode = MIR_NOP;
                        changes++;
                    }
                    break;
                }
                /* If ptr is used, stop searching (it may escape) */
                if (value_used_in_inst(release, ptr)) break;
            }
        }
    }
    return changes;
}

/* ================================================================
 * Pipeline: run all passes to fixed point
 * ================================================================ */

void mir_optimize_function(MirFunction* func, MirOptLevel level, MirOptStats* stats) {
    if (level == MIR_OPT_NONE) return;
    if (func->is_extern) return;

    mir_assert_ssa_valid(func, "pre-optimize");
    mir_assert_ownership_valid(func, "pre-optimize");

    int max_iterations = 10;
    for (int iter = 0; iter < max_iterations; iter++) {
        int total_changes = 0;

        /* Phase 1: constant folding */
        {
            int c = mir_pass_constant_fold(func);
            if (stats) stats->constants_folded += c;
            total_changes += c;
            mir_assert_ssa_valid(func, "constant_fold");
            mir_assert_ownership_valid(func, "constant_fold");
        }

        /* Phase 2: copy propagation */
        if (level >= MIR_OPT_STANDARD) {
            int c = mir_pass_copy_propagate(func);
            if (stats) stats->copies_propagated += c;
            total_changes += c;
            mir_assert_ssa_valid(func, "copy_propagate");
            mir_assert_ownership_valid(func, "copy_propagate");
        }

        /* Phase 3: dead code elimination */
        {
            int c = mir_pass_dead_code_eliminate(func);
            if (stats) stats->dead_insts_eliminated += c;
            total_changes += c;
            mir_assert_ssa_valid(func, "dead_code_eliminate");
            mir_assert_ownership_valid(func, "dead_code_eliminate");
        }

        /* Phase 4: strength reduction */
        if (level >= MIR_OPT_STANDARD) {
            int c = mir_pass_strength_reduce(func);
            if (stats) stats->strength_reductions += c;
            total_changes += c;
            mir_assert_ssa_valid(func, "strength_reduce");
            mir_assert_ownership_valid(func, "strength_reduce");
        }

        /* Phase 5: CFG simplification */
        if (level >= MIR_OPT_STANDARD) {
            int c = mir_pass_simplify_cfg(func);
            if (stats) stats->branches_simplified += c;
            total_changes += c;
            mir_assert_ssa_valid(func, "simplify_cfg");
            mir_assert_ownership_valid(func, "simplify_cfg");
        }

        /* Phase 6: ARC elision */
        if (level >= MIR_OPT_BASIC) {
            int c = mir_pass_arc_elision(func);
            if (stats) stats->arc_pairs_elided += c;
            total_changes += c;
            mir_assert_ssa_valid(func, "arc_elision");
            mir_assert_ownership_valid(func, "arc_elision");
        }

        if (stats) stats->total_passes++;
        if (total_changes == 0) break; /* Fixed point reached */
    }
}

void mir_optimize_module(MirModule* module, MirOptLevel level, MirOptStats* stats) {
    if (stats) memset(stats, 0, sizeof(MirOptStats));
    for (MirFunction* f = module->func_list; f; f = f->next_func) {
        mir_optimize_function(f, level, stats);
    }
}
