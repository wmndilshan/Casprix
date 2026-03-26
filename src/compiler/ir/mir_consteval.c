/*
 * Casprix Compiler — MIR Constant Evaluation Engine Implementation
 *
 * Interprets MIR instructions to evaluate constexpr functions
 * at compile time. Produces MirConstValue results that can be
 * folded back into the MIR as constants.
 */

#include "mir_consteval.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Utility constructors
 * ================================================================ */

MirConstValue mir_cval_int(int64_t v) {
    return (MirConstValue){ .kind = MIR_CVAL_INT, .type = NULL, .as.i64 = v };
}

MirConstValue mir_cval_float(double v) {
    return (MirConstValue){ .kind = MIR_CVAL_FLOAT, .type = NULL, .as.f64 = v };
}

MirConstValue mir_cval_bool(bool v) {
    return (MirConstValue){ .kind = MIR_CVAL_BOOL, .type = NULL, .as.b = v };
}

MirConstValue mir_cval_void(void) {
    return (MirConstValue){ .kind = MIR_CVAL_VOID, .type = NULL };
}

/* ================================================================
 * Init / Destroy
 * ================================================================ */

void mir_consteval_init(MirConstEval* ce, MirModule* module) {
    memset(ce, 0, sizeof(MirConstEval));
    ce->module = module;
    ce->max_steps = MIR_CONSTEVAL_MAX_STEPS;
    ce->max_call_depth = 32;
    ce->slot_capacity = 256;
    ce->slots = calloc(ce->slot_capacity, sizeof(MirConstValue));
    ce->slot_count = 0;
}

void mir_consteval_destroy(MirConstEval* ce) {
    free(ce->slots);
    memset(ce, 0, sizeof(MirConstEval));
}

/* ================================================================
 * Eligibility check
 * ================================================================ */

bool mir_consteval_eligible(MirFunction* func) {
    if (!func || func->is_extern) return false;

    /* Walk all instructions looking for disqualifying ops */
    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            switch (inst->opcode) {
                /* Heap / I/O / extern — not constexpr */
                case MIR_OBJ_ALLOC:
                case MIR_ARC_RETAIN:
                case MIR_ARC_RELEASE:
                case MIR_CALL_INDIRECT:
                case MIR_CALL_VIRTUAL:
                    return false;

                /* Direct calls: only constexpr-eligible callees allowed */
                case MIR_CALL: {
                    MirModule* mod = func->parent;
                    MirFunction* callee = mir_module_find_function(mod, inst->as.call.func_name);
                    if (!callee || (!callee->is_constexpr && !mir_consteval_eligible(callee))) {
                        return false;
                    }
                    break;
                }

                default:
                    break;
            }
        }
    }
    return true;
}

/* ================================================================
 * Slot management
 * ================================================================ */

static void ensure_slot(MirConstEval* ce, MirValueId id) {
    if ((int)id >= ce->slot_capacity) {
        int new_cap = ce->slot_capacity * 2;
        if ((int)id >= new_cap) new_cap = (int)id + 64;
        ce->slots = realloc(ce->slots, new_cap * sizeof(MirConstValue));
        memset(ce->slots + ce->slot_capacity, 0,
               (new_cap - ce->slot_capacity) * sizeof(MirConstValue));
        ce->slot_capacity = new_cap;
    }
}

static MirConstValue* get_slot(MirConstEval* ce, MirValueId id) {
    ensure_slot(ce, id);
    return &ce->slots[id];
}

static void set_slot(MirConstEval* ce, MirValueId id, MirConstValue val) {
    ensure_slot(ce, id);
    ce->slots[id] = val;
}

/* ================================================================
 * Interpret a single instruction
 * ================================================================ */

