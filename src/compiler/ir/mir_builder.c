/*
 * Casprix Compiler — MIR Builder Implementation
 *
 * Provides the instruction emission API used by the AST→MIR lowering pass.
 * Every mir_build_* function creates a MirInst, assigns an SSA value ID,
 * appends it to the current basic block, and returns the result value.
 *
 * Design decisions:
 *   - All instructions are arena-allocated (zero-cost bulk deallocation).
 *   - CFG edges (predecessors/successors) are maintained automatically
 *     whenever a terminator instruction is emitted.
 *   - Phi edges are added post-construction via mir_phi_add_edge().
 */

#include "mir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ================================================================
 * Internal helpers
 * ================================================================ */

static MirInst* alloc_inst(MirBuilder* b, MirOpcode op, MirType* type) {
    MirInst* inst = (MirInst*)mir_arena_alloc(b->module->arena, sizeof(MirInst));
    inst->opcode = op;
    inst->type = type;
    inst->result = MIR_VALUE_NONE;
    inst->next = NULL;
    inst->prev = NULL;
    inst->src_line = 0;
    inst->src_col = 0;
    return inst;
}

static MirValueId emit(MirBuilder* b, MirInst* inst) {
    assert(b->current_block && "No current block set in MIR builder");
    if (inst->type && inst->type->kind != MIR_TYPE_VOID) {
        inst->result = mir_function_new_value(b->func, inst->type);
    }
    mir_block_append(b->current_block, inst);
    return inst->result;
}

/* Emit a void instruction (no result value). */
static void emit_void(MirBuilder* b, MirInst* inst) {
    assert(b->current_block && "No current block set in MIR builder");
    inst->result = MIR_VALUE_NONE;
    mir_block_append(b->current_block, inst);
}

/* Wire CFG edges between current block and target. */
static void link_cfg(MirBlock* from, MirBlock* to) {
    mir_block_add_successor(from, to);
    mir_block_add_predecessor(to, from);
}

/* ================================================================
 * Builder lifecycle
 * ================================================================ */

void mir_builder_init(MirBuilder* b, MirModule* module, MirFunction* func) {
    b->module = module;
    b->func = func;
    b->current_block = NULL;
}

void mir_builder_set_block(MirBuilder* b, MirBlock* block) {
    b->current_block = block;
}

/* ================================================================
 * Constants
 * ================================================================ */

MirValueId mir_build_const_int(MirBuilder* b, int64_t val, MirType* type) {
    MirInst* inst = alloc_inst(b, MIR_CONST_INT, type);
    inst->as.imm_i64 = val;
    return emit(b, inst);
}

MirValueId mir_build_const_float(MirBuilder* b, double val, MirType* type) {
    MirInst* inst = alloc_inst(b, MIR_CONST_FLOAT, type);
    inst->as.imm_f64 = val;
    return emit(b, inst);
}

MirValueId mir_build_const_bool(MirBuilder* b, bool val) {
    MirInst* inst = alloc_inst(b, MIR_CONST_BOOL, mir_type_bool(b->module));
    inst->as.imm_bool = val;
    return emit(b, inst);
}

MirValueId mir_build_const_string(MirBuilder* b, const char* str) {
    MirInst* inst = alloc_inst(b, MIR_CONST_STRING,
                               mir_type_ptr(b->module, mir_type_i8(b->module)));
    int idx = mir_module_add_string(b->module, str);
    inst->as.imm_string = b->module->string_literals[idx];
    return emit(b, inst);
}

MirValueId mir_build_const_func(MirBuilder* b, const char* func_name, MirType* type) {
    MirInst* inst = alloc_inst(b, MIR_CONST_FUNC, type);
    inst->as.imm_string = mir_arena_strdup(b->module->arena, func_name);
    return emit(b, inst);
}

MirValueId mir_build_const_null(MirBuilder* b, MirType* ptr_type) {
    MirInst* inst = alloc_inst(b, MIR_CONST_NULL, ptr_type);
    return emit(b, inst);
}

MirValueId mir_build_global_addr(MirBuilder* b, const char* global_name, MirType* value_type) {
    MirInst* inst = alloc_inst(b, MIR_GLOBAL_ADDR,
                               mir_type_ptr(b->module, value_type));
    inst->as.global_name = mir_arena_strdup(b->module->arena, global_name);
    return emit(b, inst);
}

