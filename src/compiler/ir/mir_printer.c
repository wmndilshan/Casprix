/*
 * Casprix Compiler — MIR Printer & Validator
 *
 * Textual IR dump (similar to LLVM IR format) for debugging and testing.
 * Structural validation checks SSA dominance, block termination, phi correctness.
 */

#include "mir.h"
#include "mir_mem2reg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ================================================================
 * Type Printer
 * ================================================================ */

void mir_print_type(MirType* type, FILE* out) {
    if (!type) { fprintf(out, "void"); return; }
    switch (type->kind) {
        case MIR_TYPE_VOID:     fprintf(out, "void"); break;
        case MIR_TYPE_BOOL:     fprintf(out, "bool"); break;
        case MIR_TYPE_I8:       fprintf(out, "i8"); break;
        case MIR_TYPE_I16:      fprintf(out, "i16"); break;
        case MIR_TYPE_I32:      fprintf(out, "i32"); break;
        case MIR_TYPE_I64:      fprintf(out, "i64"); break;
        case MIR_TYPE_U8:       fprintf(out, "u8"); break;
        case MIR_TYPE_U16:      fprintf(out, "u16"); break;
        case MIR_TYPE_U32:      fprintf(out, "u32"); break;
        case MIR_TYPE_U64:      fprintf(out, "u64"); break;
        case MIR_TYPE_F32:      fprintf(out, "f32"); break;
        case MIR_TYPE_F64:      fprintf(out, "f64"); break;
        case MIR_TYPE_PTR:
            fprintf(out, "ptr<");
            mir_print_type(type->as.ptr.pointee, out);
            fprintf(out, ">");
            break;
        case MIR_TYPE_REF:
            fprintf(out, "&");
            mir_print_type(type->as.ref.pointee, out);
            break;
        case MIR_TYPE_MUT_REF:
            fprintf(out, "&mut ");
            mir_print_type(type->as.ref.pointee, out);
            break;
        case MIR_TYPE_FUNC:
            fprintf(out, "fn(");
            for (int i = 0; i < type->as.func.n_params; i++) {
                if (i > 0) fprintf(out, ", ");
                mir_print_type(type->as.func.params[i], out);
            }
            fprintf(out, ") -> ");
            mir_print_type(type->as.func.ret, out);
            break;
        case MIR_TYPE_STRUCT:
            fprintf(out, "%%%s", type->as.strct.name ? type->as.strct.name : "anon");
            break;
        case MIR_TYPE_ARRAY:
            fprintf(out, "[");
            mir_print_type(type->as.array.elem, out);
            fprintf(out, "; %d]", type->as.array.count);
            break;
        case MIR_TYPE_SLICE:
            fprintf(out, "[");
            mir_print_type(type->as.slice.elem, out);
            fprintf(out, "]");
            break;
        case MIR_TYPE_AGGREGATE:
            fprintf(out, "aggregate");
            break;
    }
}

/* ================================================================
 * Instruction Printer
 * ================================================================ */

static void print_value(MirValueId v, FILE* out) {
    if (v == MIR_VALUE_NONE) fprintf(out, "void");
    else fprintf(out, "%%v%u", v);
}

