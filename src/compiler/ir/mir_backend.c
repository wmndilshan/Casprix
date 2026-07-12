/*
 * Casprix Compiler — Backend Abstraction Layer Implementation
 *
 * Provides the backend driver and stub implementations.
 * The x86-64 backend bridges to the existing asmgen.c infrastructure;
 * the VM backend produces compact bytecode; the JIT backend is a stub
 * that will be filled in with platform-specific code generation.
 */

#include "mir_backend.h"
#include "compiler/opt/simd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Utility: target/format names
 * ================================================================ */

const char* mir_target_name(MirTargetArch target) {
    switch (target) {
        case MIR_TARGET_X86_64:  return "x86-64";
        case MIR_TARGET_AARCH64: return "aarch64";
        case MIR_TARGET_WASM32:  return "wasm32";
        case MIR_TARGET_VM:      return "vm";
        case MIR_TARGET_JIT:     return "jit";
        default:                 return "unknown";
    }
}

const char* mir_output_format_name(MirOutputFormat fmt) {
    switch (fmt) {
        case MIR_OUTPUT_ASM:      return "asm";
        case MIR_OUTPUT_OBJ:      return "obj";
        case MIR_OUTPUT_BYTECODE: return "bytecode";
        case MIR_OUTPUT_C:        return "c";
        default:                  return "unknown";
    }
}

/* ================================================================
 * Backend driver: emit entire module
 * ================================================================ */

bool mir_backend_emit_module(MirBackend* backend, MirModule* module) {
    if (!backend || !module) return false;
    if (!mir_validate_module(module)) return false;

    if (!backend->begin_module(backend, module)) {
        fprintf(stderr, "  [ERROR] Backend '%s': begin_module failed\n", backend->name);
        return false;
    }

    for (MirFunction* func = module->func_list; func; func = func->next_func) {
        if (func->is_extern) continue; /* Externs are declared, not defined */

        if (!backend->begin_function(backend, func)) {
            fprintf(stderr, "  [ERROR] Backend '%s': begin_function failed for '%s'\n",
                    backend->name, func->name);
            backend->end_module(backend);
            return false;
        }

        /* Emit all blocks */
        for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
            backend->emit_block_label(backend, bb);
            for (MirInst* inst = bb->first; inst; inst = inst->next) {
                backend->emit_inst(backend, inst, func);
            }
        }

        backend->end_function(backend);
    }

    backend->end_module(backend);

    if (backend->config.output_path) {
        return backend->finalize(backend, backend->config.output_path);
    }

    return true;
}

/* ================================================================
 * VM Bytecode Backend (stub implementation)
 *
 * Translates MIR → stack-based bytecode.
 * ================================================================ */

typedef struct {
    VmBytecodeChunk chunk;
    FILE*           output;
} VmBackendData;

static void vm_emit_byte(VmBackendData* d, uint8_t byte) {
    if (d->chunk.code_size >= d->chunk.code_capacity) {
        d->chunk.code_capacity = d->chunk.code_capacity ? d->chunk.code_capacity * 2 : 4096;
        d->chunk.code = realloc(d->chunk.code, d->chunk.code_capacity);
    }
    d->chunk.code[d->chunk.code_size++] = byte;
}

static void vm_emit_u16(VmBackendData* d, uint16_t val) {
    vm_emit_byte(d, (uint8_t)(val & 0xFF));
    vm_emit_byte(d, (uint8_t)((val >> 8) & 0xFF));
}

static void vm_emit_u32(VmBackendData* d, uint32_t val) {
    vm_emit_byte(d, (uint8_t)(val & 0xFF));
    vm_emit_byte(d, (uint8_t)((val >> 8) & 0xFF));
    vm_emit_byte(d, (uint8_t)((val >> 16) & 0xFF));
    vm_emit_byte(d, (uint8_t)((val >> 24) & 0xFF));
}

static void vm_emit_i64(VmBackendData* d, int64_t val) {
    for (int i = 0; i < 8; i++) {
        vm_emit_byte(d, (uint8_t)((val >> (i * 8)) & 0xFF));
    }
}

