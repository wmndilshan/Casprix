/*
 * Casprix Compiler — Backend Abstraction Layer
 *
 * Provides a uniform interface for all code generation backends.
 * The compiler pipeline lowers MIR → backend-specific output via
 * this abstraction, enabling:
 *
 *   - Native codegen (x86-64, ARM)     via --aot
 *   - VM bytecode                      via --vm
 *   - JIT compilation                  via --jit
 *
 * Each backend implements the MirBackend vtable.
 *
 * Backend lifecycle:
 *   1. Create backend with appropriate factory function
 *   2. Call begin_module()
 *   3. For each function: begin_function() → emit instructions → end_function()
 *   4. Call end_module()
 *   5. Call finalize() to write output
 *   6. Destroy backend
 */

#ifndef MIR_BACKEND_H
#define MIR_BACKEND_H

#include "mir.h"
#include <stdio.h>

/* ────────────────────────────────────────────────────────────
 * Target architecture
 * ──────────────────────────────────────────────────────────── */
typedef enum {
    MIR_TARGET_X86_64,
    MIR_TARGET_AARCH64,
    MIR_TARGET_WASM32,
    MIR_TARGET_VM,          /* Casprix bytecode VM */
    MIR_TARGET_JIT,         /* JIT (starts as VM, promotes hot paths) */
} MirTargetArch;

/* ────────────────────────────────────────────────────────────
 * Backend output format
 * ──────────────────────────────────────────────────────────── */
typedef enum {
    MIR_OUTPUT_ASM,         /* NASM / GAS assembly text */
    MIR_OUTPUT_OBJ,         /* Object file (.o) */
    MIR_OUTPUT_BYTECODE,    /* VM bytecode binary */
    MIR_OUTPUT_C,           /* C source (transpile) */
} MirOutputFormat;

/* ────────────────────────────────────────────────────────────
 * Backend configuration
 * ──────────────────────────────────────────────────────────── */
typedef struct {
    MirTargetArch       target;
    MirOutputFormat     output_format;
    int                 opt_level;          /* 0-3 */
    bool                debug_info;
    bool                pic;                /* Position-independent code */
    bool                size_opt;           /* Optimize for code size */
    const char*         output_path;        /* Output file path */
} MirBackendConfig;

/* ────────────────────────────────────────────────────────────
 * Backend vtable — every backend implements these
 * ──────────────────────────────────────────────────────────── */
typedef struct MirBackend MirBackend;

struct MirBackend {
    const char*         name;               /* "x86-64", "vm", "jit" */
    MirBackendConfig    config;

    /* Lifecycle */
    bool (*begin_module)(MirBackend* self, MirModule* module);
    void (*end_module)(MirBackend* self);

    bool (*begin_function)(MirBackend* self, MirFunction* func);
    void (*end_function)(MirBackend* self);

    /* Instruction emission — the core of each backend */
    void (*emit_inst)(MirBackend* self, MirInst* inst, MirFunction* func);

    /* Block management */
    void (*emit_block_label)(MirBackend* self, MirBlock* block);

    /* Finalize: write output, flush buffers */
    bool (*finalize)(MirBackend* self, const char* output_path);

    /* Cleanup */
    void (*destroy)(MirBackend* self);

    /* Backend-specific private data */
    void*               data;
};

/* ────────────────────────────────────────────────────────────
 * Backend factory functions
 * ──────────────────────────────────────────────────────────── */

/* Create a native x86-64 backend (outputs NASM assembly) */
MirBackend* mir_backend_create_x86_64(MirBackendConfig config);

/* Create an AArch64 backend (outputs GAS assembly + NEON) */
MirBackend* mir_backend_create_aarch64(MirBackendConfig config);

/* Create a pure-scalar backend -- no SIMD instructions at all.
 * Useful for targets with no vector registers (tiny MCUs, WASM MVP,
 * portable C transpile target).  Relies on simd_legalize_* having
 * scalarized every VEC_* op before emission. */
MirBackend* mir_backend_create_scalar(MirBackendConfig config);

/* Create a VM bytecode backend */
MirBackend* mir_backend_create_vm(MirBackendConfig config);

/* Create a JIT backend (interprets + compiles hot paths) */
MirBackend* mir_backend_create_jit(MirBackendConfig config);
MirBackend* mir_backend_create_c(MirBackendConfig config);

/* ────────────────────────────────────────────────────────────
 * Backend driver — runs a module through a backend
 * ──────────────────────────────────────────────────────────── */

/* Emit an entire module through the given backend.
 * This handles the begin/end lifecycle automatically.
 * Returns true on success. */
bool mir_backend_emit_module(MirBackend* backend, MirModule* module);

/* Query utilities */
const char* mir_target_name(MirTargetArch target);
const char* mir_output_format_name(MirOutputFormat fmt);

/* Test / introspection helper: returns the internal text buffer for
 * the SIMD-aware backends (x86-64, AArch64, scalar).  The buffer is
 * owned by the backend; do NOT fclose it.  Rewind before reading. */
FILE* mir_backend_get_text_buffer(MirBackend* self);