void mir_print_inst(MirInst* inst, FILE* out) {
    /* Print result assignment if non-void */
    if (inst->result != MIR_VALUE_NONE) {
        fprintf(out, "    %%v%u = ", inst->result);
    } else {
        fprintf(out, "    ");
    }

    switch (inst->opcode) {
        /* Constants */
        case MIR_CONST_INT:
            fprintf(out, "const.int ");
            mir_print_type(inst->type, out);
            fprintf(out, " %lld", (long long)inst->as.imm_i64);
            break;
        case MIR_CONST_FLOAT:
            fprintf(out, "const.float ");
            mir_print_type(inst->type, out);
            fprintf(out, " %g", inst->as.imm_f64);
            break;
        case MIR_CONST_BOOL:
            fprintf(out, "const.bool %s", inst->as.imm_bool ? "true" : "false");
            break;
        case MIR_CONST_STRING:
            fprintf(out, "const.string \"%s\"", inst->as.imm_string ? inst->as.imm_string : "");
            break;
        case MIR_CONST_FUNC:
            fprintf(out, "const.func ");
            mir_print_type(inst->type, out);
            fprintf(out, " @%s", inst->as.imm_string ? inst->as.imm_string : "?");
            break;
        case MIR_CONST_NULL:
            fprintf(out, "const.null ");
            mir_print_type(inst->type, out);
            break;
        case MIR_GLOBAL_ADDR:
            fprintf(out, "global.addr ");
            mir_print_type(inst->type, out);
            fprintf(out, " @%s", inst->as.global_name ? inst->as.global_name : "?");
            break;

        /* Integer arithmetic */
        case MIR_ADD:  fprintf(out, "add ");  goto print_binary;
        case MIR_SUB:  fprintf(out, "sub ");  goto print_binary;
        case MIR_MUL:  fprintf(out, "mul ");  goto print_binary;
        case MIR_DIV:  fprintf(out, "div ");  goto print_binary;
        case MIR_MOD:  fprintf(out, "mod ");  goto print_binary;
        /* Float arithmetic */
        case MIR_FADD: fprintf(out, "fadd "); goto print_binary;
        case MIR_FSUB: fprintf(out, "fsub "); goto print_binary;
        case MIR_FMUL: fprintf(out, "fmul "); goto print_binary;
        case MIR_FDIV: fprintf(out, "fdiv "); goto print_binary;
        /* Bitwise */
        case MIR_BAND: fprintf(out, "and ");  goto print_binary;
        case MIR_BOR:  fprintf(out, "or ");   goto print_binary;
        case MIR_BXOR: fprintf(out, "xor ");  goto print_binary;
        case MIR_SHL:  fprintf(out, "shl ");  goto print_binary;
        case MIR_SHR:  fprintf(out, "shr ");  goto print_binary;
        case MIR_USHR: fprintf(out, "ushr "); goto print_binary;
        /* Comparison */
        case MIR_CMP_EQ: fprintf(out, "cmp.eq "); goto print_binary;
        case MIR_CMP_NE: fprintf(out, "cmp.ne "); goto print_binary;
        case MIR_CMP_LT: fprintf(out, "cmp.lt "); goto print_binary;
        case MIR_CMP_LE: fprintf(out, "cmp.le "); goto print_binary;
        case MIR_CMP_GT: fprintf(out, "cmp.gt "); goto print_binary;
        case MIR_CMP_GE: fprintf(out, "cmp.ge "); goto print_binary;
        /* Logical */
        case MIR_LOGIC_AND: fprintf(out, "logic.and "); goto print_binary;
        case MIR_LOGIC_OR:  fprintf(out, "logic.or ");  goto print_binary;

        print_binary:
            mir_print_type(inst->type, out);
            fprintf(out, " ");
            print_value(inst->as.binary.lhs, out);
            fprintf(out, ", ");
            print_value(inst->as.binary.rhs, out);
            break;

        /* Unary ops */
        case MIR_NEG:       fprintf(out, "neg "); goto print_unary;
        case MIR_FNEG:      fprintf(out, "fneg "); goto print_unary;
        case MIR_BNOT:      fprintf(out, "bnot "); goto print_unary;
        case MIR_LOGIC_NOT: fprintf(out, "logic.not "); goto print_unary;
        case MIR_CAST:      fprintf(out, "cast "); goto print_unary;
        case MIR_BITCAST:   fprintf(out, "bitcast "); goto print_unary;
        case MIR_TRUNC:     fprintf(out, "trunc "); goto print_unary;
        case MIR_ZEXT:      fprintf(out, "zext "); goto print_unary;
        case MIR_SEXT:      fprintf(out, "sext "); goto print_unary;
        case MIR_SITOFP:    fprintf(out, "sitofp "); goto print_unary;
        case MIR_FPTOSI:    fprintf(out, "fptosi "); goto print_unary;

        print_unary:
            print_value(inst->as.unary.operand, out);
            if (inst->as.unary.target_type) {
                fprintf(out, " to ");
                mir_print_type(inst->as.unary.target_type, out);
            }
            break;

        /* Memory */
        case MIR_ALLOCA:
            fprintf(out, "alloca ");
            mir_print_type(inst->as.alloca.alloc_type, out);
            if (inst->as.alloca.count > 1)
                fprintf(out, ", %d", inst->as.alloca.count);
            break;
        case MIR_LOAD:
            fprintf(out, "load ");
            mir_print_type(inst->type, out);
            fprintf(out, ", ");
            print_value(inst->as.mem.ptr, out);
            break;
        case MIR_STORE:
            fprintf(out, "store ");
            print_value(inst->as.mem.ptr, out);
            fprintf(out, ", ");
            print_value(inst->as.mem.value, out);
            break;
        case MIR_GET_FIELD_PTR:
            fprintf(out, "gep.field ");
            print_value(inst->as.gep.base, out);
            fprintf(out, ", %d", inst->as.gep.field_index);
            break;
        case MIR_GET_ELEM_PTR:
            fprintf(out, "gep.elem ");
            print_value(inst->as.gep.base, out);
            fprintf(out, ", ");
            print_value(inst->as.gep.index, out);
            break;

        /* Control flow */
        case MIR_BR:
            fprintf(out, "br bb%u", inst->as.br.target->id);
            break;
        case MIR_CONDBR:
            fprintf(out, "condbr ");
            print_value(inst->as.condbr.cond, out);
            fprintf(out, ", bb%u, bb%u",
                    inst->as.condbr.true_bb->id, inst->as.condbr.false_bb->id);
            break;
        case MIR_SWITCH:
            fprintf(out, "switch ");
            print_value(inst->as.sw.discriminant, out);
            fprintf(out, " [");
            for (int i = 0; i < inst->as.sw.n_cases; i++) {
                if (i > 0) fprintf(out, ", ");
                fprintf(out, "%lld: bb%u", (long long)inst->as.sw.case_values[i],
                        inst->as.sw.targets[i]->id);
            }
            fprintf(out, ", default: bb%u]",
                    inst->as.sw.default_bb ? inst->as.sw.default_bb->id : 0);
            break;
        case MIR_RET:
            fprintf(out, "ret ");
            print_value(inst->as.ret.value, out);
            break;
        case MIR_RET_VOID:
            fprintf(out, "ret void");
            break;
        case MIR_UNREACHABLE:
            fprintf(out, "unreachable");
            break;

        /* Calls */
        case MIR_CALL:
            fprintf(out, "call ");
            mir_print_type(inst->type, out);
            fprintf(out, " @%s(", inst->as.call.func_name ? inst->as.call.func_name : "?");
            for (int i = 0; i < inst->as.call.n_args; i++) {
                if (i > 0) fprintf(out, ", ");
                print_value(inst->as.call.args[i], out);
            }
            fprintf(out, ")");
            break;
        case MIR_CALL_INDIRECT:
            fprintf(out, "call.indirect ");
            print_value(inst->as.call.callee, out);
            fprintf(out, "(");
            for (int i = 0; i < inst->as.call.n_args; i++) {
                if (i > 0) fprintf(out, ", ");
                print_value(inst->as.call.args[i], out);
            }
            fprintf(out, ")");
            break;
        case MIR_CALL_VIRTUAL:
            fprintf(out, "call.virtual ");
            print_value(inst->as.vcall.self_obj, out);
            fprintf(out, "[%d](", inst->as.vcall.vtable_index);
            for (int i = 0; i < inst->as.vcall.n_args; i++) {
                if (i > 0) fprintf(out, ", ");
                print_value(inst->as.vcall.args[i], out);
            }
            fprintf(out, ")");
            break;

        /* Phi */
        case MIR_PHI:
            fprintf(out, "phi ");
            mir_print_type(inst->type, out);
            fprintf(out, " [");
            for (int i = 0; i < inst->as.phi.n_edges; i++) {
                if (i > 0) fprintf(out, ", ");
                fprintf(out, "bb%u: ", inst->as.phi.edges[i].block->id);
                print_value(inst->as.phi.edges[i].value, out);
            }
            fprintf(out, "]");
            break;
        case MIR_COPY:
            fprintf(out, "copy ");
            print_value(inst->as.transfer.source, out);
            break;

        /* Ownership */
        case MIR_BORROW:
            fprintf(out, "borrow ");
            print_value(inst->as.transfer.source, out);
            break;
        case MIR_BORROW_MUT:
            fprintf(out, "borrow.mut ");
            print_value(inst->as.transfer.source, out);
            break;
        case MIR_MOVE:
            fprintf(out, "move ");
            print_value(inst->as.transfer.source, out);
            break;
        case MIR_DROP:
            fprintf(out, "drop ");
            print_value(inst->as.refop.ptr, out);
            break;

        /* ARC */
        case MIR_ARC_RETAIN:
            fprintf(out, "arc.retain ");
            print_value(inst->as.refop.ptr, out);
            break;
        case MIR_ARC_RELEASE:
            fprintf(out, "arc.release ");
            print_value(inst->as.refop.ptr, out);
            break;
        case MIR_OBJ_ALLOC:
            fprintf(out, "obj.alloc @%s, %d",
                    inst->as.obj_alloc.class_name, inst->as.obj_alloc.size);
            break;

        /* Aggregates */
        case MIR_STRUCT_INIT:
            fprintf(out, "struct.init ");
            mir_print_type(inst->type, out);
            fprintf(out, " {");
            for (int i = 0; i < inst->as.struct_init.n_fields; i++) {
                if (i > 0) fprintf(out, ", ");
                print_value(inst->as.struct_init.fields[i], out);
            }
            fprintf(out, "}");
            break;
        case MIR_EXTRACT:
            fprintf(out, "extract ");
            print_value(inst->as.field_op.aggregate, out);
            fprintf(out, ", %d", inst->as.field_op.field_idx);
            break;
        case MIR_INSERT:
            fprintf(out, "insert ");
            print_value(inst->as.field_op.aggregate, out);
            fprintf(out, ", %d, ", inst->as.field_op.field_idx);
            print_value(inst->as.field_op.insert_val, out);
            break;

        /* Debug */
        case MIR_NOP:
            fprintf(out, "nop");
            break;
        case MIR_DEBUGLOC:
            fprintf(out, "debugloc %s:%d:%d",
                    inst->as.debug.file ? inst->as.debug.file : "?",
                    inst->as.debug.line, inst->as.debug.column);
            break;
    }

    /* Source location annotation */
    if (inst->src_line > 0) {
        fprintf(out, "  ; line %d", inst->src_line);
    }

    fprintf(out, "\n");
}

