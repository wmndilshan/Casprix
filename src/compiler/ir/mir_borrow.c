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

/* ================================================================
 * Check borrow conflicts for a given source value
 * ================================================================ */

static bool inst_uses_value(MirInst* inst, MirValueId val) {
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
            if (inst->opcode == MIR_CALL_INDIRECT && inst->as.call.callee == val)
                return true;
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
            return inst->as.field_op.aggregate == val || inst->as.field_op.insert_val == val;

        case MIR_SUSPEND:
            return inst->as.suspend.future == val;

        case MIR_VEC_LOAD: case MIR_VEC_LOAD_UNALIGNED:
        case MIR_VEC_STORE: case MIR_VEC_STORE_UNALIGNED:
        case MIR_VEC_BROADCAST: case MIR_VEC_REDUCE_SUM:
        case MIR_VEC_ADD: case MIR_VEC_SUB: case MIR_VEC_MUL: case MIR_VEC_DIV:
        case MIR_VEC_MIN: case MIR_VEC_MAX:
        case MIR_VEC_AND: case MIR_VEC_OR:  case MIR_VEC_XOR:
        case MIR_VEC_FMA: case MIR_VEC_DOT:
        case MIR_VEC_CMP_EQ: case MIR_VEC_CMP_LT: case MIR_VEC_CMP_GT:
        case MIR_VEC_SELECT:
            return inst->as.vec.a == val || inst->as.vec.b == val || inst->as.vec.c == val;

        default:
            return false;
    }
}

static bool is_live_after(MirBlock* bb, MirInst* start_inst, MirValueId val, bool* live_out_bb, int num_vals) {
    if (val == MIR_VALUE_NONE || val >= (MirValueId)num_vals) return false;
    for (MirInst* inst = start_inst->next; inst; inst = inst->next) {
        if (inst_uses_value(inst, val)) return true;
    }
    return live_out_bb[val];
}