static bool vm_begin_module(MirBackend* self, MirModule* module) {
    (void)module;
    VmBackendData* d = (VmBackendData*)self->data;
    memset(&d->chunk, 0, sizeof(VmBytecodeChunk));
    return true;
}

static void vm_end_module(MirBackend* self) {
    VmBackendData* d = (VmBackendData*)self->data;
    vm_emit_byte(d, VM_HALT);
}

static bool vm_begin_function(MirBackend* self, MirFunction* func) {
    VmBackendData* d = (VmBackendData*)self->data;

    /* Register function in the function table */
    if (d->chunk.func_count >= d->chunk.func_capacity) {
        d->chunk.func_capacity = d->chunk.func_capacity ? d->chunk.func_capacity * 2 : 64;
        d->chunk.functions = realloc(d->chunk.functions,
                                      d->chunk.func_capacity * sizeof(d->chunk.functions[0]));
    }
    int idx = d->chunk.func_count++;
    d->chunk.functions[idx].name = func->name;
    d->chunk.functions[idx].offset = d->chunk.code_size;
    d->chunk.functions[idx].n_params = func->param_count;
    d->chunk.functions[idx].n_locals = (int)func->next_value_id;

    return true;
}

static void vm_end_function(MirBackend* self) {
    (void)self;
}

