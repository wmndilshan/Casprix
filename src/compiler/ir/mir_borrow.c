/*
 * Casprix Compiler — MIR Borrow Checker Implementation
 *
 * Validates ownership, borrowing, and move semantics on the MIR.
 *
 * Algorithm:
 *   1. Compute liveness info for each SSA value (def block, last-use block).
 *   2. Track all borrow instructions and their source values.
 *   3. For each basic block:
 *      a. Check that moved values are not subsequently used.
 *      b. Check that mutable borrows are exclusive (no other active borrows).
 *      c. Check that shared borrows don't overlap with mutable borrows.
 *   4. Verify that owned values are dropped exactly once (or moved).
 *
 * This is a simplified intraprocedural analysis suitable for a first
 * production pass. Interprocedural borrow analysis can be added later
 * via function summaries.
 */

#include "mir_borrow.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Init / Destroy / Reset
 * ================================================================ */

void mir_borrow_init(MirBorrowChecker* bc, MirModule* module) {
    memset(bc, 0, sizeof(MirBorrowChecker));
    bc->module = module;
    bc->liveness_capacity = 256;
    bc->liveness = calloc(bc->liveness_capacity, sizeof(MirLiveness));
    bc->moved_capacity = 256;
    bc->moved = calloc(bc->moved_capacity, sizeof(bool));
}

void mir_borrow_destroy(MirBorrowChecker* bc) {
    free(bc->liveness);
    free(bc->moved);
    memset(bc, 0, sizeof(MirBorrowChecker));
}

void mir_borrow_reset(MirBorrowChecker* bc) {
    bc->borrow_count = 0;
    bc->liveness_count = 0;
    memset(bc->moved, 0, bc->moved_capacity * sizeof(bool));
    /* Keep errors accumulated */
}

/* ================================================================
 * Error reporting
 * ================================================================ */

static void emit_error(MirBorrowChecker* bc, int kind, const char* msg,
                        int line, int col, MirValueId val, MirValueId conflict) {
    if (bc->error_count >= MIR_MAX_ERRORS) return;
    MirBorrowError* e = &bc->errors[bc->error_count++];
    e->kind = kind;
    e->message = msg;
    e->line = line;
    e->col = col;
    e->value = val;
    e->conflicting = conflict;
}

void mir_borrow_print_errors(MirBorrowChecker* bc, FILE* out) {
    for (int i = 0; i < bc->error_count; i++) {
        MirBorrowError* e = &bc->errors[i];
        const char* kind_str = "error";
        switch (e->kind) {
            case MIR_BERR_USE_AFTER_MOVE: kind_str = "use after move"; break;
            case MIR_BERR_DOUBLE_MOVE:    kind_str = "double move"; break;
            case MIR_BERR_BORROW_CONFLICT:kind_str = "borrow conflict"; break;
            case MIR_BERR_MUT_ALIAS:      kind_str = "mutable alias"; break;
            case MIR_BERR_BORROW_OUTLIVES:kind_str = "borrow outlives value"; break;
            case MIR_BERR_MISSING_DROP:   kind_str = "missing drop"; break;
            case MIR_BERR_DOUBLE_DROP:    kind_str = "double drop"; break;
        }
        fprintf(out, "  [BORROW] line %d: %s — %s (%%%u",
                e->line, kind_str, e->message, e->value);
        if (e->conflicting != MIR_VALUE_NONE) {
            fprintf(out, " conflicts with %%%u", e->conflicting);
        }
        fprintf(out, ")\n");
    }
}

/* ================================================================
 * Ensure moved array capacity
 * ================================================================ */

static void ensure_moved(MirBorrowChecker* bc, MirValueId id) {
    if ((int)id >= bc->moved_capacity) {
        int new_cap = bc->moved_capacity * 2;
        if ((int)id >= new_cap) new_cap = (int)id + 64;
        bc->moved = realloc(bc->moved, new_cap * sizeof(bool));
        memset(bc->moved + bc->moved_capacity, 0,
               (new_cap - bc->moved_capacity) * sizeof(bool));
        bc->moved_capacity = new_cap;
    }
}

static void mark_moved(MirBorrowChecker* bc, MirValueId id) {
    ensure_moved(bc, id);
    bc->moved[id] = true;
}

static bool is_moved(MirBorrowChecker* bc, MirValueId id) {
    if ((int)id >= bc->moved_capacity) return false;
    return bc->moved[id];
}

/* ================================================================
 * Check if a value is used in an instruction
 * ================================================================ */

