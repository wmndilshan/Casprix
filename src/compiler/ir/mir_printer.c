/*
 * Casprix Compiler — MIR Printer & Validator
 *
 * Textual IR dump (similar to LLVM IR format) for debugging and testing.
 * Structural validation checks SSA dominance, block termination, phi correctness.
 */

#include "mir.h"
#include <stdio.h>
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
        case MIR_SUSPEND:
            fprintf(out, "suspend bb%u, ", inst->as.suspend.resume_bb->id);
            print_value(inst->as.suspend.future, out);
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
    if (func->is_async) fprintf(out, " async");
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

static bool validate_block(MirBlock* block, int* errors) {
    bool ok = true;

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
        bool is_term = (inst->opcode == MIR_BR || inst->opcode == MIR_CONDBR ||
                        inst->opcode == MIR_SWITCH || inst->opcode == MIR_RET ||
                        inst->opcode == MIR_RET_VOID || inst->opcode == MIR_UNREACHABLE);
        if (is_term && inst->next != NULL) {
            fprintf(stderr, "MIR VALIDATE: bb%u has instructions after terminator\n",
                    block->id);
            (*errors)++;
            ok = false;
        }
    }

    /* Phi edge count should match predecessor count */
    for (MirInst* inst = block->first; inst && inst->opcode == MIR_PHI; inst = inst->next) {
        if (inst->as.phi.n_edges != block->pred_count) {
            fprintf(stderr, "MIR VALIDATE: bb%u phi %%v%u has %d edges but %d predecessors\n",
                    block->id, inst->result, inst->as.phi.n_edges, block->pred_count);
            (*errors)++;
            ok = false;
        }
    }

    return ok;
}

bool mir_validate_function(MirFunction* func) {
    int errors = 0;

    if (!func->entry_block) {
        fprintf(stderr, "MIR VALIDATE: function @%s has no entry block\n", func->name);
        return false;
    }

    /* Entry block must have no predecessors */
    if (func->entry_block->pred_count > 0) {
        fprintf(stderr, "MIR VALIDATE: @%s entry block has predecessors\n", func->name);
        errors++;
    }

    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        validate_block(bb, &errors);
    }

    if (errors > 0) {
        fprintf(stderr, "MIR VALIDATE: @%s has %d error(s)\n", func->name, errors);
    }
    return errors == 0;
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