/* ================================================================
 * Binary arithmetic (integer)
 * ================================================================ */

static MirValueId build_binary(MirBuilder* b, MirOpcode op,
                                MirValueId lhs, MirValueId rhs) {
    MirType* type = mir_function_value_type(b->func, lhs);
    MirInst* inst = alloc_inst(b, op, type);
    inst->as.binary.lhs = lhs;
    inst->as.binary.rhs = rhs;
    return emit(b, inst);
}

MirValueId mir_build_add(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_binary(b, MIR_ADD, lhs, rhs);
}
MirValueId mir_build_sub(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_binary(b, MIR_SUB, lhs, rhs);
}
MirValueId mir_build_mul(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_binary(b, MIR_MUL, lhs, rhs);
}
MirValueId mir_build_div(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_binary(b, MIR_DIV, lhs, rhs);
}
MirValueId mir_build_mod(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_binary(b, MIR_MOD, lhs, rhs);
}

MirValueId mir_build_neg(MirBuilder* b, MirValueId operand) {
    MirType* type = mir_function_value_type(b->func, operand);
    MirInst* inst = alloc_inst(b, MIR_NEG, type);
    inst->as.unary.operand = operand;
    inst->as.unary.target_type = NULL;
    return emit(b, inst);
}

/* ================================================================
 * Float arithmetic
 * ================================================================ */

MirValueId mir_build_fadd(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_binary(b, MIR_FADD, lhs, rhs);
}
MirValueId mir_build_fsub(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_binary(b, MIR_FSUB, lhs, rhs);
}
MirValueId mir_build_fmul(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_binary(b, MIR_FMUL, lhs, rhs);
}
MirValueId mir_build_fdiv(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_binary(b, MIR_FDIV, lhs, rhs);
}
MirValueId mir_build_fneg(MirBuilder* b, MirValueId operand) {
    MirType* type = mir_function_value_type(b->func, operand);
    MirInst* inst = alloc_inst(b, MIR_FNEG, type);
    inst->as.unary.operand = operand;
    inst->as.unary.target_type = NULL;
    return emit(b, inst);
}

/* ================================================================
 * Comparison — always returns bool
 * ================================================================ */

static MirValueId build_cmp(MirBuilder* b, MirOpcode op,
                             MirValueId lhs, MirValueId rhs) {
    MirInst* inst = alloc_inst(b, op, mir_type_bool(b->module));
    inst->as.binary.lhs = lhs;
    inst->as.binary.rhs = rhs;
    return emit(b, inst);
}

MirValueId mir_build_cmp_eq(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_cmp(b, MIR_CMP_EQ, lhs, rhs);
}
MirValueId mir_build_cmp_ne(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_cmp(b, MIR_CMP_NE, lhs, rhs);
}
MirValueId mir_build_cmp_lt(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_cmp(b, MIR_CMP_LT, lhs, rhs);
}
MirValueId mir_build_cmp_le(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_cmp(b, MIR_CMP_LE, lhs, rhs);
}
MirValueId mir_build_cmp_gt(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_cmp(b, MIR_CMP_GT, lhs, rhs);
}
MirValueId mir_build_cmp_ge(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_cmp(b, MIR_CMP_GE, lhs, rhs);
}

/* ================================================================
 * Bitwise
 * ================================================================ */

MirValueId mir_build_and(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_binary(b, MIR_BAND, lhs, rhs);
}
MirValueId mir_build_or(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_binary(b, MIR_BOR, lhs, rhs);
}
MirValueId mir_build_xor(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_binary(b, MIR_BXOR, lhs, rhs);
}
MirValueId mir_build_shl(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_binary(b, MIR_SHL, lhs, rhs);
}
MirValueId mir_build_shr(MirBuilder* b, MirValueId lhs, MirValueId rhs) {
    return build_binary(b, MIR_SHR, lhs, rhs);
}

MirValueId mir_build_not(MirBuilder* b, MirValueId operand) {
    MirType* type = mir_function_value_type(b->func, operand);
    MirInst* inst = alloc_inst(b, MIR_BNOT, type);
    inst->as.unary.operand = operand;
    inst->as.unary.target_type = NULL;
    return emit(b, inst);
}

/* ================================================================
 * Memory operations
 * ================================================================ */