/* ================================================================
 * Block Printer
 * ================================================================ */

void mir_print_block(MirBlock* block, FILE* out) {
    fprintf(out, "  bb%u", block->id);
    if (block->label) fprintf(out, " (%s)", block->label);
    fprintf(out, ":");

    /* Print predecessors */
    if (block->pred_count > 0) {
        fprintf(out, "  ; preds: ");
        for (int i = 0; i < block->pred_count; i++) {
            if (i > 0) fprintf(out, ", ");
            fprintf(out, "bb%u", block->predecessors[i]->id);
        }
    }
    fprintf(out, "\n");

    for (MirInst* inst = block->first; inst; inst = inst->next) {
        mir_print_inst(inst, out);
    }
    fprintf(out, "\n");
}

/* ================================================================
 * Function Printer
 * ================================================================ */

void mir_print_function(MirFunction* func, FILE* out) {
    if (func->is_extern) {
        fprintf(out, "declare ");
    } else {
        fprintf(out, "define ");
    }

    mir_print_type(func->return_type, out);
    fprintf(out, " @%s(", func->name);

    for (int i = 0; i < func->param_count; i++) {
        if (i > 0) fprintf(out, ", ");
        mir_print_type(func->params[i].type, out);
        fprintf(out, " %%v%u", func->params[i].value_id);
        if (func->params[i].name) {
            fprintf(out, " /* %s */", func->params[i].name);
        }
    }
    fprintf(out, ")");

    if (func->is_constexpr) fprintf(out, " constexpr");
    if (func->is_extern) { fprintf(out, "\n\n"); return; }

    fprintf(out, " {\n");
    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        mir_print_block(bb, out);
    }
    fprintf(out, "}\n\n");
}