/* Override the SIMD capability the backend emits for.  Must be called
 * before begin_module() so the banner reports the right feature set.
 * Only meaningful for the x86-64 and AArch64 backends -- other
 * backends silently ignore it.
 *
 * The `cap` argument is opaque here (declared as int) to avoid the
 * public header having to pull compiler/opt/simd.h; valid values are
 * members of SimdCapability. */
void mir_backend_set_simd_capability(MirBackend* self, int cap);

/* ────────────────────────────────────────────────────────────
 * VM Bytecode Format (for VM and JIT backends)
 * ──────────────────────────────────────────────────────────── */

/* Bytecode opcodes — compact 1-byte encoding */
typedef enum {
    /* Stack manipulation */
    VM_NOP          = 0x00,
    VM_CONST_I64    = 0x01,  /* push 64-bit int */
    VM_CONST_F64    = 0x02,  /* push 64-bit float */
    VM_CONST_TRUE   = 0x03,
    VM_CONST_FALSE  = 0x04,
    VM_CONST_NULL   = 0x05,
    VM_CONST_STR    = 0x06,  /* push string index */

    /* Load / Store */
    VM_LOAD_LOCAL   = 0x10,
    VM_STORE_LOCAL  = 0x11,
    VM_LOAD_GLOBAL  = 0x12,
    VM_STORE_GLOBAL = 0x13,
    VM_LOAD_FIELD   = 0x14,
    VM_STORE_FIELD  = 0x15,
    VM_LOAD_ELEM    = 0x16,
    VM_STORE_ELEM   = 0x17,

    /* Arithmetic */
    VM_ADD_I        = 0x20,
    VM_SUB_I        = 0x21,
    VM_MUL_I        = 0x22,
    VM_DIV_I        = 0x23,
    VM_MOD_I        = 0x24,
    VM_NEG_I        = 0x25,
    VM_ADD_F        = 0x26,
    VM_SUB_F        = 0x27,
    VM_MUL_F        = 0x28,
    VM_DIV_F        = 0x29,
    VM_NEG_F        = 0x2A,

    /* Bitwise */
    VM_BAND         = 0x30,
    VM_BOR          = 0x31,
    VM_BXOR         = 0x32,
    VM_BNOT         = 0x33,
    VM_SHL          = 0x34,
    VM_SHR          = 0x35,

    /* Comparison */
    VM_CMP_EQ       = 0x40,
    VM_CMP_NE       = 0x41,
    VM_CMP_LT       = 0x42,
    VM_CMP_LE       = 0x43,
    VM_CMP_GT       = 0x44,
    VM_CMP_GE       = 0x45,

    /* Logic */
    VM_NOT          = 0x48,

    /* Conversion */
    VM_I2F          = 0x50,
    VM_F2I          = 0x51,

    /* Control flow */
    VM_JUMP         = 0x60,
    VM_JUMP_IF      = 0x61,
    VM_JUMP_IF_NOT  = 0x62,
    VM_CALL         = 0x63,  /* call func_index, n_args */
    VM_CALL_NATIVE  = 0x64,  /* call native function */
    VM_RET          = 0x65,
    VM_RET_VOID     = 0x66,

    /* Object / ARC */
    VM_OBJ_ALLOC    = 0x70,
    VM_ARC_RETAIN   = 0x71,
    VM_ARC_RELEASE  = 0x72,
    VM_DROP         = 0x73,

    /* Stack ops */
    VM_POP          = 0x80,
    VM_DUP          = 0x81,
    VM_SWAP         = 0x82,

    /* Debug */
    VM_DEBUGLOC     = 0xF0,
    VM_HALT         = 0xFF,
} VmOpcode;

/* Bytecode chunk */
typedef struct {
    uint8_t*    code;
    int         code_size;
    int         code_capacity;

    /* Constant pool */
    int64_t*    const_ints;
    int         const_int_count;
    double*     const_floats;
    int         const_float_count;
    const char** const_strings;
    int         const_string_count;

    /* Function table */
    struct {
        const char* name;
        int         offset;     /* byte offset into code */
        int         n_params;
        int         n_locals;
    }*          functions;
    int         func_count;
    int         func_capacity;
} VmBytecodeChunk;

/* ────────────────────────────────────────────────────────────
 * JIT Profile Data
 * ──────────────────────────────────────────────────────────── */

/* JIT tiers:
 *   Tier 0: Interpreted (VM bytecode)
 *   Tier 1: Baseline JIT (quick compile, no optimization)
 *   Tier 2: Optimizing JIT (full optimization, expensive compile)
 */
typedef enum {
    JIT_TIER_INTERPRET  = 0,
    JIT_TIER_BASELINE   = 1,
    JIT_TIER_OPTIMIZED  = 2,
} JitTier;

/* Per-function JIT profile */
typedef struct {
    const char*     name;
    int             call_count;
    int             loop_iterations;
    JitTier         current_tier;
    void*           native_code;        /* NULL until JIT'd */
    int             native_size;
} JitProfile;

/* Tier promotion thresholds */
#define JIT_BASELINE_THRESHOLD   100    /* calls before Tier 1 compile */
#define JIT_OPTIMIZE_THRESHOLD   5000   /* calls before Tier 2 compile */

#endif /* MIR_BACKEND_H */