static void vm_emit_inst(MirBackend* self, MirInst* inst, MirFunction* func) {
    VmBackendData* d = (VmBackendData*)self->data;
    (void)func;

    switch (inst->opcode) {
        case MIR_CONST_INT:
            vm_emit_byte(d, VM_CONST_I64);
            vm_emit_i64(d, inst->as.imm_i64);
            if (inst->result != MIR_VALUE_NONE) {
                vm_emit_byte(d, VM_STORE_LOCAL);
                vm_emit_u16(d, (uint16_t)inst->result);
            }
            break;

        case MIR_CONST_FLOAT:
            vm_emit_byte(d, VM_CONST_F64);
            { int64_t bits; memcpy(&bits, &inst->as.imm_f64, sizeof bits);
              vm_emit_i64(d, bits); }
            if (inst->result != MIR_VALUE_NONE) {
                vm_emit_byte(d, VM_STORE_LOCAL);
                vm_emit_u16(d, (uint16_t)inst->result);
            }
            break;

        case MIR_CONST_BOOL:
            vm_emit_byte(d, inst->as.imm_bool ? VM_CONST_TRUE : VM_CONST_FALSE);
            if (inst->result != MIR_VALUE_NONE) {
                vm_emit_byte(d, VM_STORE_LOCAL);
                vm_emit_u16(d, (uint16_t)inst->result);
            }
            break;

        case MIR_ADD:
            vm_emit_byte(d, VM_LOAD_LOCAL);
            vm_emit_u16(d, (uint16_t)inst->as.binary.lhs);
            vm_emit_byte(d, VM_LOAD_LOCAL);
            vm_emit_u16(d, (uint16_t)inst->as.binary.rhs);
            vm_emit_byte(d, VM_ADD_I);
            if (inst->result != MIR_VALUE_NONE) {
                vm_emit_byte(d, VM_STORE_LOCAL);
                vm_emit_u16(d, (uint16_t)inst->result);
            }
            break;

        case MIR_SUB:
            vm_emit_byte(d, VM_LOAD_LOCAL);
            vm_emit_u16(d, (uint16_t)inst->as.binary.lhs);
            vm_emit_byte(d, VM_LOAD_LOCAL);
            vm_emit_u16(d, (uint16_t)inst->as.binary.rhs);
            vm_emit_byte(d, VM_SUB_I);
            if (inst->result != MIR_VALUE_NONE) {
                vm_emit_byte(d, VM_STORE_LOCAL);
                vm_emit_u16(d, (uint16_t)inst->result);
            }
            break;

        case MIR_MUL:
            vm_emit_byte(d, VM_LOAD_LOCAL);
            vm_emit_u16(d, (uint16_t)inst->as.binary.lhs);
            vm_emit_byte(d, VM_LOAD_LOCAL);
            vm_emit_u16(d, (uint16_t)inst->as.binary.rhs);
            vm_emit_byte(d, VM_MUL_I);
            if (inst->result != MIR_VALUE_NONE) {
                vm_emit_byte(d, VM_STORE_LOCAL);
                vm_emit_u16(d, (uint16_t)inst->result);
            }
            break;

        case MIR_CMP_EQ:
            vm_emit_byte(d, VM_LOAD_LOCAL);
            vm_emit_u16(d, (uint16_t)inst->as.binary.lhs);
            vm_emit_byte(d, VM_LOAD_LOCAL);
            vm_emit_u16(d, (uint16_t)inst->as.binary.rhs);
            vm_emit_byte(d, VM_CMP_EQ);
            if (inst->result != MIR_VALUE_NONE) {
                vm_emit_byte(d, VM_STORE_LOCAL);
                vm_emit_u16(d, (uint16_t)inst->result);
            }
            break;

        case MIR_RET:
            vm_emit_byte(d, VM_LOAD_LOCAL);
            vm_emit_u16(d, (uint16_t)inst->as.ret.value);
            vm_emit_byte(d, VM_RET);
            break;

        case MIR_RET_VOID:
            vm_emit_byte(d, VM_RET_VOID);
            break;

        case MIR_BR:
            vm_emit_byte(d, VM_JUMP);
            /* Placeholder offset — needs patching in a real implementation */
            vm_emit_u32(d, 0);
            break;

        case MIR_CONDBR:
            vm_emit_byte(d, VM_LOAD_LOCAL);
            vm_emit_u16(d, (uint16_t)inst->as.condbr.cond);
            vm_emit_byte(d, VM_JUMP_IF);
            vm_emit_u32(d, 0); /* Placeholder for true target */
            vm_emit_byte(d, VM_JUMP);
            vm_emit_u32(d, 0); /* Placeholder for false target */
            break;

        case MIR_CALL:
            /* Push arguments */
            for (int i = 0; i < inst->as.call.n_args; i++) {
                vm_emit_byte(d, VM_LOAD_LOCAL);
                vm_emit_u16(d, (uint16_t)inst->as.call.args[i]);
            }
            vm_emit_byte(d, VM_CALL);
            vm_emit_u16(d, 0); /* Placeholder function index */
            vm_emit_byte(d, (uint8_t)inst->as.call.n_args);
            if (inst->result != MIR_VALUE_NONE) {
                vm_emit_byte(d, VM_STORE_LOCAL);
                vm_emit_u16(d, (uint16_t)inst->result);
            }
            break;

        case MIR_ARC_RETAIN:
            vm_emit_byte(d, VM_LOAD_LOCAL);
            vm_emit_u16(d, (uint16_t)inst->as.refop.ptr);
            vm_emit_byte(d, VM_ARC_RETAIN);
            break;

        case MIR_ARC_RELEASE:
            vm_emit_byte(d, VM_LOAD_LOCAL);
            vm_emit_u16(d, (uint16_t)inst->as.refop.ptr);
            vm_emit_byte(d, VM_ARC_RELEASE);
            break;

        case MIR_NOP:
            break;

        default:
            /* Stub: emit NOP for unhandled instructions */
            vm_emit_byte(d, VM_NOP);
            break;
    }
}

static void vm_emit_block_label(MirBackend* self, MirBlock* block) {
    (void)self;
    (void)block;
    /* In a real implementation, record the current offset for label patching */
}