MirValueId mir_build_alloca(MirBuilder* b, MirType* type) {
    MirType* ptr_type = mir_type_ptr(b->module, type);
    MirInst* inst = alloc_inst(b, MIR_ALLOCA, ptr_type);
    inst->as.alloca.alloc_type = type;
    inst->as.alloca.count = 1;
    return emit(b, inst);
}

MirValueId mir_build_load(MirBuilder* b, MirValueId ptr, MirType* type) {
    MirInst* inst = alloc_inst(b, MIR_LOAD, type);
    inst->as.mem.ptr = ptr;
    inst->as.mem.value = MIR_VALUE_NONE;
    return emit(b, inst);
}

void mir_build_store(MirBuilder* b, MirValueId ptr, MirValueId value) {
    MirInst* inst = alloc_inst(b, MIR_STORE, mir_type_void(b->module));
    inst->as.mem.ptr = ptr;
    inst->as.mem.value = value;
    emit_void(b, inst);
}

MirValueId mir_build_get_field_ptr(MirBuilder* b, MirValueId base, int field_idx) {
    MirType* base_type = mir_function_value_type(b->func, base);
    MirType* result_type = base_type; /* Simplified — real impl resolves field type */

    /* If base is ptr to struct, get the field type */
    if (base_type && base_type->kind == MIR_TYPE_PTR &&
        base_type->as.ptr.pointee && base_type->as.ptr.pointee->kind == MIR_TYPE_STRUCT) {
        MirType* st = base_type->as.ptr.pointee;
        if (field_idx < st->as.strct.n_fields) {
            result_type = mir_type_ptr(b->module, st->as.strct.fields[field_idx]);
        }
    }

    MirInst* inst = alloc_inst(b, MIR_GET_FIELD_PTR, result_type);
    inst->as.gep.base = base;
    inst->as.gep.field_index = field_idx;
    inst->as.gep.index = MIR_VALUE_NONE;
    return emit(b, inst);
}

MirValueId mir_build_get_elem_ptr(MirBuilder* b, MirValueId base, MirValueId idx) {
    MirType* base_type = mir_function_value_type(b->func, base);
    MirInst* inst = alloc_inst(b, MIR_GET_ELEM_PTR, base_type);
    inst->as.gep.base = base;
    inst->as.gep.field_index = -1;
    inst->as.gep.index = idx;
    return emit(b, inst);
}

/* ================================================================
 * Control flow
 * ================================================================ */

void mir_build_br(MirBuilder* b, MirBlock* target) {
    MirInst* inst = alloc_inst(b, MIR_BR, mir_type_void(b->module));
    inst->as.br.target = target;
    emit_void(b, inst);
    link_cfg(b->current_block, target);
}

void mir_build_condbr(MirBuilder* b, MirValueId cond,
                      MirBlock* true_bb, MirBlock* false_bb) {
    MirInst* inst = alloc_inst(b, MIR_CONDBR, mir_type_void(b->module));
    inst->as.condbr.cond = cond;
    inst->as.condbr.true_bb = true_bb;
    inst->as.condbr.false_bb = false_bb;
    emit_void(b, inst);
    link_cfg(b->current_block, true_bb);
    link_cfg(b->current_block, false_bb);
}

void mir_build_ret(MirBuilder* b, MirValueId value) {
    MirInst* inst = alloc_inst(b, MIR_RET, mir_type_void(b->module));
    inst->as.ret.value = value;
    emit_void(b, inst);
}

void mir_build_ret_void(MirBuilder* b) {
    MirInst* inst = alloc_inst(b, MIR_RET_VOID, mir_type_void(b->module));
    emit_void(b, inst);
}

MirValueId mir_build_suspend(MirBuilder* b, MirBlock* resume_bb, MirValueId future) {
    MirType* ret_type = mir_function_value_type(b->func, future);
    // Result of suspend is the result of the future once resumed
    MirInst* inst = alloc_inst(b, MIR_SUSPEND, ret_type);
    inst->as.suspend.resume_bb = resume_bb;
    inst->as.suspend.future = future;
    emit(b, inst);
    link_cfg(b->current_block, resume_bb);
    return inst->result;
}

/* ================================================================
 * Function calls
 * ================================================================ */

