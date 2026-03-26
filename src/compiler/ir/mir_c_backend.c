#include "mir_backend.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FILE* out;
    MirModule* module;
    MirFunction* current_func;
    MirBlock* current_block;
    bool had_error;
    bool has_entry;
} CBackendData;

static bool c_is_pointer_like(MirType* type) {
    if (!type) return false;

    switch (type->kind) {
        case MIR_TYPE_PTR:
        case MIR_TYPE_REF:
        case MIR_TYPE_MUT_REF:
        case MIR_TYPE_FUNC:
        case MIR_TYPE_STRUCT:
        case MIR_TYPE_ARRAY:
        case MIR_TYPE_SLICE:
        case MIR_TYPE_AGGREGATE:
            return true;
        default:
            return false;
    }
}

static void c_type_name(MirType* type, char* buf, size_t buf_size) {
    if (!type) {
        snprintf(buf, buf_size, "void");
        return;
    }

    switch (type->kind) {
        case MIR_TYPE_VOID:  snprintf(buf, buf_size, "void"); break;
        case MIR_TYPE_BOOL:  snprintf(buf, buf_size, "bool"); break;
        case MIR_TYPE_I8:    snprintf(buf, buf_size, "int8_t"); break;
        case MIR_TYPE_I16:   snprintf(buf, buf_size, "int16_t"); break;
        case MIR_TYPE_I32:   snprintf(buf, buf_size, "int32_t"); break;
        case MIR_TYPE_I64:   snprintf(buf, buf_size, "int64_t"); break;
        case MIR_TYPE_U8:    snprintf(buf, buf_size, "uint8_t"); break;
        case MIR_TYPE_U16:   snprintf(buf, buf_size, "uint16_t"); break;
        case MIR_TYPE_U32:   snprintf(buf, buf_size, "uint32_t"); break;
        case MIR_TYPE_U64:   snprintf(buf, buf_size, "uint64_t"); break;
        case MIR_TYPE_F32:   snprintf(buf, buf_size, "float"); break;
        case MIR_TYPE_F64:   snprintf(buf, buf_size, "double"); break;
        case MIR_TYPE_FUNC:  snprintf(buf, buf_size, "void*"); break;
        default:             snprintf(buf, buf_size, "void*"); break;
    }
}

static void c_global_symbol_name(MirModule* module, const char* name,
                                 char* buf, size_t buf_size) {
    int idx = mir_module_find_global(module, name);
    size_t pos;

    if (idx < 0) idx = 0;

    pos = (size_t)snprintf(buf, buf_size, "cpx_g_%d_", idx);
    if (pos >= buf_size) {
        buf[buf_size - 1] = '\0';
        return;
    }

    while (*name && pos + 1 < buf_size) {
        unsigned char ch = (unsigned char)*name++;
        buf[pos++] = (char)(isalnum(ch) || ch == '_' ? ch : '_');
    }
    buf[pos] = '\0';
}

static void c_emit_global_decls(FILE* out, MirModule* module) {
    for (int i = 0; i < module->global_count; i++) {
        char type_buf[64];
        char symbol_buf[128];
        MirType* type = module->globals[i].type;

        c_type_name(type, type_buf, sizeof(type_buf));
        c_global_symbol_name(module, module->globals[i].name, symbol_buf, sizeof(symbol_buf));

        if (c_is_pointer_like(type)) {
            fprintf(out, "static %s %s = NULL;\n", type_buf, symbol_buf);
        } else if (type && type->kind == MIR_TYPE_BOOL) {
            fprintf(out, "static bool %s = false;\n", symbol_buf);
        } else {
            fprintf(out, "static %s %s = 0;\n", type_buf, symbol_buf);
        }
    }

    if (module->global_count > 0) {
        fputc('\n', out);
    }
}