static bool vm_finalize(MirBackend* self, const char* output_path) {
    VmBackendData* d = (VmBackendData*)self->data;

    FILE* f = fopen(output_path, "wb");
    if (!f) {
        fprintf(stderr, "  [ERROR] Cannot open output: %s\n", output_path);
        return false;
    }

    /* Write magic + version header */
    const uint8_t magic[] = { 'C', 'P', 'X', 'V', 1, 0 };
    fwrite(magic, 1, sizeof(magic), f);

    /* Write function table */
    uint16_t func_count = (uint16_t)d->chunk.func_count;
    fwrite(&func_count, 2, 1, f);
    for (int i = 0; i < d->chunk.func_count; i++) {
        uint8_t name_len = (uint8_t)strlen(d->chunk.functions[i].name);
        fwrite(&name_len, 1, 1, f);
        fwrite(d->chunk.functions[i].name, 1, name_len, f);
        int32_t offset = (int32_t)d->chunk.functions[i].offset;
        fwrite(&offset, 4, 1, f);
        uint16_t n_params = (uint16_t)d->chunk.functions[i].n_params;
        fwrite(&n_params, 2, 1, f);
        uint16_t n_locals = (uint16_t)d->chunk.functions[i].n_locals;
        fwrite(&n_locals, 2, 1, f);
    }

    /* Write bytecode */
    int32_t code_size = (int32_t)d->chunk.code_size;
    fwrite(&code_size, 4, 1, f);
    fwrite(d->chunk.code, 1, d->chunk.code_size, f);

    fclose(f);
    return true;
}

static void vm_destroy(MirBackend* self) {
    VmBackendData* d = (VmBackendData*)self->data;
    free(d->chunk.code);
    free(d->chunk.const_ints);
    free(d->chunk.const_floats);
    free(d->chunk.functions);
    free(d);
    free(self);
}

MirBackend* mir_backend_create_vm(MirBackendConfig config) {
    MirBackend* b = calloc(1, sizeof(MirBackend));
    VmBackendData* d = calloc(1, sizeof(VmBackendData));

    b->name = "vm";
    b->config = config;
    b->data = d;

    b->begin_module = vm_begin_module;
    b->end_module = vm_end_module;
    b->begin_function = vm_begin_function;
    b->end_function = vm_end_function;
    b->emit_inst = vm_emit_inst;
    b->emit_block_label = vm_emit_block_label;
    b->finalize = vm_finalize;
    b->destroy = vm_destroy;

    return b;
}

/* ================================================================
 * Generic SIMD-aware asm-text backend
 *
 * The x86-64, AArch64 and scalar backends share the same control-flow
 * skeleton -- they differ only in which SIMD emission helper they
 * invoke for MIR_VEC_* instructions.  This small wrapper keeps the
 * three vtables in sync and avoids code duplication.
 *
 * All three backends write a textual assembly-like output stream:
 *   * x86-64:  Intel-syntax (NASM) with AVX2 / AVX-512 mnemonics.
 *   * aarch64: GAS-style with NEON mnemonics.
 *   * scalar:  Pseudo-asm that documents each instruction for the
 *              portable C transpile / VM lowering paths.
 *
 * Non-VEC_* MIR opcodes are bridged to the existing asmgen.c pipeline
 * by the surrounding compiler driver -- here we only enrich the
 * stream with SIMD-specific emission text so that the driver can
 * weave it into the final object file.
 * ================================================================ */

typedef struct {
    FILE*       out;        /* NULL until finalize() opens a file */
    FILE*       buffer;     /* in-memory tmp buffer used during emission */
    char*       buffer_mem;
    size_t      buffer_size;
    SimdTarget  target;
    bool        own_target; /* if true, auto-picks target in begin_module */
} SimdAsmBackendData;

static FILE* simd_backend_open_buffer(SimdAsmBackendData* d) {
#if defined(_WIN32)
    /* Use tmpfile() as a portable buffer on Windows. */
    d->buffer = tmpfile();
#else
    d->buffer = open_memstream(&d->buffer_mem, &d->buffer_size);
#endif
    return d->buffer;
}

static bool simd_asm_begin_module(MirBackend* self, MirModule* module) {
    (void)module;
    SimdAsmBackendData* d = (SimdAsmBackendData*)self->data;
    if (d->own_target && d->target.capability == SIMD_CAP_NONE) {
        d->target = simd_target_default(self->config.target);
    }
    if (!d->buffer) simd_backend_open_buffer(d);
    if (!d->buffer) return false;
    fprintf(d->buffer, "; casprix-mir SIMD backend: %s (%s, %d-bit)\n",
            self->name, simd_capability_name(d->target.capability),
            d->target.native_bits);
    return true;
}