/* ================================================================
 * Module Printer
 * ================================================================ */

void mir_print_module(MirModule* module, FILE* out) {
    fprintf(out, "; Casprix MIR Module: %s\n", module->name);
    fprintf(out, "; Functions: %d\n", module->func_count);
    fprintf(out, "; Globals: %d\n", module->global_count);
    fprintf(out, "; Strings: %d\n\n", module->string_count);

    /* String literal table */
    for (int i = 0; i < module->string_count; i++) {
        fprintf(out, "@str.%d = \"%s\"\n", i, module->string_literals[i]);
    }
    if (module->string_count > 0) fprintf(out, "\n");

    /* Global variables */
    for (int i = 0; i < module->global_count; i++) {
        fprintf(out, "@%s = %s ",
                module->globals[i].name,
                module->globals[i].is_const ? "const" : "global");
        mir_print_type(module->globals[i].type, out);
        fprintf(out, "\n");
    }
    if (module->global_count > 0) fprintf(out, "\n");

    /* Functions */
    for (MirFunction* f = module->func_list; f; f = f->next_func) {
        mir_print_function(f, out);
    }
}

/* ================================================================
 * Validator
 * ================================================================ */

static bool is_terminator_opcode(MirOpcode opcode) {
    return opcode == MIR_BR || opcode == MIR_CONDBR || opcode == MIR_SWITCH ||
           opcode == MIR_RET || opcode == MIR_RET_VOID || opcode == MIR_UNREACHABLE;
}