static void mark_uses(MirInst* inst, bool* def_bb, bool* use_bb, int num_vals) {
    #define RECORD_USE(id) \
        if ((id) != MIR_VALUE_NONE && (id) < (MirValueId)num_vals) { \
            if (!def_bb[(id)]) use_bb[(id)] = true; \
        }

    switch (inst->opcode) {
        case MIR_ADD: case MIR_SUB: case MIR_MUL: case MIR_DIV: case MIR_MOD:
        case MIR_FADD: case MIR_FSUB: case MIR_FMUL: case MIR_FDIV:
        case MIR_BAND: case MIR_BOR: case MIR_BXOR: case MIR_SHL: case MIR_SHR:
        case MIR_USHR:
        case MIR_CMP_EQ: case MIR_CMP_NE: case MIR_CMP_LT: case MIR_CMP_LE:
        case MIR_CMP_GT: case MIR_CMP_GE:
        case MIR_LOGIC_AND: case MIR_LOGIC_OR:
            RECORD_USE(inst->as.binary.lhs);
            RECORD_USE(inst->as.binary.rhs);
            break;

        case MIR_NEG: case MIR_FNEG: case MIR_BNOT: case MIR_LOGIC_NOT:
        case MIR_CAST: case MIR_BITCAST: case MIR_TRUNC:
        case MIR_ZEXT: case MIR_SEXT: case MIR_SITOFP: case MIR_FPTOSI:
            RECORD_USE(inst->as.unary.operand);
            break;

        case MIR_LOAD:
            RECORD_USE(inst->as.mem.ptr);
            break;
        case MIR_STORE:
            RECORD_USE(inst->as.mem.ptr);
            RECORD_USE(inst->as.mem.value);
            break;
        case MIR_GET_FIELD_PTR:
        case MIR_GET_ELEM_PTR:
            RECORD_USE(inst->as.gep.base);
            RECORD_USE(inst->as.gep.index);
            break;

        case MIR_CONDBR:
            RECORD_USE(inst->as.condbr.cond);
            break;
        case MIR_RET:
            RECORD_USE(inst->as.ret.value);
            break;
        case MIR_SWITCH:
            RECORD_USE(inst->as.sw.discriminant);
            break;

        case MIR_CALL:
        case MIR_CALL_INDIRECT:
            if (inst->opcode == MIR_CALL_INDIRECT)
                RECORD_USE(inst->as.call.callee);
            for (int i = 0; i < inst->as.call.n_args; i++)
                RECORD_USE(inst->as.call.args[i]);
            break;

        case MIR_CALL_VIRTUAL:
            RECORD_USE(inst->as.vcall.self_obj);
            for (int i = 0; i < inst->as.vcall.n_args; i++)
                RECORD_USE(inst->as.vcall.args[i]);
            break;

        case MIR_PHI:
            for (int i = 0; i < inst->as.phi.n_edges; i++)
                RECORD_USE(inst->as.phi.edges[i].value);
            break;

        case MIR_COPY: case MIR_BORROW: case MIR_BORROW_MUT: case MIR_MOVE:
            RECORD_USE(inst->as.transfer.source);
            break;

        case MIR_ARC_RETAIN: case MIR_ARC_RELEASE: case MIR_DROP:
            RECORD_USE(inst->as.refop.ptr);
            break;

        case MIR_STRUCT_INIT:
            for (int i = 0; i < inst->as.struct_init.n_fields; i++)
                RECORD_USE(inst->as.struct_init.fields[i]);
            break;

        case MIR_EXTRACT:
            RECORD_USE(inst->as.field_op.aggregate);
            break;
        case MIR_INSERT:
            RECORD_USE(inst->as.field_op.aggregate);
            RECORD_USE(inst->as.field_op.insert_val);
            break;

        case MIR_SUSPEND:
            RECORD_USE(inst->as.suspend.future);
            break;

        case MIR_VEC_LOAD: case MIR_VEC_LOAD_UNALIGNED:
        case MIR_VEC_STORE: case MIR_VEC_STORE_UNALIGNED:
        case MIR_VEC_BROADCAST: case MIR_VEC_REDUCE_SUM:
        case MIR_VEC_ADD: case MIR_VEC_SUB: case MIR_VEC_MUL: case MIR_VEC_DIV:
        case MIR_VEC_MIN: case MIR_VEC_MAX:
        case MIR_VEC_AND: case MIR_VEC_OR:  case MIR_VEC_XOR:
        case MIR_VEC_FMA: case MIR_VEC_DOT:
        case MIR_VEC_CMP_EQ: case MIR_VEC_CMP_LT: case MIR_VEC_CMP_GT:
        case MIR_VEC_SELECT:
            RECORD_USE(inst->as.vec.a);
            RECORD_USE(inst->as.vec.b);
            RECORD_USE(inst->as.vec.c);
            break;

        default:
            break;
    }
    #undef RECORD_USE
}