static void simd_asm_end_module(MirBackend* self) {
    SimdAsmBackendData* d = (SimdAsmBackendData*)self->data;
    if (d->buffer) fprintf(d->buffer, "; end-of-module\n");
}

static bool simd_asm_begin_function(MirBackend* self, MirFunction* func) {
    SimdAsmBackendData* d = (SimdAsmBackendData*)self->data;
    fprintf(d->buffer, "\n; ── function %s ──\n%s:\n",
            func->name ? func->name : "<anon>",
            func->name ? func->name : "__anon__");
    return true;
}
static void simd_asm_end_function(MirBackend* self) {
    SimdAsmBackendData* d = (SimdAsmBackendData*)self->data;
    fprintf(d->buffer, "    ret\n");
}

static void simd_asm_emit_block_label(MirBackend* self, MirBlock* block) {
    SimdAsmBackendData* d = (SimdAsmBackendData*)self->data;
    fprintf(d->buffer, ".bb%u:\n", block->id);
}

/* Instruction emitter dispatch -- vector ops route to simd_emit_*,
 * non-vector ops are commented out (the main asmgen path owns them). */
static void x86_emit_inst(MirBackend* self, MirInst* inst, MirFunction* func) {
    (void)func;
    SimdAsmBackendData* d = (SimdAsmBackendData*)self->data;
    if (!mir_opcode_is_vec(inst->opcode)) {
        fprintf(d->buffer, "    ; (non-vec op %d handled by asmgen)\n", inst->opcode);
        return;
    }
    if (!simd_emit_x86_64(d->buffer, inst, d->target)) {
        fprintf(d->buffer, "    ; UNSUPPORTED VEC OP %d on %s -- scalarize\n",
                inst->opcode, simd_capability_name(d->target.capability));
        simd_emit_scalar_text(d->buffer, inst);
    }
}

static void aarch64_emit_inst(MirBackend* self, MirInst* inst, MirFunction* func) {
    (void)func;
    SimdAsmBackendData* d = (SimdAsmBackendData*)self->data;
    if (!mir_opcode_is_vec(inst->opcode)) {
        fprintf(d->buffer, "    // (non-vec op %d handled by asmgen)\n", inst->opcode);
        return;
    }
    if (!simd_emit_aarch64(d->buffer, inst, d->target)) {
        fprintf(d->buffer, "    // UNSUPPORTED VEC OP %d on NEON -- scalarize\n",
                inst->opcode);
        simd_emit_scalar_text(d->buffer, inst);
    }
}

static void scalar_emit_inst(MirBackend* self, MirInst* inst, MirFunction* func) {
    (void)func;
    SimdAsmBackendData* d = (SimdAsmBackendData*)self->data;
    if (!mir_opcode_is_vec(inst->opcode)) {
        fprintf(d->buffer, "    ; (non-vec op %d handled by asmgen)\n", inst->opcode);
        return;
    }
    /* Scalar backend must never see VEC_* after legalization -- if we
     * do, emit a clear diagnostic trace. */
    simd_emit_scalar_text(d->buffer, inst);
}

static bool simd_asm_finalize(MirBackend* self, const char* output_path) {
    SimdAsmBackendData* d = (SimdAsmBackendData*)self->data;
    if (!d->buffer) {
        fprintf(stderr, "  [ERROR] %s backend: no buffer\n", self->name);
        return false;
    }
    if (!output_path) return true; /* test path: keep buffer in memory */

    FILE* f = fopen(output_path, "wb");
    if (!f) {
        fprintf(stderr, "  [ERROR] %s backend: cannot write %s\n",
                self->name, output_path);
        return false;
    }

#if defined(_WIN32)
    fflush(d->buffer);
    rewind(d->buffer);
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), d->buffer)) > 0) {
        fwrite(tmp, 1, n, f);
    }
#else
    fflush(d->buffer);
    if (d->buffer_mem) fwrite(d->buffer_mem, 1, d->buffer_size, f);
#endif

    fclose(f);
    return true;
}

static void simd_asm_destroy(MirBackend* self) {
    SimdAsmBackendData* d = (SimdAsmBackendData*)self->data;
    if (d) {
        if (d->buffer) fclose(d->buffer);
        free(d->buffer_mem);
        free(d);
    }
    free(self);
}