static bool block_has_predecessor(MirBlock* block, MirBlock* pred) {
    for (int i = 0; i < block->pred_count; i++) {
        if (block->predecessors[i] == pred) {
            return true;
        }
    }
    return false;
}

static bool block_has_successor(MirBlock* block, MirBlock* succ) {
    for (int i = 0; i < block->succ_count; i++) {
        if (block->successors[i] == succ) {
            return true;
        }
    }
    return false;
}

static bool block_dominates(MirBlock* dom, MirBlock* block) {
    MirBlock* cursor = block;

    if (!dom || !block) {
        return false;
    }

    while (cursor) {
        if (cursor == dom) {
            return true;
        }
        if (cursor->idom == cursor) {
            break;
        }
        cursor = cursor->idom;
    }

    return dom == cursor;
}

static bool validate_block(MirBlock* block, int* errors) {
    bool ok = true;
    int terminators = 0;

    /* Every block must be terminated */
    if (!mir_block_is_terminated(block)) {
        fprintf(stderr, "MIR VALIDATE: bb%u (%s) is not terminated\n",
                block->id, block->label ? block->label : "?");
        (*errors)++;
        ok = false;
    }

    /* Phi nodes must be at the beginning of the block */
    bool past_phis = false;
    for (MirInst* inst = block->first; inst; inst = inst->next) {
        if (inst->opcode == MIR_PHI) {
            if (past_phis) {
                fprintf(stderr, "MIR VALIDATE: bb%u has phi after non-phi instruction\n",
                        block->id);
                (*errors)++;
                ok = false;
            }
        } else {
            past_phis = true;
        }

        /* Terminators must be the last instruction */
        bool is_term = is_terminator_opcode(inst->opcode);
        if (is_term) {
            terminators++;
        }
        if (is_term && inst->next != NULL) {
            fprintf(stderr, "MIR VALIDATE: bb%u has instructions after terminator\n",
                    block->id);
            (*errors)++;
            ok = false;
        }
    }

    if (terminators > 1) {
        fprintf(stderr, "MIR VALIDATE: bb%u has multiple terminators\n", block->id);
        (*errors)++;
        ok = false;
    }

    /* Phi edge count should match predecessor count */
    for (MirInst* inst = block->first; inst && inst->opcode == MIR_PHI; inst = inst->next) {
        if (inst->as.phi.n_edges != block->pred_count) {
            fprintf(stderr, "MIR VALIDATE: bb%u phi %%v%u has %d edges but %d predecessors\n",
                    block->id, inst->result, inst->as.phi.n_edges, block->pred_count);
            (*errors)++;
            ok = false;
        }
        for (int i = 0; i < inst->as.phi.n_edges; i++) {
            MirBlock* pred = inst->as.phi.edges[i].block;
            if (!pred || !block_has_predecessor(block, pred)) {
                fprintf(stderr,
                        "MIR VALIDATE: bb%u phi %%v%u references non-predecessor block\n",
                        block->id, inst->result);
                (*errors)++;
                ok = false;
                continue;
            }
            for (int j = i + 1; j < inst->as.phi.n_edges; j++) {
                if (inst->as.phi.edges[j].block == pred) {
                    fprintf(stderr,
                            "MIR VALIDATE: bb%u phi %%v%u has duplicate incoming block bb%u\n",
                            block->id, inst->result, pred->id);
                    (*errors)++;
                    ok = false;
                }
            }
        }
    }

    return ok;
}