bool mir_consteval_step(MirConstEval* ce, MirInst* inst) {
    ce->steps++;
    if (ce->steps > ce->max_steps) {
        ce->error_msg = "consteval: step limit exceeded (possible infinite loop)";
        return false;
    }

    MirValueId r = inst->result;

    switch (inst->opcode) {
        case MIR_CONST_INT:
            set_slot(ce, r, mir_cval_int(inst->as.imm_i64));
            return true;

        case MIR_CONST_FLOAT:
            set_slot(ce, r, mir_cval_float(inst->as.imm_f64));
            return true;

        case MIR_CONST_BOOL:
            set_slot(ce, r, mir_cval_bool(inst->as.imm_bool));
            return true;

        case MIR_CONST_STRING: {
            MirConstValue v = { .kind = MIR_CVAL_STRING, .as.str = inst->as.imm_string };
            set_slot(ce, r, v);
            return true;
        }

        case MIR_CONST_FUNC:
            ce->error_msg = "consteval: function addresses are not supported as compile-time values";
            return false;

        case MIR_CONST_NULL:
            set_slot(ce, r, (MirConstValue){ .kind = MIR_CVAL_NULL });
            return true;

        /* Integer arithmetic */
        case MIR_ADD: case MIR_SUB: case MIR_MUL: case MIR_DIV: case MIR_MOD: {
            MirConstValue* l = get_slot(ce, inst->as.binary.lhs);
            MirConstValue* rv = get_slot(ce, inst->as.binary.rhs);
            if (l->kind != MIR_CVAL_INT || rv->kind != MIR_CVAL_INT) {
                ce->error_msg = "consteval: integer arithmetic on non-integer";
                return false;
            }
            int64_t result = 0;
            switch (inst->opcode) {
                case MIR_ADD: result = l->as.i64 + rv->as.i64; break;
                case MIR_SUB: result = l->as.i64 - rv->as.i64; break;
                case MIR_MUL: result = l->as.i64 * rv->as.i64; break;
                case MIR_DIV:
                    if (rv->as.i64 == 0) { ce->error_msg = "consteval: division by zero"; return false; }
                    result = l->as.i64 / rv->as.i64; break;
                case MIR_MOD:
                    if (rv->as.i64 == 0) { ce->error_msg = "consteval: modulo by zero"; return false; }
                    result = l->as.i64 % rv->as.i64; break;
                default: break;
            }
            set_slot(ce, r, mir_cval_int(result));
            return true;
        }

        /* Float arithmetic */
        case MIR_FADD: case MIR_FSUB: case MIR_FMUL: case MIR_FDIV: {
            MirConstValue* l = get_slot(ce, inst->as.binary.lhs);
            MirConstValue* rv = get_slot(ce, inst->as.binary.rhs);
            if (l->kind != MIR_CVAL_FLOAT || rv->kind != MIR_CVAL_FLOAT) {
                ce->error_msg = "consteval: float arithmetic on non-float";
                return false;
            }
            double result = 0;
            switch (inst->opcode) {
                case MIR_FADD: result = l->as.f64 + rv->as.f64; break;
                case MIR_FSUB: result = l->as.f64 - rv->as.f64; break;
                case MIR_FMUL: result = l->as.f64 * rv->as.f64; break;
                case MIR_FDIV: result = l->as.f64 / rv->as.f64; break;
                default: break;
            }
            set_slot(ce, r, mir_cval_float(result));
            return true;
        }

        /* Comparisons */
        case MIR_CMP_EQ: case MIR_CMP_NE: case MIR_CMP_LT:
        case MIR_CMP_LE: case MIR_CMP_GT: case MIR_CMP_GE: {
            MirConstValue* l = get_slot(ce, inst->as.binary.lhs);
            MirConstValue* rv = get_slot(ce, inst->as.binary.rhs);
            if (l->kind != MIR_CVAL_INT || rv->kind != MIR_CVAL_INT) {
                ce->error_msg = "consteval: comparison on non-integer";
                return false;
            }
            bool result = false;
            switch (inst->opcode) {
                case MIR_CMP_EQ: result = l->as.i64 == rv->as.i64; break;
                case MIR_CMP_NE: result = l->as.i64 != rv->as.i64; break;
                case MIR_CMP_LT: result = l->as.i64 < rv->as.i64; break;
                case MIR_CMP_LE: result = l->as.i64 <= rv->as.i64; break;
                case MIR_CMP_GT: result = l->as.i64 > rv->as.i64; break;
                case MIR_CMP_GE: result = l->as.i64 >= rv->as.i64; break;
                default: break;
            }
            set_slot(ce, r, mir_cval_bool(result));
            return true;
        }

        /* Unary */
        case MIR_NEG: {
            MirConstValue* op = get_slot(ce, inst->as.unary.operand);
            if (op->kind == MIR_CVAL_INT) set_slot(ce, r, mir_cval_int(-op->as.i64));
            else { ce->error_msg = "consteval: neg on non-integer"; return false; }
            return true;
        }
        case MIR_FNEG: {
            MirConstValue* op = get_slot(ce, inst->as.unary.operand);
            if (op->kind == MIR_CVAL_FLOAT) set_slot(ce, r, mir_cval_float(-op->as.f64));
            else { ce->error_msg = "consteval: fneg on non-float"; return false; }
            return true;
        }
        case MIR_LOGIC_NOT: {
            MirConstValue* op = get_slot(ce, inst->as.unary.operand);
            if (op->kind == MIR_CVAL_BOOL) set_slot(ce, r, mir_cval_bool(!op->as.b));
            else { ce->error_msg = "consteval: not on non-bool"; return false; }
            return true;
        }

        /* Bitwise */
        case MIR_BAND: case MIR_BOR: case MIR_BXOR: case MIR_SHL: case MIR_SHR: {
            MirConstValue* l = get_slot(ce, inst->as.binary.lhs);
            MirConstValue* rv = get_slot(ce, inst->as.binary.rhs);
            if (l->kind != MIR_CVAL_INT || rv->kind != MIR_CVAL_INT) {
                ce->error_msg = "consteval: bitwise on non-integer";
                return false;
            }
            int64_t result = 0;
            switch (inst->opcode) {
                case MIR_BAND: result = l->as.i64 & rv->as.i64; break;
                case MIR_BOR:  result = l->as.i64 | rv->as.i64; break;
                case MIR_BXOR: result = l->as.i64 ^ rv->as.i64; break;
                case MIR_SHL:  result = l->as.i64 << rv->as.i64; break;
                case MIR_SHR:  result = l->as.i64 >> rv->as.i64; break;
                default: break;
            }
            set_slot(ce, r, mir_cval_int(result));
            return true;
        }

        /* Type conversions */
        case MIR_SITOFP: {
            MirConstValue* op = get_slot(ce, inst->as.unary.operand);
            if (op->kind == MIR_CVAL_INT) set_slot(ce, r, mir_cval_float((double)op->as.i64));
            else { ce->error_msg = "consteval: sitofp on non-integer"; return false; }
            return true;
        }
        case MIR_FPTOSI: {
            MirConstValue* op = get_slot(ce, inst->as.unary.operand);
            if (op->kind == MIR_CVAL_FLOAT) set_slot(ce, r, mir_cval_int((int64_t)op->as.f64));
            else { ce->error_msg = "consteval: fptosi on non-float"; return false; }
            return true;
        }
        case MIR_ZEXT: case MIR_SEXT: case MIR_TRUNC: case MIR_CAST: {
            MirConstValue* op = get_slot(ce, inst->as.unary.operand);
            /* For consteval, integer extensions/truncations keep the value */
            set_slot(ce, r, *op);
            return true;
        }

        /* Memory: alloca → create an aggregate slot */
        case MIR_ALLOCA:
            /* Consteval allocas are simulated as value slots */
            set_slot(ce, r, (MirConstValue){ .kind = MIR_CVAL_INT, .as.i64 = 0 });
            return true;

        /* Memory: load/store — simplified: treat value IDs as addresses */
        case MIR_LOAD: {
            MirConstValue* ptr = get_slot(ce, inst->as.mem.ptr);
            /* In consteval, "loading" from an alloca slot just copies the value */
            set_slot(ce, r, *ptr);
            return true;
        }
        case MIR_STORE: {
            MirConstValue* val = get_slot(ce, inst->as.mem.value);
            /* Store into the alloca slot */
            set_slot(ce, inst->as.mem.ptr, *val);
            return true;
        }

        /* Copy / move */
        case MIR_COPY: case MIR_MOVE: {
            MirConstValue* src = get_slot(ce, inst->as.transfer.source);
            set_slot(ce, r, *src);
            return true;
        }

        /* NOP / debugloc — skip */
        case MIR_NOP:
        case MIR_DEBUGLOC:
            return true;

        /* Unsupported in consteval */
        default:
            ce->error_msg = "consteval: unsupported instruction";
            return false;
    }
}