/* ---- x86-64 factory ---- */
MirBackend* mir_backend_create_x86_64(MirBackendConfig config) {
    MirBackend*          b = calloc(1, sizeof(MirBackend));
    SimdAsmBackendData*  d = calloc(1, sizeof(SimdAsmBackendData));
    b->name             = "x86-64";
    b->config           = config;
    b->data             = d;
    d->own_target       = true;
    d->target           = simd_target_default(MIR_TARGET_X86_64);

    b->begin_module     = simd_asm_begin_module;
    b->end_module       = simd_asm_end_module;
    b->begin_function   = simd_asm_begin_function;
    b->end_function     = simd_asm_end_function;
    b->emit_block_label = simd_asm_emit_block_label;
    b->emit_inst        = x86_emit_inst;
    b->finalize         = simd_asm_finalize;
    b->destroy          = simd_asm_destroy;
    return b;
}

/* ---- AArch64 / NEON factory ---- */
MirBackend* mir_backend_create_aarch64(MirBackendConfig config) {
    MirBackend*          b = calloc(1, sizeof(MirBackend));
    SimdAsmBackendData*  d = calloc(1, sizeof(SimdAsmBackendData));
    b->name             = "aarch64";
    b->config           = config;
    b->data             = d;
    d->own_target       = true;
    d->target           = simd_target_default(MIR_TARGET_AARCH64);

    b->begin_module     = simd_asm_begin_module;
    b->end_module       = simd_asm_end_module;
    b->begin_function   = simd_asm_begin_function;
    b->end_function     = simd_asm_end_function;
    b->emit_block_label = simd_asm_emit_block_label;
    b->emit_inst        = aarch64_emit_inst;
    b->finalize         = simd_asm_finalize;
    b->destroy          = simd_asm_destroy;
    return b;
}

/* ---- Pure-scalar factory (no SIMD) ---- */
MirBackend* mir_backend_create_scalar(MirBackendConfig config) {
    MirBackend*          b = calloc(1, sizeof(MirBackend));
    SimdAsmBackendData*  d = calloc(1, sizeof(SimdAsmBackendData));
    b->name             = "scalar";
    b->config           = config;
    b->data             = d;
    d->own_target       = false;
    d->target           = simd_target_make(config.target, SIMD_CAP_NONE);

    b->begin_module     = simd_asm_begin_module;
    b->end_module       = simd_asm_end_module;
    b->begin_function   = simd_asm_begin_function;
    b->end_function     = simd_asm_end_function;
    b->emit_block_label = simd_asm_emit_block_label;
    b->emit_inst        = scalar_emit_inst;
    b->finalize         = simd_asm_finalize;
    b->destroy          = simd_asm_destroy;
    return b;
}

/* Expose the internal buffer so tests can assert on emitted text
 * without having to write it to disk.  This is read-only for the
 * caller -- do not fclose. */
static bool is_simd_asm_backend(MirBackend* b) {
    return b && (b->emit_inst == x86_emit_inst ||
                 b->emit_inst == aarch64_emit_inst ||
                 b->emit_inst == scalar_emit_inst);
}

FILE* mir_backend_get_text_buffer(MirBackend* self) {
    if (!is_simd_asm_backend(self)) return NULL;
    SimdAsmBackendData* d = (SimdAsmBackendData*)self->data;
    return d->buffer;
}

void mir_backend_set_simd_capability(MirBackend* self, int cap) {
    if (!is_simd_asm_backend(self)) return;
    SimdAsmBackendData* d = (SimdAsmBackendData*)self->data;
    d->target     = simd_target_make(self->config.target, (SimdCapability)cap);
    d->own_target = false;   /* caller has pinned the capability */
}

/* ================================================================
 * JIT Backend (stub)
 *
 * Tiered compilation: starts interpreted (VM bytecode),
 * promotes hot functions to native code.
 * ================================================================ */

typedef struct {
    JitProfile*     profiles;
    int             profile_count;
    int             profile_capacity;
    VmBytecodeChunk bytecode;       /* Tier 0: interpreted */
} JitBackendData;