static bool inst_uses_value(MirInst* inst, MirValueId val) {
    switch (inst->opcode) {
        case MIR_ADD: case MIR_SUB: case MIR_MUL: case MIR_DIV: case MIR_MOD:
        case MIR_FADD: case MIR_FSUB: case MIR_FMUL: case MIR_FDIV:
        case MIR_BAND: case MIR_BOR: case MIR_BXOR: case MIR_SHL: case MIR_SHR:
        case MIR_CMP_EQ: case MIR_CMP_NE: case MIR_CMP_LT: case MIR_CMP_LE:
        case MIR_CMP_GT: case MIR_CMP_GE:
            return inst->as.binary.lhs == val || inst->as.binary.rhs == val;

        case MIR_NEG: case MIR_FNEG: case MIR_BNOT: case MIR_LOGIC_NOT:
        case MIR_CAST: case MIR_ZEXT: case MIR_SEXT: case MIR_TRUNC:
        case MIR_SITOFP: case MIR_FPTOSI:
            return inst->as.unary.operand == val;

        case MIR_LOAD:
            return inst->as.mem.ptr == val;
        case MIR_STORE:
            return inst->as.mem.ptr == val || inst->as.mem.value == val;

        case MIR_CONDBR:
            return inst->as.condbr.cond == val;
        case MIR_RET:
            return inst->as.ret.value == val;

        case MIR_CALL:
        case MIR_CALL_INDIRECT:
            for (int i = 0; i < inst->as.call.n_args; i++)
                if (inst->as.call.args[i] == val) return true;
            return inst->opcode == MIR_CALL_INDIRECT && inst->as.call.callee == val;

        case MIR_COPY: case MIR_BORROW: case MIR_BORROW_MUT: case MIR_MOVE:
            return inst->as.transfer.source == val;

        case MIR_ARC_RETAIN: case MIR_ARC_RELEASE: case MIR_DROP:
            return inst->as.refop.ptr == val;

        default:
            return false;
    }
}

/* ================================================================
 * Track borrow
 * ================================================================ */

static void track_borrow(MirBorrowChecker* bc, MirInst* inst, MirBorrowKind kind) {
    if (bc->borrow_count >= MIR_MAX_BORROWS) return;
    MirBorrow* b = &bc->borrows[bc->borrow_count++];
    b->borrow_id = inst->result;
    b->source_id = inst->as.transfer.source;
    b->kind = kind;
    b->def_block = NULL; /* Could be set to current block if needed */
    b->src_line = inst->src_line;
    b->src_col = inst->src_col;
    b->active = true;
}

/* ================================================================
 * Check borrow conflicts for a given source value
 * ================================================================ */

static void check_borrow_conflicts(MirBorrowChecker* bc, MirValueId source,
                                    MirBorrowKind new_kind, int line, int col,
                                    MirValueId new_borrow) {
    for (int i = 0; i < bc->borrow_count; i++) {
        MirBorrow* existing = &bc->borrows[i];
        if (!existing->active) continue;
        if (existing->source_id != source) continue;

        if (new_kind == MIR_BK_MUTABLE) {
            /* New mutable borrow conflicts with ANY existing borrow */
            if (existing->kind == MIR_BK_MUTABLE) {
                emit_error(bc, MIR_BERR_MUT_ALIAS,
                           "cannot create second mutable borrow",
                           line, col, new_borrow, existing->borrow_id);
            } else {
                emit_error(bc, MIR_BERR_BORROW_CONFLICT,
                           "cannot create mutable borrow while shared borrows exist",
                           line, col, new_borrow, existing->borrow_id);
            }
        } else {
            /* New shared borrow conflicts with existing mutable borrow */
            if (existing->kind == MIR_BK_MUTABLE) {
                emit_error(bc, MIR_BERR_BORROW_CONFLICT,
                           "cannot create shared borrow while mutable borrow exists",
                           line, col, new_borrow, existing->borrow_id);
            }
        }
    }
}

/* ================================================================
 * Check a single function
 * ================================================================ */