MirValueId mir_build_call(MirBuilder* b, const char* func_name,
                           MirValueId* args, int n_args, MirType* ret_type) {
    MirInst* inst = alloc_inst(b, MIR_CALL, ret_type);
    inst->as.call.func_name = mir_arena_strdup(b->module->arena, func_name);
    inst->as.call.n_args = n_args;
    inst->as.call.callee = MIR_VALUE_NONE;
    if (n_args > 0) {
        inst->as.call.args = (MirValueId*)mir_arena_alloc(b->module->arena,
                              n_args * sizeof(MirValueId));
        memcpy(inst->as.call.args, args, n_args * sizeof(MirValueId));
    } else {
        inst->as.call.args = NULL;
    }
    return emit(b, inst);
}

MirValueId mir_build_call_indirect(MirBuilder* b, MirValueId callee,
                                    MirValueId* args, int n_args, MirType* ret_type) {
    MirInst* inst = alloc_inst(b, MIR_CALL_INDIRECT, ret_type);
    inst->as.call.func_name = NULL;
    inst->as.call.callee = callee;
    inst->as.call.n_args = n_args;
    if (n_args > 0) {
        inst->as.call.args = (MirValueId*)mir_arena_alloc(b->module->arena,
                              n_args * sizeof(MirValueId));
        memcpy(inst->as.call.args, args, n_args * sizeof(MirValueId));
    } else {
        inst->as.call.args = NULL;
    }
    return emit(b, inst);
}

MirValueId mir_build_call_virtual(MirBuilder* b, MirValueId self,
                                   int vtable_idx, MirValueId* args,
                                   int n_args, MirType* ret_type) {
    MirInst* inst = alloc_inst(b, MIR_CALL_VIRTUAL, ret_type);
    inst->as.vcall.self_obj = self;
    inst->as.vcall.vtable_index = vtable_idx;
    inst->as.vcall.n_args = n_args;
    if (n_args > 0) {
        inst->as.vcall.args = (MirValueId*)mir_arena_alloc(b->module->arena,
                               n_args * sizeof(MirValueId));
        memcpy(inst->as.vcall.args, args, n_args * sizeof(MirValueId));
    } else {
        inst->as.vcall.args = NULL;
    }
    return emit(b, inst);
}

/* ================================================================
 * Phi nodes
 * ================================================================ */

MirValueId mir_build_phi(MirBuilder* b, MirType* type) {
    MirInst* inst = alloc_inst(b, MIR_PHI, type);
    inst->as.phi.edges = NULL;
    inst->as.phi.n_edges = 0;
    return emit(b, inst);
}

void mir_phi_add_edge(MirInst* phi_inst, MirBlock* block, MirValueId value) {
    assert(phi_inst->opcode == MIR_PHI);
    MirArena* arena = block->parent->parent->arena;

    int n = phi_inst->as.phi.n_edges;
    MirPhiEdge* new_edges = (MirPhiEdge*)mir_arena_alloc(arena,
                             (n + 1) * sizeof(MirPhiEdge));
    if (phi_inst->as.phi.edges) {
        memcpy(new_edges, phi_inst->as.phi.edges, n * sizeof(MirPhiEdge));
    }
    new_edges[n].block = block;
    new_edges[n].value = value;
    phi_inst->as.phi.edges = new_edges;
    phi_inst->as.phi.n_edges = n + 1;
}

/* ================================================================
 * Type conversions
 * ================================================================ */

static MirValueId build_convert(MirBuilder* b, MirOpcode op,
                                 MirValueId val, MirType* target) {
    MirInst* inst = alloc_inst(b, op, target);
    inst->as.unary.operand = val;
    inst->as.unary.target_type = target;
    return emit(b, inst);
}

MirValueId mir_build_cast(MirBuilder* b, MirValueId val, MirType* target) {
    return build_convert(b, MIR_CAST, val, target);
}
MirValueId mir_build_zext(MirBuilder* b, MirValueId val, MirType* target) {
    return build_convert(b, MIR_ZEXT, val, target);
}
MirValueId mir_build_sext(MirBuilder* b, MirValueId val, MirType* target) {
    return build_convert(b, MIR_SEXT, val, target);
}
MirValueId mir_build_trunc(MirBuilder* b, MirValueId val, MirType* target) {
    return build_convert(b, MIR_TRUNC, val, target);
}
MirValueId mir_build_sitofp(MirBuilder* b, MirValueId val, MirType* target) {
    return build_convert(b, MIR_SITOFP, val, target);
}
MirValueId mir_build_fptosi(MirBuilder* b, MirValueId val, MirType* target) {
    return build_convert(b, MIR_FPTOSI, val, target);
}