bool mir_validate_function(MirFunction* func) {
    MirBlock** block_by_id = NULL;
    MirBlock** value_block = NULL;
    int* value_order = NULL;
    int* def_count = NULL;
    int errors = 0;
    bool ok = true;
    int next_value_limit;
    int order;

    if (!func) {
        return false;
    }

    if (!func->entry_block) {
        fprintf(stderr, "MIR VALIDATE: function @%s has no entry block\n", func->name);
        return false;
    }

    if (func->block_count <= 0) {
        fprintf(stderr, "MIR VALIDATE: @%s has no basic blocks\n", func->name);
        return false;
    }

    block_by_id = (MirBlock**)calloc((size_t)func->block_count, sizeof(MirBlock*));
    next_value_limit = (int)func->next_value_id;
    if (next_value_limit < 1) {
        fprintf(stderr, "MIR VALIDATE: @%s has invalid next_value_id=%u\n",
                func->name, func->next_value_id);
        errors++;
        next_value_limit = 1;
    }
    value_block = (MirBlock**)calloc((size_t)next_value_limit, sizeof(MirBlock*));
    value_order = (int*)calloc((size_t)next_value_limit, sizeof(int));
    def_count = (int*)calloc((size_t)next_value_limit, sizeof(int));
    if (!block_by_id || !value_block || !value_order || !def_count) {
        fprintf(stderr, "MIR VALIDATE: allocation failure validating @%s\n", func->name);
        free(block_by_id);
        free(value_block);
        free(value_order);
        free(def_count);
        return false;
    }

    /* Entry block must have no predecessors */
    if (func->entry_block->pred_count > 0) {
        fprintf(stderr, "MIR VALIDATE: @%s entry block has predecessors\n", func->name);
        errors++;
    }

    /* Collect blocks and validate CFG symmetry. */
    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        if (bb->id >= (uint32_t)func->block_count) {
            fprintf(stderr, "MIR VALIDATE: @%s has block id bb%u outside block_count=%d\n",
                    func->name, bb->id, func->block_count);
            errors++;
            continue;
        }
        if (block_by_id[bb->id]) {
            fprintf(stderr, "MIR VALIDATE: @%s defines duplicate block id bb%u\n",
                    func->name, bb->id);
            errors++;
            continue;
        }
        block_by_id[bb->id] = bb;

        for (int i = 0; i < bb->pred_count; i++) {
            MirBlock* pred = bb->predecessors[i];
            if (!pred || !block_has_successor(pred, bb)) {
                fprintf(stderr,
                        "MIR VALIDATE: predecessor/successor mismatch for bb%u <- bb%u\n",
                        bb->id, pred ? pred->id : 0);
                errors++;
            }
        }
        for (int i = 0; i < bb->succ_count; i++) {
            MirBlock* succ = bb->successors[i];
            if (!succ || !block_has_predecessor(succ, bb)) {
                fprintf(stderr,
                        "MIR VALIDATE: predecessor/successor mismatch for bb%u -> bb%u\n",
                        bb->id, succ ? succ->id : 0);
                errors++;
            }
        }
        validate_block(bb, &errors);
    }

    /* Parameters are SSA definitions available from the entry block. */
    for (int i = 0; i < func->param_count; i++) {
        MirValueId id = func->params[i].value_id;
        if (id == MIR_VALUE_NONE || (int)id >= next_value_limit) {
            fprintf(stderr, "MIR VALIDATE: @%s parameter %d has invalid value id %u\n",
                    func->name, i, id);
            errors++;
            continue;
        }
        def_count[id]++;
        value_block[id] = func->entry_block;
        value_order[id] = -1;
    }

    /* Record SSA definitions and validate type-table bounds. */
    order = 0;
    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            order++;
            if (inst->result == MIR_VALUE_NONE) {
                continue;
            }
            if ((int)inst->result >= next_value_limit) {
                fprintf(stderr, "MIR VALIDATE: @%s instruction defines out-of-range value %%v%u\n",
                        func->name, inst->result);
                errors++;
                continue;
            }
            def_count[inst->result]++;
            value_block[inst->result] = bb;
            value_order[inst->result] = order;
            if ((int)inst->result >= func->value_type_capacity) {
                fprintf(stderr,
                        "MIR VALIDATE: @%s result %%v%u has no value_types entry (capacity=%d)\n",
                        func->name, inst->result, func->value_type_capacity);
                errors++;
            }
        }
    }

    for (int i = 1; i < next_value_limit; i++) {
        if (def_count[i] > 1) {
            fprintf(stderr, "MIR VALIDATE: @%s has duplicate SSA definition for %%v%d\n",
                    func->name, i);
            errors++;
        }
    }

    /* Refresh dominators for reachable-block dominance checks. */
    mir_compute_dominators(func);

    /* Validate operand definitions and dominance. */
    order = 0;
    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            MirValueId operands[16];
            int operand_count = 0;

            order++;
            memset(operands, 0, sizeof(operands));
            switch (inst->opcode) {
                case MIR_ADD: case MIR_SUB: case MIR_MUL: case MIR_DIV: case MIR_MOD:
                case MIR_FADD: case MIR_FSUB: case MIR_FMUL: case MIR_FDIV:
                case MIR_BAND: case MIR_BOR: case MIR_BXOR: case MIR_SHL: case MIR_SHR:
                case MIR_USHR:
                case MIR_CMP_EQ: case MIR_CMP_NE: case MIR_CMP_LT: case MIR_CMP_LE:
                case MIR_CMP_GT: case MIR_CMP_GE:
                case MIR_LOGIC_AND: case MIR_LOGIC_OR:
                    operands[operand_count++] = inst->as.binary.lhs;
                    operands[operand_count++] = inst->as.binary.rhs;
                    break;
                case MIR_NEG: case MIR_FNEG: case MIR_BNOT: case MIR_LOGIC_NOT:
                case MIR_CAST: case MIR_BITCAST: case MIR_TRUNC:
                case MIR_ZEXT: case MIR_SEXT: case MIR_SITOFP: case MIR_FPTOSI:
                    operands[operand_count++] = inst->as.unary.operand;
                    break;
                case MIR_LOAD:
                    operands[operand_count++] = inst->as.mem.ptr;
                    break;
                case MIR_STORE:
                    operands[operand_count++] = inst->as.mem.ptr;
                    operands[operand_count++] = inst->as.mem.value;
                    break;
                case MIR_GET_FIELD_PTR:
                case MIR_GET_ELEM_PTR:
                    operands[operand_count++] = inst->as.gep.base;
                    if (inst->opcode == MIR_GET_ELEM_PTR) {
                        operands[operand_count++] = inst->as.gep.index;
                    }
                    break;
                case MIR_CONDBR:
                    operands[operand_count++] = inst->as.condbr.cond;
                    break;
                case MIR_SWITCH:
                    operands[operand_count++] = inst->as.sw.discriminant;
                    break;
                case MIR_RET:
                    operands[operand_count++] = inst->as.ret.value;
                    break;
                case MIR_CALL:
                case MIR_CALL_INDIRECT:
                    if (inst->opcode == MIR_CALL_INDIRECT) {
                        operands[operand_count++] = inst->as.call.callee;
                    }
                    for (int i = 0; i < inst->as.call.n_args && operand_count < 16; i++) {
                        operands[operand_count++] = inst->as.call.args[i];
                    }
                    break;
                case MIR_CALL_VIRTUAL:
                    operands[operand_count++] = inst->as.vcall.self_obj;
                    for (int i = 0; i < inst->as.vcall.n_args && operand_count < 16; i++) {
                        operands[operand_count++] = inst->as.vcall.args[i];
                    }
                    break;
                case MIR_COPY: case MIR_BORROW: case MIR_BORROW_MUT: case MIR_MOVE:
                    operands[operand_count++] = inst->as.transfer.source;
                    break;
                case MIR_ARC_RETAIN: case MIR_ARC_RELEASE: case MIR_DROP:
                    operands[operand_count++] = inst->as.refop.ptr;
                    break;
                case MIR_STRUCT_INIT:
                    for (int i = 0; i < inst->as.struct_init.n_fields && operand_count < 16; i++) {
                        operands[operand_count++] = inst->as.struct_init.fields[i];
                    }
                    break;
                case MIR_EXTRACT:
                    operands[operand_count++] = inst->as.field_op.aggregate;
                    break;
                case MIR_INSERT:
                    operands[operand_count++] = inst->as.field_op.aggregate;
                    operands[operand_count++] = inst->as.field_op.insert_val;
                    break;
                case MIR_PHI:
                    for (int i = 0; i < inst->as.phi.n_edges; i++) {
                        MirValueId value = inst->as.phi.edges[i].value;
                        MirBlock* pred = inst->as.phi.edges[i].block;
                        MirBlock* def_block;

                        if (value == MIR_VALUE_NONE) {
                            continue;
                        }
                        if ((int)value >= next_value_limit || def_count[value] == 0) {
                            fprintf(stderr,
                                    "MIR VALIDATE: @%s phi %%v%u uses invalid value %%v%u\n",
                                    func->name, inst->result, value);
                            errors++;
                            continue;
                        }
                        def_block = value_block[value];
                        if (!block_dominates(def_block, pred)) {
                            fprintf(stderr,
                                    "MIR VALIDATE: @%s phi %%v%u has non-dominating incoming %%v%u from bb%u\n",
                                    func->name, inst->result, value, pred ? pred->id : 0);
                            errors++;
                        }
                    }
                    operand_count = 0;
                    break;
                default:
                    break;
            }

            for (int i = 0; i < operand_count; i++) {
                MirValueId value = operands[i];
                MirBlock* def_block;

                if (value == MIR_VALUE_NONE) {
                    continue;
                }
                if ((int)value >= next_value_limit || def_count[value] == 0) {
                    fprintf(stderr, "MIR VALIDATE: @%s uses invalid value %%v%u in bb%u\n",
                            func->name, value, bb->id);
                    errors++;
                    continue;
                }

                def_block = value_block[value];
                if (!def_block) {
                    fprintf(stderr, "MIR VALIDATE: @%s missing definition block for %%v%u\n",
                            func->name, value);
                    errors++;
                    continue;
                }

                if (def_block == bb) {
                    if (value_order[value] >= order) {
                        fprintf(stderr,
                                "MIR VALIDATE: @%s uses %%v%u before its definition in bb%u\n",
                                func->name, value, bb->id);
                        errors++;
                    }
                } else if (!block_dominates(def_block, bb)) {
                    fprintf(stderr,
                            "MIR VALIDATE: @%s uses non-dominating value %%v%u in bb%u\n",
                            func->name, value, bb->id);
                    errors++;
                }
            }
        }
    }

    if (errors > 0) {
        fprintf(stderr, "MIR VALIDATE: @%s has %d error(s)\n", func->name, errors);
        ok = false;
    }

    free(block_by_id);
    free(value_block);
    free(value_order);
    free(def_count);
    return ok && errors == 0;
}

bool mir_validate_module(MirModule* module) {
    bool ok = true;
    for (MirFunction* f = module->func_list; f; f = f->next_func) {
        if (!f->is_extern) {
            if (!mir_validate_function(f)) ok = false;
        }
    }
    return ok;
}