/* ================================================================
 * Evaluate a function call
 * ================================================================ */

bool mir_consteval_call(MirConstEval* ce, MirFunction* func,
                        MirConstValue* args, int n_args,
                        MirConstValue* out_result) {
    if (!func || func->is_extern) {
        ce->error_msg = "consteval: cannot evaluate extern function";
        return false;
    }
    if (ce->call_depth >= ce->max_call_depth) {
        ce->error_msg = "consteval: maximum call depth exceeded";
        return false;
    }

    ce->call_depth++;

    /* Set up parameter slots */
    for (int i = 0; i < n_args && i < func->param_count; i++) {
        ensure_slot(ce, func->params[i].value_id);
        ce->slots[func->params[i].value_id] = args[i];
    }

    /* Interpret basic blocks: follow the CFG */
    MirBlock* current = func->entry_block;
    while (current) {
        for (MirInst* inst = current->first; inst; inst = inst->next) {
            /* Handle control flow specially */
            if (inst->opcode == MIR_BR) {
                current = inst->as.br.target;
                goto next_block;
            }
            if (inst->opcode == MIR_CONDBR) {
                MirConstValue* cond = get_slot(ce, inst->as.condbr.cond);
                if (cond->kind != MIR_CVAL_BOOL) {
                    ce->error_msg = "consteval: condbr on non-bool";
                    ce->call_depth--;
                    return false;
                }
                current = cond->as.b ? inst->as.condbr.true_bb : inst->as.condbr.false_bb;
                goto next_block;
            }
            if (inst->opcode == MIR_RET) {
                MirConstValue* val = get_slot(ce, inst->as.ret.value);
                *out_result = *val;
                ce->call_depth--;
                return true;
            }
            if (inst->opcode == MIR_RET_VOID) {
                *out_result = mir_cval_void();
                ce->call_depth--;
                return true;
            }

            /* Handle nested calls */
            if (inst->opcode == MIR_CALL) {
                MirFunction* callee = mir_module_find_function(ce->module, inst->as.call.func_name);
                if (!callee) {
                    ce->error_msg = "consteval: unknown function in call";
                    ce->call_depth--;
                    return false;
                }
                MirConstValue call_args[16];
                int nca = inst->as.call.n_args;
                if (nca > 16) nca = 16;
                for (int i = 0; i < nca; i++) {
                    call_args[i] = *get_slot(ce, inst->as.call.args[i]);
                }
                MirConstValue call_result;
                if (!mir_consteval_call(ce, callee, call_args, nca, &call_result)) {
                    ce->call_depth--;
                    return false;
                }
                if (inst->result != MIR_VALUE_NONE) {
                    set_slot(ce, inst->result, call_result);
                }
                continue;
            }

            /* Regular instruction */
            if (!mir_consteval_step(ce, inst)) {
                ce->call_depth--;
                return false;
            }
        }
        /* Block fell through without terminator — shouldn't happen */
        ce->error_msg = "consteval: block without terminator";
        ce->call_depth--;
        return false;

    next_block:
        continue;
    }

    ce->error_msg = "consteval: reached null block";
    ce->call_depth--;
    return false;
}
