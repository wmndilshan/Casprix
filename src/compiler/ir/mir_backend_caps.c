#include "compiler/ir/mir_backend_caps.h"

#include <stdio.h>

const char* mir_capability_profile_name(MirCapabilityProfile profile) {
    switch (profile) {
        case MIR_CAP_LEGACY_NATIVE: return "legacy-native";
        case MIR_CAP_CVM:           return "cvm";
        case MIR_CAP_EMIT_C:        return "emit-c";
        case MIR_CAP_MIR_C_NATIVE:  return "mir-c-native";
        case MIR_CAP_MIR_X86_ASM:   return "mir-x86-asm";
        case MIR_CAP_VM_BYTECODE:   return "vm-bytecode";
        case MIR_CAP_JIT_PACKAGE:   return "jit-package";
        default:                    return "unknown";
    }
}

static bool opcode_supported_emit_c(MirOpcode op) {
    switch (op) {
        case MIR_NOP:
        case MIR_DEBUGLOC:
        case MIR_UNDEF:
        case MIR_CONST_INT:
        case MIR_CONST_FLOAT:
        case MIR_CONST_BOOL:
        case MIR_CONST_NULL:
        case MIR_CONST_STRING:
        case MIR_CONST_FUNC:
        case MIR_GLOBAL_ADDR:
        case MIR_ALLOCA:
        case MIR_LOAD:
        case MIR_STORE:
        case MIR_GET_FIELD_PTR:
        case MIR_GET_ELEM_PTR:
        case MIR_ADD: case MIR_SUB: case MIR_MUL: case MIR_DIV: case MIR_MOD:
        case MIR_FADD: case MIR_FSUB: case MIR_FMUL: case MIR_FDIV: case MIR_FNEG:
        case MIR_NEG: case MIR_BNOT: case MIR_LOGIC_NOT:
        case MIR_BAND: case MIR_BOR: case MIR_BXOR: case MIR_SHL: case MIR_SHR: case MIR_USHR:
        case MIR_LOGIC_AND: case MIR_LOGIC_OR:
        case MIR_CMP_EQ: case MIR_CMP_NE: case MIR_CMP_LT: case MIR_CMP_LE:
        case MIR_CMP_GT: case MIR_CMP_GE:
        case MIR_CAST: case MIR_BITCAST: case MIR_TRUNC: case MIR_ZEXT: case MIR_SEXT:
        case MIR_SITOFP: case MIR_FPTOSI:
        case MIR_BR: case MIR_CONDBR: case MIR_SWITCH:
        case MIR_RET: case MIR_RET_VOID:
        case MIR_UNREACHABLE:
        case MIR_CALL: case MIR_CALL_INDIRECT:
        case MIR_PHI:
        case MIR_COPY: case MIR_MOVE: case MIR_BORROW: case MIR_BORROW_MUT:
        case MIR_OBJ_ALLOC:
        case MIR_ARC_RETAIN:
        case MIR_ARC_RELEASE:
        case MIR_DROP:
            return true;
        case MIR_CALL_VIRTUAL:
        case MIR_SUSPEND:
            return false;
        default:
            return false;
    }
}

static bool opcode_supported_mir_c_native(MirOpcode op) {
    /* Executable MIR→C→native path: no managed heap until ARC emission is real. */
    if (op == MIR_OBJ_ALLOC || op == MIR_ARC_RETAIN ||
        op == MIR_ARC_RELEASE || op == MIR_DROP ||
        op == MIR_CALL_VIRTUAL || op == MIR_SUSPEND)
        return false;
    return opcode_supported_emit_c(op) && op != MIR_OBJ_ALLOC;
}

static bool opcode_supported_mir_x86_asm(MirOpcode op) {
    /* Current x86 MIR backend only lowers MIR_VEC_*; scalars are comments. */
    return mir_opcode_is_vec(op) || op == MIR_NOP || op == MIR_DEBUGLOC;
}

static bool opcode_ok(MirCapabilityProfile profile, MirOpcode op) {
    switch (profile) {
        case MIR_CAP_EMIT_C:      return opcode_supported_emit_c(op);
        case MIR_CAP_MIR_C_NATIVE: return opcode_supported_mir_c_native(op);
        case MIR_CAP_CVM:         return mir_cvm_opcode_supported(op);
        case MIR_CAP_MIR_X86_ASM: return opcode_supported_mir_x86_asm(op);
        case MIR_CAP_VM_BYTECODE:
            /* Only opcodes with complete (non-placeholder) encodings. */
            switch (op) {
                case MIR_NOP: case MIR_DEBUGLOC:
                case MIR_CONST_INT: case MIR_CONST_FLOAT: case MIR_CONST_BOOL:
                case MIR_ADD: case MIR_SUB: case MIR_MUL:
                case MIR_CMP_EQ:
                case MIR_RET: case MIR_RET_VOID:
                case MIR_ARC_RETAIN: case MIR_ARC_RELEASE:
                    return true;
                default:
                    return false;
            }
        case MIR_CAP_JIT_PACKAGE:
            /* Standalone JIT packaging is incomplete. */
            return false;
        case MIR_CAP_LEGACY_NATIVE:
            return true;
        default:
            return false;
    }
}

static bool instruction_supported(MirFunction* func, MirInst* inst,
                                  MirCapabilityProfile profile) {
    if (!opcode_ok(profile, inst->opcode)) return false;
    if (profile != MIR_CAP_CVM) return true;

    if (inst->opcode == MIR_ARC_RETAIN || inst->opcode == MIR_ARC_RELEASE ||
        inst->opcode == MIR_DROP) {
        MirType* type = mir_function_value_type(func, inst->as.refop.ptr);
        if (!type || (type->kind != MIR_TYPE_PTR && type->kind != MIR_TYPE_REF &&
                      type->kind != MIR_TYPE_MUT_REF)) return false;
    }
    if (inst->opcode == MIR_OBJ_ALLOC) {
        int size = inst->as.obj_alloc.size;
        int count = inst->as.obj_alloc.managed_field_count;
        int* offsets = inst->as.obj_alloc.managed_offsets;
        if (size <= 0 || count < 0 || (count > 0 && !offsets)) return false;
        for (int i = 0; i < count; i++) {
            int off = offsets[i];
            if (off < (int)sizeof(void*) || off % (int)_Alignof(void*) != 0 ||
                off > size || (int)sizeof(void*) > size - off) return false;
            for (int j = 0; j < i; j++)
                if (offsets[j] == off) return false;
        }
    }
    return true;
}

bool mir_backend_caps_validate(MirModule* module, MirCapabilityProfile profile,
                               FILE* diag) {
    if (!module) return false;
    if (!diag) diag = stderr;

    for (MirFunction* f = module->func_list; f; f = f->next_func) {
        for (MirBlock* bb = f->entry_block; bb; bb = bb->next_block) {
            for (MirInst* inst = bb->first; inst; inst = inst->next) {
                if (instruction_supported(f, inst, profile)) continue;
                fprintf(diag,
                        "MIR_CAPABILITY: backend '%s' does not support opcode %s (%d) "
                        "or its operand metadata in function '%s', bb%u at %d:%d\n",
                        mir_capability_profile_name(profile),
                        mir_opcode_name(inst->opcode),
                        (int)inst->opcode,
                        f->name ? f->name : "<anon>", bb->id,
                        inst->src_line, inst->src_col);
                return false;
            }
        }
    }
    return true;
}
