/*
 * Backend capability contracts for Casprix MIR consumers.
 *
 * A module may be MIR-valid and still unsupported by a given backend.
 * Capability checks must run before emission; unsupported opcodes must fail
 * with diagnostics — never become silent NOPs or ignored comments.
 */

#ifndef CASPRIX_MIR_BACKEND_CAPS_H
#define CASPRIX_MIR_BACKEND_CAPS_H

#include "compiler/ir/mir.h"
#include "compiler/ir/mir_backend.h"

#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MIR_CAP_LEGACY_NATIVE = 0,
    MIR_CAP_CVM,
    MIR_CAP_EMIT_C,          /* C source; OBJ_ALLOC → calloc stub OK for syntax */
    MIR_CAP_MIR_C_NATIVE,    /* MIR→C→host-cc; scalar subset, no managed heap */
    MIR_CAP_MIR_X86_ASM,     /* experimental SIMD-oriented asm path */
    MIR_CAP_VM_BYTECODE,
    MIR_CAP_JIT_PACKAGE
} MirCapabilityProfile;

/* Authoritative conservative CVM opcode contract. New MIR opcodes stay
 * unsupported until they have a real interpreter handler. */
static inline bool mir_cvm_opcode_supported(MirOpcode op) {
    switch (op) {
        case MIR_CONST_INT: case MIR_CONST_FLOAT: case MIR_CONST_BOOL:
        case MIR_CONST_STRING: case MIR_CONST_NULL: case MIR_GLOBAL_ADDR:
        case MIR_ADD: case MIR_SUB: case MIR_MUL: case MIR_DIV: case MIR_MOD:
        case MIR_NEG: case MIR_FADD: case MIR_FSUB: case MIR_FMUL:
        case MIR_FDIV: case MIR_FNEG:
        case MIR_BAND: case MIR_BOR: case MIR_BXOR: case MIR_BNOT:
        case MIR_SHL: case MIR_SHR: case MIR_USHR:
        case MIR_CMP_EQ: case MIR_CMP_NE: case MIR_CMP_LT: case MIR_CMP_LE:
        case MIR_CMP_GT: case MIR_CMP_GE:
        case MIR_LOGIC_AND: case MIR_LOGIC_OR: case MIR_LOGIC_NOT:
        case MIR_CAST: case MIR_BITCAST: case MIR_TRUNC: case MIR_ZEXT:
        case MIR_SEXT: case MIR_SITOFP: case MIR_FPTOSI:
        case MIR_ALLOCA: case MIR_LOAD: case MIR_STORE:
        case MIR_GET_FIELD_PTR: case MIR_GET_ELEM_PTR:
        case MIR_BR: case MIR_CONDBR: case MIR_SWITCH:
        case MIR_RET: case MIR_RET_VOID: case MIR_UNREACHABLE:
        case MIR_CALL: case MIR_PHI: case MIR_COPY:
        case MIR_ARC_RETAIN: case MIR_ARC_RELEASE: case MIR_OBJ_ALLOC:
        case MIR_BORROW: case MIR_BORROW_MUT: case MIR_MOVE: case MIR_DROP:
        case MIR_NOP: case MIR_DEBUGLOC:
            return true;
        default:
            return false;
    }
}

/* Returns true if every instruction in `module` is supported by `profile`.
 * On failure, writes a diagnostic for the first unsupported opcode. */
bool mir_backend_caps_validate(MirModule* module, MirCapabilityProfile profile,
                               FILE* diag);

const char* mir_capability_profile_name(MirCapabilityProfile profile);

#ifdef __cplusplus
}
#endif

#endif /* CASPRIX_MIR_BACKEND_CAPS_H */