/* ================================================================
 * Ownership / safety instructions
 * ================================================================ */

MirValueId mir_build_borrow(MirBuilder* b, MirValueId source) {
    MirType* src_type = mir_function_value_type(b->func, source);
    MirType* ref_type = mir_type_ref(b->module, src_type, false);
    MirInst* inst = alloc_inst(b, MIR_BORROW, ref_type);
    inst->as.transfer.source = source;
    return emit(b, inst);
}

MirValueId mir_build_borrow_mut(MirBuilder* b, MirValueId source) {
    MirType* src_type = mir_function_value_type(b->func, source);
    MirType* ref_type = mir_type_ref(b->module, src_type, true);
    MirInst* inst = alloc_inst(b, MIR_BORROW_MUT, ref_type);
    inst->as.transfer.source = source;
    return emit(b, inst);
}

MirValueId mir_build_move(MirBuilder* b, MirValueId source) {
    MirType* type = mir_function_value_type(b->func, source);
    MirInst* inst = alloc_inst(b, MIR_MOVE, type);
    inst->as.transfer.source = source;
    return emit(b, inst);
}

void mir_build_drop(MirBuilder* b, MirValueId value) {
    MirInst* inst = alloc_inst(b, MIR_DROP, mir_type_void(b->module));
    inst->as.refop.ptr = value;
    emit_void(b, inst);
}

/* ================================================================
 * Reference counting
 * ================================================================ */

void mir_build_arc_retain(MirBuilder* b, MirValueId ptr) {
    MirInst* inst = alloc_inst(b, MIR_ARC_RETAIN, mir_type_void(b->module));
    inst->as.refop.ptr = ptr;
    emit_void(b, inst);
}

void mir_build_arc_release(MirBuilder* b, MirValueId ptr) {
    MirInst* inst = alloc_inst(b, MIR_ARC_RELEASE, mir_type_void(b->module));
    inst->as.refop.ptr = ptr;
    emit_void(b, inst);
}

MirValueId mir_build_obj_alloc(MirBuilder* b, const char* class_name, int size) {
    MirType* ptr_type = mir_type_ptr(b->module, mir_type_i8(b->module));
    MirInst* inst = alloc_inst(b, MIR_OBJ_ALLOC, ptr_type);
    inst->as.obj_alloc.class_name = mir_arena_strdup(b->module->arena, class_name);
    inst->as.obj_alloc.size = size;
    return emit(b, inst);
}

/* ================================================================
 * Aggregate operations
 * ================================================================ */

MirValueId mir_build_struct_init(MirBuilder* b, MirType* type,
                                  MirValueId* fields, int n_fields) {
    MirInst* inst = alloc_inst(b, MIR_STRUCT_INIT, type);
    inst->as.struct_init.n_fields = n_fields;
    if (n_fields > 0) {
        inst->as.struct_init.fields = (MirValueId*)mir_arena_alloc(b->module->arena,
                                      n_fields * sizeof(MirValueId));
        memcpy(inst->as.struct_init.fields, fields, n_fields * sizeof(MirValueId));
    }
    return emit(b, inst);
}

MirValueId mir_build_extract(MirBuilder* b, MirValueId agg, int field_idx) {
    MirType* agg_type = mir_function_value_type(b->func, agg);
    MirType* result_type = agg_type;
    if (agg_type && agg_type->kind == MIR_TYPE_STRUCT &&
        field_idx < agg_type->as.strct.n_fields) {
        result_type = agg_type->as.strct.fields[field_idx];
    }
    MirInst* inst = alloc_inst(b, MIR_EXTRACT, result_type);
    inst->as.field_op.aggregate = agg;
    inst->as.field_op.field_idx = field_idx;
    inst->as.field_op.insert_val = MIR_VALUE_NONE;
    return emit(b, inst);
}

MirValueId mir_build_insert(MirBuilder* b, MirValueId agg,
                             int field_idx, MirValueId val) {
    MirType* agg_type = mir_function_value_type(b->func, agg);
    MirInst* inst = alloc_inst(b, MIR_INSERT, agg_type);
    inst->as.field_op.aggregate = agg;
    inst->as.field_op.field_idx = field_idx;
    inst->as.field_op.insert_val = val;
    return emit(b, inst);
}