static bool jit_begin_module(MirBackend* self, MirModule* module) {
    (void)module;
    JitBackendData* d = (JitBackendData*)self->data;
    memset(&d->bytecode, 0, sizeof(VmBytecodeChunk));
    return true;
}
static void jit_end_module(MirBackend* self) { (void)self; }
static bool jit_begin_function(MirBackend* self, MirFunction* func) {
    JitBackendData* d = (JitBackendData*)self->data;

    /* Register profile entry */
    if (d->profile_count >= d->profile_capacity) {
        d->profile_capacity = d->profile_capacity ? d->profile_capacity * 2 : 64;
        d->profiles = realloc(d->profiles, d->profile_capacity * sizeof(JitProfile));
    }
    JitProfile* p = &d->profiles[d->profile_count++];
    p->name = func->name;
    p->call_count = 0;
    p->loop_iterations = 0;
    p->current_tier = JIT_TIER_INTERPRET;
    p->native_code = NULL;
    p->native_size = 0;

    return true;
}
static void jit_end_function(MirBackend* self) { (void)self; }
static void jit_emit_inst(MirBackend* self, MirInst* inst, MirFunction* func) {
    (void)self; (void)inst; (void)func;
    /* JIT: bytecode emission + profiling counters.
     * Full implementation would insert profile points at loop headers
     * and function entries, and compile hot paths to native code. */
}
static void jit_emit_block_label(MirBackend* self, MirBlock* block) {
    (void)self; (void)block;
}
static bool jit_finalize(MirBackend* self, const char* output_path) {
    JitBackendData* d = (JitBackendData*)self->data;
    FILE* f = fopen(output_path, "wb");
    if (!f) {
        fprintf(stderr, "  [ERROR] Cannot open JIT output: %s\n", output_path);
        return false;
    }

    {
        const uint8_t magic[] = { 'C', 'P', 'X', 'J', 1, 0 };
        uint16_t profile_count = (uint16_t)d->profile_count;
        fwrite(magic, 1, sizeof(magic), f);
        fwrite(&profile_count, sizeof(profile_count), 1, f);
    }

    for (int i = 0; i < d->profile_count; i++) {
        const JitProfile* profile = &d->profiles[i];
        uint8_t name_len = (uint8_t)strlen(profile->name);
        uint8_t tier = (uint8_t)profile->current_tier;
        uint32_t call_count = (uint32_t)profile->call_count;
        uint32_t loop_iterations = (uint32_t)profile->loop_iterations;

        fwrite(&name_len, sizeof(name_len), 1, f);
        fwrite(profile->name, 1, name_len, f);
        fwrite(&tier, sizeof(tier), 1, f);
        fwrite(&call_count, sizeof(call_count), 1, f);
        fwrite(&loop_iterations, sizeof(loop_iterations), 1, f);
    }

    fclose(f);
    return true;
}
static void jit_destroy(MirBackend* self) {
    JitBackendData* d = (JitBackendData*)self->data;
    free(d->profiles);
    free(d->bytecode.code);
    free(d);
    free(self);
}

MirBackend* mir_backend_create_jit(MirBackendConfig config) {
    MirBackend* b = calloc(1, sizeof(MirBackend));
    JitBackendData* d = calloc(1, sizeof(JitBackendData));

    b->name = "jit";
    b->config = config;
    b->data = d;

    b->begin_module = jit_begin_module;
    b->end_module = jit_end_module;
    b->begin_function = jit_begin_function;
    b->end_function = jit_end_function;
    b->emit_inst = jit_emit_inst;
    b->emit_block_label = jit_emit_block_label;
    b->finalize = jit_finalize;
    b->destroy = jit_destroy;

    return b;
}

bool mir_opcode_has_native_lower_mapping(MirOpcode op, MirTargetArch arch) {
    (void)arch;
    /* Keep in sync with `MirOpcode` — anything outside this contiguous
     * range is a compiler bug or a stale opcode value. */
    return (int)op >= (int)MIR_CONST_INT && (int)op <= (int)MIR_DEBUGLOC;
}