static void check_borrow_conflicts(MirBorrowChecker* bc, MirValueId source,
                                    MirBorrowKind new_kind, int line, int col,
                                    MirValueId new_borrow, MirBlock* bb, MirInst* inst,
                                    bool** live_out, int num_vals) {
    for (int i = 0; i < bc->borrow_count; i++) {
        MirBorrow* existing = &bc->borrows[i];
        if (existing->borrow_id == new_borrow) continue;

        bool active = (existing->borrow_id == inst->result) ||
                      inst_uses_value(inst, existing->borrow_id) ||
                      is_live_after(bb, inst, existing->borrow_id, live_out[bb->id], num_vals);

        if (!active) continue;
        if (existing->source_id != source) continue;

        if (new_kind == MIR_BK_MUTABLE) {
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

static void borrow_operand_moved(MirBorrowChecker* bc, MirInst* inst,
                                  MirValueId id, const char* ctx) {
    if (id == MIR_VALUE_NONE) return;
    if (is_moved(bc, id)) {
        emit_error(bc, MIR_BERR_USE_AFTER_MOVE, ctx,
                   inst->src_line, inst->src_col, id, MIR_VALUE_NONE);
    }
}

static void borrow_check_inst_operands(MirBorrowChecker* bc, MirInst* inst) {
    if (inst->opcode == MIR_MOVE) return;

    switch (inst->opcode) {
        case MIR_ADD: case MIR_SUB: case MIR_MUL: case MIR_DIV: case MIR_MOD:
        case MIR_FADD: case MIR_FSUB: case MIR_FMUL: case MIR_FDIV:
        case MIR_BAND: case MIR_BOR: case MIR_BXOR: case MIR_SHL: case MIR_SHR:
        case MIR_USHR:
        case MIR_CMP_EQ: case MIR_CMP_NE: case MIR_CMP_LT: case MIR_CMP_LE:
        case MIR_CMP_GT: case MIR_CMP_GE:
        case MIR_LOGIC_AND: case MIR_LOGIC_OR:
            borrow_operand_moved(bc, inst, inst->as.binary.lhs, "value used after move (lhs)");
            borrow_operand_moved(bc, inst, inst->as.binary.rhs, "value used after move (rhs)");
            break;

        case MIR_NEG: case MIR_FNEG: case MIR_BNOT: case MIR_LOGIC_NOT:
        case MIR_CAST: case MIR_BITCAST: case MIR_TRUNC:
        case MIR_ZEXT: case MIR_SEXT: case MIR_SITOFP: case MIR_FPTOSI:
            borrow_operand_moved(bc, inst, inst->as.unary.operand, "value used after move (operand)");
            break;

        case MIR_LOAD:
            borrow_operand_moved(bc, inst, inst->as.mem.ptr, "value used after move (load ptr)");
            break;
        case MIR_STORE:
            borrow_operand_moved(bc, inst, inst->as.mem.ptr, "value used after move (store ptr)");
            borrow_operand_moved(bc, inst, inst->as.mem.value, "value used after move (store val)");
            break;
        case MIR_GET_FIELD_PTR:
        case MIR_GET_ELEM_PTR:
            borrow_operand_moved(bc, inst, inst->as.gep.base, "value used after move (gep base)");
            borrow_operand_moved(bc, inst, inst->as.gep.index, "value used after move (gep index)");
            break;

        case MIR_CONDBR:
            borrow_operand_moved(bc, inst, inst->as.condbr.cond, "value used after move (cond)");
            break;
        case MIR_RET:
            borrow_operand_moved(bc, inst, inst->as.ret.value, "value used after move (ret)");
            break;
        case MIR_SWITCH:
            borrow_operand_moved(bc, inst, inst->as.sw.discriminant, "value used after move (switch)");
            break;

        case MIR_CALL:
        case MIR_CALL_INDIRECT:
            if (inst->opcode == MIR_CALL_INDIRECT)
                borrow_operand_moved(bc, inst, inst->as.call.callee, "call target used after move");
            for (int i = 0; i < inst->as.call.n_args; i++)
                borrow_operand_moved(bc, inst, inst->as.call.args[i], "call argument used after move");
            break;

        case MIR_CALL_VIRTUAL:
            borrow_operand_moved(bc, inst, inst->as.vcall.self_obj, "vcall self used after move");
            for (int i = 0; i < inst->as.vcall.n_args; i++)
                borrow_operand_moved(bc, inst, inst->as.vcall.args[i], "vcall arg used after move");
            break;

        case MIR_PHI:
            for (int i = 0; i < inst->as.phi.n_edges; i++)
                borrow_operand_moved(bc, inst, inst->as.phi.edges[i].value, "phi operand used after move");
            break;

        case MIR_COPY: case MIR_BORROW: case MIR_BORROW_MUT:
            borrow_operand_moved(bc, inst, inst->as.transfer.source, "transfer source used after move");
            break;

        case MIR_ARC_RETAIN: case MIR_ARC_RELEASE: case MIR_DROP:
            borrow_operand_moved(bc, inst, inst->as.refop.ptr, "ref op ptr used after move");
            break;

        case MIR_STRUCT_INIT:
            for (int i = 0; i < inst->as.struct_init.n_fields; i++)
                borrow_operand_moved(bc, inst, inst->as.struct_init.fields[i], "struct field used after move");
            break;

        case MIR_EXTRACT:
            borrow_operand_moved(bc, inst, inst->as.field_op.aggregate, "extract agg used after move");
            break;
        case MIR_INSERT:
            borrow_operand_moved(bc, inst, inst->as.field_op.aggregate, "insert agg used after move");
            borrow_operand_moved(bc, inst, inst->as.field_op.insert_val, "insert val used after move");
            break;

        case MIR_SUSPEND:
            borrow_operand_moved(bc, inst, inst->as.suspend.future, "suspend future used after move");
            break;

        case MIR_VEC_LOAD: case MIR_VEC_LOAD_UNALIGNED:
        case MIR_VEC_STORE: case MIR_VEC_STORE_UNALIGNED:
        case MIR_VEC_BROADCAST: case MIR_VEC_REDUCE_SUM:
        case MIR_VEC_ADD: case MIR_VEC_SUB: case MIR_VEC_MUL: case MIR_VEC_DIV:
        case MIR_VEC_MIN: case MIR_VEC_MAX:
        case MIR_VEC_AND: case MIR_VEC_OR:  case MIR_VEC_XOR:
        case MIR_VEC_FMA: case MIR_VEC_DOT:
        case MIR_VEC_CMP_EQ: case MIR_VEC_CMP_LT: case MIR_VEC_CMP_GT:
        case MIR_VEC_SELECT:
            borrow_operand_moved(bc, inst, inst->as.vec.a, "vec operand a used after move");
            borrow_operand_moved(bc, inst, inst->as.vec.b, "vec operand b used after move");
            borrow_operand_moved(bc, inst, inst->as.vec.c, "vec operand c used after move");
            break;

        default:
            break;
    }
}

int mir_borrow_check_function(MirBorrowChecker* bc, MirFunction* func) {
    if (func->is_extern) return 0;
    if (!func->block_list) return 0;

    int initial_errors = bc->error_count;
    mir_borrow_reset(bc);

    int num_vals = func->next_value_id;

    // Find max block ID to allocate arrays
    uint32_t max_block_id = 0;
    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        if (bb->id > max_block_id) max_block_id = bb->id;
    }
    uint32_t num_block_ids = max_block_id + 1;

    // Allocate move analysis arrays
    bool** block_out = calloc(num_block_ids, sizeof(bool*));
    for (uint32_t i = 0; i < num_block_ids; i++) {
        block_out[i] = calloc(num_vals, sizeof(bool));
    }

    // Worklist queue
    MirBlock** worklist = malloc(num_block_ids * sizeof(MirBlock*));
    bool* in_worklist = calloc(num_block_ids, sizeof(bool));
    int wl_head = 0;
    int wl_tail = 0;
    int wl_count = 0;

    #define PUSH_WL(bb) \
        if (!in_worklist[(bb)->id]) { \
            worklist[wl_tail] = (bb); \
            wl_tail = (wl_tail + 1) % num_block_ids; \
            in_worklist[(bb)->id] = true; \
            wl_count++; \
        }

    #define POP_WL() \
        MirBlock* popped = worklist[wl_head]; \
        wl_head = (wl_head + 1) % num_block_ids; \
        in_worklist[popped->id] = false; \
        wl_count--;

    // Forward move analysis fixed-point iteration
    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        PUSH_WL(bb);
    }

    bool* temp_state = malloc(num_vals * sizeof(bool));

    while (wl_count > 0) {
        POP_WL();
        MirBlock* bb = popped;

        // InState = Union_{p in predecessors} Out[p]
        memset(temp_state, 0, num_vals * sizeof(bool));
        for (int p = 0; p < bb->pred_count; p++) {
            MirBlock* pred = bb->predecessors[p];
            for (int v = 0; v < num_vals; v++) {
                if (block_out[pred->id][v]) {
                    temp_state[v] = true;
                }
            }
        }

        // Traverse instructions to compute new Out state
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            if (inst->opcode == MIR_MOVE) {
                MirValueId src = inst->as.transfer.source;
                if (src != MIR_VALUE_NONE && src < (MirValueId)num_vals) {
                    temp_state[src] = true;
                }
            }
        }

        // Check if Out state changed
        bool changed = false;
        for (int v = 0; v < num_vals; v++) {
            if (block_out[bb->id][v] != temp_state[v]) {
                block_out[bb->id][v] = temp_state[v];
                changed = true;
            }
        }

        if (changed) {
            for (int s = 0; s < bb->succ_count; s++) {
                PUSH_WL(bb->successors[s]);
            }
        }
    }

    // Allocate liveness arrays
    bool** def_block = calloc(num_block_ids, sizeof(bool*));
    bool** use_block = calloc(num_block_ids, sizeof(bool*));
    bool** live_in = calloc(num_block_ids, sizeof(bool*));
    bool** live_out = calloc(num_block_ids, sizeof(bool*));
    for (uint32_t i = 0; i < num_block_ids; i++) {
        def_block[i] = calloc(num_vals, sizeof(bool));
        use_block[i] = calloc(num_vals, sizeof(bool));
        live_in[i] = calloc(num_vals, sizeof(bool));
        live_out[i] = calloc(num_vals, sizeof(bool));
    }

    // Compute local Def and Use sets
    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            mark_uses(inst, def_block[bb->id], use_block[bb->id], num_vals);
            if (inst->result != MIR_VALUE_NONE && inst->result < (MirValueId)num_vals) {
                def_block[bb->id][inst->result] = true;
            }
        }
    }

    // Backward worklist
    memset(in_worklist, 0, num_block_ids * sizeof(bool));
    wl_head = 0;
    wl_tail = 0;
    wl_count = 0;

    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        PUSH_WL(bb);
    }

    while (wl_count > 0) {
        POP_WL();
        MirBlock* bb = popped;

        // LiveOut[bb] = Union_{s in successors} LiveIn[s]
        memset(temp_state, 0, num_vals * sizeof(bool));
        for (int s = 0; s < bb->succ_count; s++) {
            MirBlock* succ = bb->successors[s];
            for (int v = 0; v < num_vals; v++) {
                if (live_in[succ->id][v]) {
                    temp_state[v] = true;
                }
            }
        }
        memcpy(live_out[bb->id], temp_state, num_vals * sizeof(bool));

        // LiveIn[bb] = (LiveOut[bb] - Def[bb]) Union Use[bb]
        bool changed = false;
        for (int v = 0; v < num_vals; v++) {
            bool new_in = (live_out[bb->id][v] && !def_block[bb->id][v]) || use_block[bb->id][v];
            if (live_in[bb->id][v] != new_in) {
                live_in[bb->id][v] = new_in;
                changed = true;
            }
        }

        if (changed) {
            for (int p = 0; p < bb->pred_count; p++) {
                PUSH_WL(bb->predecessors[p]);
            }
        }
    }

    #undef PUSH_WL
    #undef POP_WL

    // Step 1: Pre-populate all borrows in the function
    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            if (inst->opcode == MIR_BORROW) {
                track_borrow(bc, inst, MIR_BK_SHARED);
            } else if (inst->opcode == MIR_BORROW_MUT) {
                track_borrow(bc, inst, MIR_BK_MUTABLE);
            }
        }
    }

    // Step 2: Final validation pass
    bool* original_moved = bc->moved;
    int original_moved_capacity = bc->moved_capacity;

    bc->moved = temp_state;
    bc->moved_capacity = num_vals;

    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        // Compute InState of move analysis at entry of bb
        memset(temp_state, 0, num_vals * sizeof(bool));
        for (int p = 0; p < bb->pred_count; p++) {
            MirBlock* pred = bb->predecessors[p];
            for (int v = 0; v < num_vals; v++) {
                if (block_out[pred->id][v]) {
                    temp_state[v] = true;
                }
            }
        }

        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            borrow_check_inst_operands(bc, inst);

            /* Track move instructions */
            if (inst->opcode == MIR_MOVE) {
                MirValueId src = inst->as.transfer.source;
                if (src != MIR_VALUE_NONE && src < (MirValueId)num_vals) {
                    if (is_moved(bc, src)) {
                        emit_error(bc, MIR_BERR_DOUBLE_MOVE,
                                   "value moved more than once",
                                   inst->src_line, inst->src_col,
                                   src, MIR_VALUE_NONE);
                    }
                    mark_moved(bc, src);
                }
            }

            /* Check borrow conflicts */
            if (inst->opcode == MIR_BORROW) {
                MirValueId src = inst->as.transfer.source;
                check_borrow_conflicts(bc, src, MIR_BK_SHARED,
                                       inst->src_line, inst->src_col, inst->result,
                                       bb, inst, live_out, num_vals);
            }

            if (inst->opcode == MIR_BORROW_MUT) {
                MirValueId src = inst->as.transfer.source;
                check_borrow_conflicts(bc, src, MIR_BK_MUTABLE,
                                       inst->src_line, inst->src_col, inst->result,
                                       bb, inst, live_out, num_vals);
            }
        }
    }

    bc->moved = original_moved;
    bc->moved_capacity = original_moved_capacity;

    // Clean up
    for (uint32_t i = 0; i < num_block_ids; i++) {
        free(block_out[i]);
        free(def_block[i]);
        free(use_block[i]);
        free(live_in[i]);
        free(live_out[i]);
    }
    free(block_out);
    free(def_block);
    free(use_block);
    free(live_in);
    free(live_out);
    free(worklist);
    free(in_worklist);
    free(temp_state);

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