static void c_emit_func_pointer_type(FILE* out, MirType* func_type,
                                     MirInst* inst, MirFunction* func) {
    MirType* ret_type = (func_type && func_type->kind == MIR_TYPE_FUNC)
        ? func_type->as.func.ret
        : inst->type;
    char ret_buf[64];

    c_type_name(ret_type, ret_buf, sizeof(ret_buf));
    fprintf(out, "(%s (*)(", ret_buf);

    if (func_type && func_type->kind == MIR_TYPE_FUNC) {
        if (func_type->as.func.n_params == 0) {
            fputs("void", out);
        } else {
            for (int i = 0; i < func_type->as.func.n_params; i++) {
                char param_buf[64];
                c_type_name(func_type->as.func.params[i], param_buf, sizeof(param_buf));
                if (i > 0) fputs(", ", out);
                fputs(param_buf, out);
            }
        }
    } else if (inst->as.call.n_args == 0) {
        fputs("void", out);
    } else {
        for (int i = 0; i < inst->as.call.n_args; i++) {
            MirType* arg_type = mir_function_value_type(func, inst->as.call.args[i]);
            char arg_buf[64];
            c_type_name(arg_type, arg_buf, sizeof(arg_buf));
            if (i > 0) fputs(", ", out);
            fputs(arg_buf, out);
        }
    }

    fputs("))", out);
}

