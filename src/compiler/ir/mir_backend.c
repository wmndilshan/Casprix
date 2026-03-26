/*
 * Casprix Compiler — Backend Abstraction Layer Implementation
 *
 * Provides the backend driver and stub implementations.
 * The x86-64 backend bridges to the existing asmgen.c infrastructure;
 * the VM backend produces compact bytecode; the JIT backend is a stub
 * that will be filled in with platform-specific code generation.
 */

#include "mir_backend.h"
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
            vm_emit_i64(d, *(int64_t*)&inst->as.imm_f64);
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
 * x86-64 Native Backend (stub — bridges to existing asmgen.c)
 *
 * In the full implementation, this would lower MIR directly to
 * x86-64 machine code or NASM assembly. For now, it's a placeholder
 * that will be connected to the existing asmgen infrastructure.
 * ================================================================ */

static bool x86_begin_module(MirBackend* self, MirModule* module) {
    (void)self; (void)module;
    /* The existing asmgen.c path handles this */
    return true;
}
static void x86_end_module(MirBackend* self) { (void)self; }
static bool x86_begin_function(MirBackend* self, MirFunction* func) {
    (void)self; (void)func;
    return true;
}
static void x86_end_function(MirBackend* self) { (void)self; }
static void x86_emit_inst(MirBackend* self, MirInst* inst, MirFunction* func) {
    (void)self; (void)inst; (void)func;
    /* TODO: Implement MIR → x86-64 instruction selection */
}
static void x86_emit_block_label(MirBackend* self, MirBlock* block) {
    (void)self; (void)block;
}
static bool x86_finalize(MirBackend* self, const char* output_path) {
    (void)self;
    fprintf(stderr,
            "  [ERROR] MIR x86-64 backend is not implemented yet; "
            "no assembly was written to '%s'\n",
            output_path ? output_path : "<null>");
    return false;
}
static void x86_destroy(MirBackend* self) {
    free(self);
}

MirBackend* mir_backend_create_x86_64(MirBackendConfig config) {
    MirBackend* b = calloc(1, sizeof(MirBackend));
    b->name = "x86-64";
    b->config = config;
    b->data = NULL;

    b->begin_module = x86_begin_module;
    b->end_module = x86_end_module;
    b->begin_function = x86_begin_function;
    b->end_function = x86_end_function;
    b->emit_inst = x86_emit_inst;
    b->emit_block_label = x86_emit_block_label;
    b->finalize = x86_finalize;
    b->destroy = x86_destroy;

    return b;
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
