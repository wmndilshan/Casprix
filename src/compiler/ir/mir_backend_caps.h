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

/* Returns true if every instruction in `module` is supported by `profile`.
 * On failure, writes a diagnostic for the first unsupported opcode. */
bool mir_backend_caps_validate(MirModule* module, MirCapabilityProfile profile,
                               FILE* diag);

const char* mir_capability_profile_name(MirCapabilityProfile profile);

#ifdef __cplusplus
}
#endif

#endif /* CASPRIX_MIR_BACKEND_CAPS_H */