static void c_emit_escaped_string(FILE* out, const char* text) {
    const unsigned char* p = (const unsigned char*)(text ? text : "");
    fputc('"', out);
    while (*p) {
        unsigned char c = *p++;
        switch (c) {
            case '\\': fputs("\\\\", out); break;
            case '"':  fputs("\\\"", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:
                if (c < 32 || c > 126) fprintf(out, "\\x%02X", c);
                else fputc((int)c, out);
                break;
        }
    }
    fputc('"', out);
}

static void c_emit_signature(FILE* out, const char* prefix, MirFunction* func) {
    char ret_buf[64];
    c_type_name(func->return_type, ret_buf, sizeof(ret_buf));
    fprintf(out, "%s %s %s(", prefix, ret_buf, func->name);
    for (int i = 0; i < func->param_count; i++) {
        char param_buf[64];
        c_type_name(func->params[i].type, param_buf, sizeof(param_buf));
        if (i > 0) fputs(", ", out);
        fprintf(out, "%s arg%d", param_buf, i);
    }
    fputs(")", out);
}

static MirInst* c_find_inst_by_result(MirFunction* func, MirValueId id) {
    for (MirBlock* block = func->block_list; block; block = block->next_block) {
        for (MirInst* inst = block->first; inst; inst = inst->next) {
            if (inst->result == id) return inst;
        }
    }
    return NULL;
}

static void c_emit_decl_for_value(FILE* out, MirFunction* func, MirValueId id) {
    MirInst* inst = c_find_inst_by_result(func, id);
    if (!inst || inst->result == MIR_VALUE_NONE || !inst->type) return;

    if (inst->opcode == MIR_ALLOCA) {
        char ptr_buf[64];
        int bytes = mir_type_size(inst->as.alloca.alloc_type);
        int words;
        if (bytes <= 0) bytes = 8;
        words = (bytes + 7) / 8;
        if (words <= 0) words = 1;
        c_type_name(inst->type, ptr_buf, sizeof(ptr_buf));
        fprintf(out, "    uint64_t cpx_v%u_storage[%d] = {0};\n", id, words);
        fprintf(out, "    %s cpx_v%u = (%s)cpx_v%u_storage;\n", ptr_buf, id, ptr_buf, id);
        return;
    }

    {
        char type_buf[64];
        c_type_name(inst->type, type_buf, sizeof(type_buf));
        if (c_is_pointer_like(inst->type)) fprintf(out, "    %s cpx_v%u = NULL;\n", type_buf, id);
        else if (inst->type->kind == MIR_TYPE_BOOL) fprintf(out, "    bool cpx_v%u = false;\n", id);
        else fprintf(out, "    %s cpx_v%u = 0;\n", type_buf, id);
    }
}

static void c_emit_locals(FILE* out, MirFunction* func) {
    fprintf(out, "    int cpx_pred = -1;\n");

    for (int i = 0; i < func->param_count; i++) {
        char type_buf[64];
        c_type_name(func->params[i].type, type_buf, sizeof(type_buf));
        fprintf(out, "    %s cpx_v%u = arg%d;\n", type_buf, func->params[i].value_id, i);
    }

    for (MirValueId id = 1; id < func->next_value_id; id++) {
        bool is_param = false;
        for (int i = 0; i < func->param_count; i++) {
            if (func->params[i].value_id == id) {
                is_param = true;
                break;
            }
        }
        if (!is_param) c_emit_decl_for_value(out, func, id);
    }
}

static void c_emit_value(FILE* out, MirFunction* func, MirValueId id, MirType* expected_type) {
    MirType* actual_type;
    char expected_buf[64];
    char actual_buf[64];

    if (id == MIR_VALUE_NONE) {
        fputs("0", out);
        return;
    }

    actual_type = mir_function_value_type(func, id);
    if (!expected_type || !actual_type) {
        fprintf(out, "cpx_v%u", id);
        return;
    }

    c_type_name(expected_type, expected_buf, sizeof(expected_buf));
    c_type_name(actual_type, actual_buf, sizeof(actual_buf));

    if (c_is_pointer_like(actual_type) && !c_is_pointer_like(expected_type)) {
        fprintf(out, "(*((%s*)cpx_v%u))", expected_buf, id);
        return;
    }

    if (!c_is_pointer_like(actual_type) && c_is_pointer_like(expected_type)) {
        fprintf(out, "((%s)(intptr_t)cpx_v%u)", expected_buf, id);
        return;
    }

    if (!c_is_pointer_like(actual_type) && !c_is_pointer_like(expected_type) && strcmp(expected_buf, actual_buf) != 0) {
        fprintf(out, "((%s)cpx_v%u)", expected_buf, id);
        return;
    }

    fprintf(out, "cpx_v%u", id);
}

static void c_emit_binary(CBackendData* data, MirInst* inst, const char* op) {
    char type_buf[64];
    MirType* lhs_type = mir_function_value_type(data->current_func, inst->as.binary.lhs);
    MirType* rhs_type = mir_function_value_type(data->current_func, inst->as.binary.rhs);
    c_type_name(inst->type, type_buf, sizeof(type_buf));
    fprintf(data->out, "    cpx_v%u = (", inst->result);
    fprintf(data->out, "%s)(", type_buf);
    c_emit_value(data->out, data->current_func, inst->as.binary.lhs, lhs_type);
    fprintf(data->out, " %s ", op);
    c_emit_value(data->out, data->current_func, inst->as.binary.rhs, rhs_type);
    fprintf(data->out, ");\n");
}

static void c_emit_phi_assignments(CBackendData* data, MirBlock* block) {
    for (MirInst* inst = block->first; inst && inst->opcode == MIR_PHI; inst = inst->next) {
        for (int i = 0; i < inst->as.phi.n_edges; i++) {
            if (i == 0) fprintf(data->out, "    if (cpx_pred == %u) cpx_v%u = ", inst->as.phi.edges[i].block->id, inst->result);
            else fprintf(data->out, "    else if (cpx_pred == %u) cpx_v%u = ", inst->as.phi.edges[i].block->id, inst->result);
            c_emit_value(data->out, data->current_func, inst->as.phi.edges[i].value, inst->type);
            fprintf(data->out, ";\n");
        }
    }
}

static bool c_begin_module(MirBackend* self, MirModule* module) {
    CBackendData* data = (CBackendData*)self->data;
    data->module = module;
    data->has_entry = false;
    data->had_error = false;
    data->out = fopen(self->config.output_path, "w");
    if (!data->out) return false;

    fprintf(data->out, "#include <stdbool.h>\n");
    fprintf(data->out, "#include <stdint.h>\n");
    fprintf(data->out, "#include <stdlib.h>\n");
    fprintf(data->out, "#include <string.h>\n\n");
    fprintf(data->out, "typedef struct AndroidApp AndroidApp;\n");
    fprintf(data->out, "void android_app_bind_current(AndroidApp* app);\n\n");
    fprintf(data->out, "static void* cpx_obj_alloc(size_t size) { return calloc(1, size); }\n\n");
    c_emit_global_decls(data->out, module);

    for (MirFunction* func = module->func_list; func; func = func->next_func) {
        c_emit_signature(data->out, func->is_extern ? "extern" : "static", func);
        fprintf(data->out, ";\n");
        if (strcmp(func->name, "__casprix_entry") == 0) data->has_entry = true;
    }
    fprintf(data->out, "\n");
    return true;
}

static void c_end_module(MirBackend* self) {
    CBackendData* data = (CBackendData*)self->data;
    if (!data->out) return;

    if (data->has_entry) {
        fprintf(data->out, "void cpx_app_main(AndroidApp* app) {\n");
        fprintf(data->out, "    android_app_bind_current(app);\n");
        fprintf(data->out, "    __casprix_entry();\n");
        fprintf(data->out, "}\n");
    }
}

static bool c_begin_function(MirBackend* self, MirFunction* func) {
    CBackendData* data = (CBackendData*)self->data;
    data->current_func = func;
    if (func->is_extern) return true;

    c_emit_signature(data->out, "static", func);
    fprintf(data->out, " {\n");
    c_emit_locals(data->out, func);
    fprintf(data->out, "    goto bb%u;\n\n", func->entry_block ? func->entry_block->id : 0);
    return true;
}

static void c_end_function(MirBackend* self) {
    CBackendData* data = (CBackendData*)self->data;
    if (!data->current_func || data->current_func->is_extern) return;
    fprintf(data->out, "}\n\n");
    data->current_func = NULL;
    data->current_block = NULL;
}

static void c_emit_block_label(MirBackend* self, MirBlock* block) {
    CBackendData* data = (CBackendData*)self->data;
    if (!data->current_func || data->current_func->is_extern) return;
    data->current_block = block;
    fprintf(data->out, "bb%u:\n", block->id);
    fprintf(data->out, "    (void)0;\n");
    c_emit_phi_assignments(data, block);
}

static void c_emit_inst(MirBackend* self, MirInst* inst, MirFunction* func) {
    CBackendData* data = (CBackendData*)self->data;
    MirFunction* callee;
    MirType* value_type;
    char type_buf[64];

    if (!data->out || !func || func->is_extern) return;
    if (inst->opcode == MIR_PHI) return;

    switch (inst->opcode) {
        case MIR_CONST_INT:
            c_type_name(inst->type, type_buf, sizeof(type_buf));
            fprintf(data->out, "    cpx_v%u = (%s)%lld;\n", inst->result,
                    type_buf,
                    (long long)inst->as.imm_i64);
            break;
        case MIR_CONST_FLOAT:
            c_type_name(inst->type, type_buf, sizeof(type_buf));
            fprintf(data->out, "    cpx_v%u = (%s)%.*g;\n", inst->result, type_buf, 17, inst->as.imm_f64);
            break;
        case MIR_CONST_BOOL:
            fprintf(data->out, "    cpx_v%u = %s;\n", inst->result, inst->as.imm_bool ? "true" : "false");
            break;
        case MIR_CONST_STRING:
            fprintf(data->out, "    cpx_v%u = (void*)", inst->result);
            c_emit_escaped_string(data->out, inst->as.imm_string);
            fprintf(data->out, ";\n");
            break;
        case MIR_CONST_FUNC:
            fprintf(data->out, "    cpx_v%u = (void*)&%s;\n", inst->result,
                    inst->as.imm_string ? inst->as.imm_string : "cpx_unknown_func");
            break;
        case MIR_CONST_NULL:
            fprintf(data->out, "    cpx_v%u = NULL;\n", inst->result);
            break;
        case MIR_GLOBAL_ADDR: {
            char symbol_buf[128];
            c_global_symbol_name(data->module,
                                 inst->as.global_name ? inst->as.global_name : "",
                                 symbol_buf, sizeof(symbol_buf));
            fprintf(data->out, "    cpx_v%u = (void*)&%s;\n", inst->result, symbol_buf);
            break;
        }
        case MIR_ALLOCA:
            fprintf(data->out, "    /* alloca cpx_v%u */\n", inst->result);
            break;
        case MIR_STORE:
            value_type = mir_function_value_type(func, inst->as.mem.value);
            c_type_name(value_type, type_buf, sizeof(type_buf));
            if (!value_type || strcmp(type_buf, "void") == 0) {
                fprintf(data->out, "    *((uintptr_t*)");
                c_emit_value(data->out, func, inst->as.mem.ptr, NULL);
                fprintf(data->out, ") = (uintptr_t)");
                c_emit_value(data->out, func, inst->as.mem.value, NULL);
                fprintf(data->out, ";\n");
                break;
            }
            fprintf(data->out, "    *((%s*)", type_buf);
            c_emit_value(data->out, func, inst->as.mem.ptr, NULL);
            fprintf(data->out, ") = ");
            c_emit_value(data->out, func, inst->as.mem.value, value_type);
            fprintf(data->out, ";\n");
            break;
        case MIR_LOAD:
            c_type_name(inst->type, type_buf, sizeof(type_buf));
            fprintf(data->out, "    cpx_v%u = *((%s*)", inst->result, type_buf);
            c_emit_value(data->out, func, inst->as.mem.ptr, NULL);
            fprintf(data->out, ");\n");
            break;
        case MIR_GET_FIELD_PTR:
            fprintf(data->out, "    cpx_v%u = (void*)((uint8_t*)", inst->result);
            c_emit_value(data->out, func, inst->as.gep.base, NULL);
            fprintf(data->out, " + (%d * 8));\n", inst->as.gep.field_index);
            break;
        case MIR_GET_ELEM_PTR: {
            MirType* base_type = mir_function_value_type(func, inst->as.gep.base);
            MirType* elem_type = (base_type && base_type->kind == MIR_TYPE_PTR) ? base_type->as.ptr.pointee : NULL;
            int elem_size = mir_type_size(elem_type);
            if (elem_size <= 0) elem_size = 8;
            fprintf(data->out, "    cpx_v%u = (void*)((uint8_t*)", inst->result);
            c_emit_value(data->out, func, inst->as.gep.base, NULL);
            fprintf(data->out, " + (");
            c_emit_value(data->out, func, inst->as.gep.index, NULL);
            fprintf(data->out, " * %d));\n", elem_size);
            break;
        }
        case MIR_OBJ_ALLOC:
            fprintf(data->out, "    cpx_v%u = cpx_obj_alloc(%d);\n", inst->result, inst->as.obj_alloc.size);
            break;
        case MIR_CALL:
            callee = mir_module_find_function(data->module, inst->as.call.func_name);
            if (inst->result != MIR_VALUE_NONE) {
                fprintf(data->out, "    cpx_v%u = ", inst->result);
            } else {
                fprintf(data->out, "    ");
            }
            fprintf(data->out, "%s(", inst->as.call.func_name ? inst->as.call.func_name : "cpx_unknown_call");
            for (int i = 0; i < inst->as.call.n_args; i++) {
                MirType* expected = (callee && i < callee->param_count) ? callee->params[i].type : NULL;
                if (i > 0) fprintf(data->out, ", ");
                c_emit_value(data->out, func, inst->as.call.args[i], expected);
            }
            fprintf(data->out, ");\n");
            break;
        case MIR_CALL_INDIRECT: {
            MirType* callee_type = mir_function_value_type(func, inst->as.call.callee);
            if (inst->result != MIR_VALUE_NONE) {
                fprintf(data->out, "    cpx_v%u = ", inst->result);
            } else {
                fprintf(data->out, "    ");
            }
            fputc('(', data->out);
            c_emit_func_pointer_type(data->out, callee_type, inst, func);
            c_emit_value(data->out, func, inst->as.call.callee, NULL);
            fprintf(data->out, ")(");
            for (int i = 0; i < inst->as.call.n_args; i++) {
                MirType* expected = (callee_type && callee_type->kind == MIR_TYPE_FUNC &&
                                     i < callee_type->as.func.n_params)
                    ? callee_type->as.func.params[i]
                    : NULL;
                if (i > 0) fprintf(data->out, ", ");
                c_emit_value(data->out, func, inst->as.call.args[i], expected);
            }
            fprintf(data->out, ");\n");
            break;
        }
        case MIR_ADD:      c_emit_binary(data, inst, "+"); break;
        case MIR_SUB:      c_emit_binary(data, inst, "-"); break;
        case MIR_MUL:      c_emit_binary(data, inst, "*"); break;
        case MIR_DIV:      c_emit_binary(data, inst, "/"); break;
        case MIR_MOD:      c_emit_binary(data, inst, "%"); break;
        case MIR_FADD:     c_emit_binary(data, inst, "+"); break;
        case MIR_FSUB:     c_emit_binary(data, inst, "-"); break;
        case MIR_FMUL:     c_emit_binary(data, inst, "*"); break;
        case MIR_FDIV:     c_emit_binary(data, inst, "/"); break;
        case MIR_BAND:     c_emit_binary(data, inst, "&"); break;
        case MIR_BOR:      c_emit_binary(data, inst, "|"); break;
        case MIR_BXOR:     c_emit_binary(data, inst, "^"); break;
        case MIR_SHL:      c_emit_binary(data, inst, "<<"); break;
        case MIR_SHR:
        case MIR_USHR:     c_emit_binary(data, inst, ">>"); break;
        case MIR_LOGIC_AND:c_emit_binary(data, inst, "&&"); break;
        case MIR_LOGIC_OR: c_emit_binary(data, inst, "||"); break;
        case MIR_CMP_EQ:   c_emit_binary(data, inst, "=="); break;
        case MIR_CMP_NE:   c_emit_binary(data, inst, "!="); break;
        case MIR_CMP_LT:   c_emit_binary(data, inst, "<"); break;
        case MIR_CMP_LE:   c_emit_binary(data, inst, "<="); break;
        case MIR_CMP_GT:   c_emit_binary(data, inst, ">"); break;
        case MIR_CMP_GE:   c_emit_binary(data, inst, ">="); break;
        case MIR_NEG:
        case MIR_FNEG:
            c_type_name(inst->type, type_buf, sizeof(type_buf));
            fprintf(data->out, "    cpx_v%u = (%s)(-", inst->result, type_buf);
            c_emit_value(data->out, func, inst->as.unary.operand, inst->type);
            fprintf(data->out, ");\n");
            break;
        case MIR_LOGIC_NOT:
        case MIR_BNOT:
            c_type_name(inst->type, type_buf, sizeof(type_buf));
            fprintf(data->out, "    cpx_v%u = (%s)(!", inst->result, type_buf);
            c_emit_value(data->out, func, inst->as.unary.operand, inst->type);
            fprintf(data->out, ");\n");
            break;
        case MIR_CAST:
        case MIR_BITCAST:
        case MIR_TRUNC:
        case MIR_ZEXT:
        case MIR_SEXT:
        case MIR_SITOFP:
        case MIR_FPTOSI:
            c_type_name(inst->type, type_buf, sizeof(type_buf));
            fprintf(data->out, "    cpx_v%u = (%s)", inst->result, type_buf);
            c_emit_value(data->out, func, inst->as.unary.operand, inst->type);
            fprintf(data->out, ";\n");
            break;
        case MIR_COPY:
        case MIR_BORROW:
        case MIR_BORROW_MUT:
        case MIR_MOVE:
            fprintf(data->out, "    cpx_v%u = ", inst->result);
            c_emit_value(data->out, func, inst->as.transfer.source, inst->type);
            fprintf(data->out, ";\n");
            break;
        case MIR_DROP:
        case MIR_ARC_RETAIN:
        case MIR_ARC_RELEASE:
        case MIR_NOP:
        case MIR_DEBUGLOC:
            fprintf(data->out, "    /* ignored opcode %d */\n", inst->opcode);
            break;
        case MIR_BR:
            fprintf(data->out, "    cpx_pred = %u; goto bb%u;\n", data->current_block ? data->current_block->id : 0, inst->as.br.target->id);
            break;
        case MIR_CONDBR:
            fprintf(data->out, "    if (");
            c_emit_value(data->out, func, inst->as.condbr.cond, NULL);
            fprintf(data->out, ") { cpx_pred = %u; goto bb%u; } else { cpx_pred = %u; goto bb%u; }\n",
                    data->current_block ? data->current_block->id : 0,
                    inst->as.condbr.true_bb->id,
                    data->current_block ? data->current_block->id : 0,
                    inst->as.condbr.false_bb->id);
            break;
        case MIR_SWITCH:
            fprintf(data->out, "    switch ((int64_t)");
            c_emit_value(data->out, func, inst->as.sw.discriminant, NULL);
            fprintf(data->out, ") {\n");
            for (int i = 0; i < inst->as.sw.n_cases; i++) {
                fprintf(data->out, "        case %lld: cpx_pred = %u; goto bb%u;\n",
                        (long long)inst->as.sw.case_values[i],
                        data->current_block ? data->current_block->id : 0,
                        inst->as.sw.targets[i]->id);
            }
            fprintf(data->out, "        default: cpx_pred = %u; goto bb%u;\n", data->current_block ? data->current_block->id : 0, inst->as.sw.default_bb ? inst->as.sw.default_bb->id : 0);
            fprintf(data->out, "    }\n");
            break;
        case MIR_RET:
            fprintf(data->out, "    return ");
            c_emit_value(data->out, func, inst->as.ret.value, func->return_type);
            fprintf(data->out, ";\n");
            break;
        case MIR_RET_VOID:
            fprintf(data->out, "    return;\n");
            break;
        default:
            fprintf(data->out, "    /* unsupported MIR opcode %d */\n", inst->opcode);
            data->had_error = true;
            break;
    }
}

static bool c_finalize(MirBackend* self, const char* output_path) {
    CBackendData* data = (CBackendData*)self->data;
    (void)output_path;
    if (data->out) {
        fclose(data->out);
        data->out = NULL;
    }
    return !data->had_error;
}

static void c_destroy(MirBackend* self) {
    CBackendData* data = (CBackendData*)self->data;
    if (data) {
        if (data->out) fclose(data->out);
        free(data);
    }
    free(self);
}

MirBackend* mir_backend_create_c(MirBackendConfig config) {
    MirBackend* backend = (MirBackend*)calloc(1, sizeof(MirBackend));
    CBackendData* data = (CBackendData*)calloc(1, sizeof(CBackendData));
    if (!backend || !data) {
        free(backend);
        free(data);
        return NULL;
    }

    backend->name = "c";
    backend->config = config;
    backend->begin_module = c_begin_module;
    backend->end_module = c_end_module;
    backend->begin_function = c_begin_function;
    backend->end_function = c_end_function;
    backend->emit_inst = c_emit_inst;
    backend->emit_block_label = c_emit_block_label;
    backend->finalize = c_finalize;
    backend->destroy = c_destroy;
    backend->data = data;
    return backend;
}