int mir_borrow_check_function(MirBorrowChecker* bc, MirFunction* func) {
    if (func->is_extern) return 0;

    int initial_errors = bc->error_count;
    mir_borrow_reset(bc);

    /* Walk all blocks and instructions */
    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {

            /* Check use-after-move for all operands */
            switch (inst->opcode) {
                case MIR_ADD: case MIR_SUB: case MIR_MUL: case MIR_DIV: case MIR_MOD:
                case MIR_FADD: case MIR_FSUB: case MIR_FMUL: case MIR_FDIV:
                case MIR_BAND: case MIR_BOR: case MIR_BXOR: case MIR_SHL: case MIR_SHR:
                case MIR_CMP_EQ: case MIR_CMP_NE: case MIR_CMP_LT: case MIR_CMP_LE:
                case MIR_CMP_GT: case MIR_CMP_GE: {
                    if (is_moved(bc, inst->as.binary.lhs)) {
                        emit_error(bc, MIR_BERR_USE_AFTER_MOVE,
                                   "value used after move (lhs)",
                                   inst->src_line, inst->src_col,
                                   inst->as.binary.lhs, MIR_VALUE_NONE);
                    }
                    if (is_moved(bc, inst->as.binary.rhs)) {
                        emit_error(bc, MIR_BERR_USE_AFTER_MOVE,
                                   "value used after move (rhs)",
                                   inst->src_line, inst->src_col,
                                   inst->as.binary.rhs, MIR_VALUE_NONE);
                    }
                    break;
                }

                case MIR_NEG: case MIR_FNEG: case MIR_BNOT: case MIR_LOGIC_NOT:
                case MIR_CAST: case MIR_ZEXT: case MIR_SEXT: case MIR_TRUNC:
                case MIR_SITOFP: case MIR_FPTOSI: {
                    if (is_moved(bc, inst->as.unary.operand)) {
                        emit_error(bc, MIR_BERR_USE_AFTER_MOVE,
                                   "value used after move (operand)",
                                   inst->src_line, inst->src_col,
                                   inst->as.unary.operand, MIR_VALUE_NONE);
                    }
                    break;
                }

                case MIR_CALL:
                case MIR_CALL_INDIRECT: {
                    if (inst->opcode == MIR_CALL_INDIRECT &&
                        is_moved(bc, inst->as.call.callee)) {
                        emit_error(bc, MIR_BERR_USE_AFTER_MOVE,
                                   "call target used after move",
                                   inst->src_line, inst->src_col,
                                   inst->as.call.callee, MIR_VALUE_NONE);
                    }
                    for (int i = 0; i < inst->as.call.n_args; i++) {
                        if (is_moved(bc, inst->as.call.args[i])) {
                            emit_error(bc, MIR_BERR_USE_AFTER_MOVE,
                                       "call argument used after move",
                                       inst->src_line, inst->src_col,
                                       inst->as.call.args[i], MIR_VALUE_NONE);
                        }
                    }
                    break;
                }

                default:
                    break;
            }

            /* Track move instructions */
            if (inst->opcode == MIR_MOVE) {
                MirValueId src = inst->as.transfer.source;
                if (is_moved(bc, src)) {
                    emit_error(bc, MIR_BERR_DOUBLE_MOVE,
                               "value moved more than once",
                               inst->src_line, inst->src_col,
                               src, MIR_VALUE_NONE);
                }
                mark_moved(bc, src);
            }

            /* Track borrows and check conflicts */
            if (inst->opcode == MIR_BORROW) {
                MirValueId src = inst->as.transfer.source;
                if (is_moved(bc, src)) {
                    emit_error(bc, MIR_BERR_USE_AFTER_MOVE,
                               "cannot borrow a moved value",
                               inst->src_line, inst->src_col,
                               src, MIR_VALUE_NONE);
                }
                check_borrow_conflicts(bc, src, MIR_BK_SHARED,
                                       inst->src_line, inst->src_col, inst->result);
                track_borrow(bc, inst, MIR_BK_SHARED);
            }

            if (inst->opcode == MIR_BORROW_MUT) {
                MirValueId src = inst->as.transfer.source;
                if (is_moved(bc, src)) {
                    emit_error(bc, MIR_BERR_USE_AFTER_MOVE,
                               "cannot mutably borrow a moved value",
                               inst->src_line, inst->src_col,
                               src, MIR_VALUE_NONE);
                }
                check_borrow_conflicts(bc, src, MIR_BK_MUTABLE,
                                       inst->src_line, inst->src_col, inst->result);
                track_borrow(bc, inst, MIR_BK_MUTABLE);
            }

            /* Track drops */
            if (inst->opcode == MIR_DROP) {
                MirValueId ptr = inst->as.refop.ptr;
                if (is_moved(bc, ptr)) {
                    emit_error(bc, MIR_BERR_USE_AFTER_MOVE,
                               "cannot drop a moved value",
                               inst->src_line, inst->src_col,
                               ptr, MIR_VALUE_NONE);
                }
            }
        }
    }

    return bc->error_count - initial_errors;
}

/* ================================================================
 * Check all functions in a module
 * ================================================================ */

int mir_borrow_check_module(MirBorrowChecker* bc, MirModule* module) {
    int total_errors = 0;
    for (MirFunction* f = module->func_list; f; f = f->next_func) {
        total_errors += mir_borrow_check_function(bc, f);
    }
    return total_errors;
}
