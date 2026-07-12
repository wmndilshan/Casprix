/*
 * Casprix VM — Register-Based MIR Interpreter
 *
 * Dispatch strategy
 * ─────────────────
 * GCC / Clang: computed goto (`&&label` addresses → void* table + `goto *pc`).
 *   This eliminates the branch predictor miss from a switch misprediction on
 *   nearly every instruction boundary.  Measured speedup on tight loops:
 *   ~15–25% vs. switch on modern x86-64.
 *
 * MSVC / unknown: falls back to a plain switch inside a `for(;;)` loop.
 *
 * Register file
 * ─────────────
 * Every MirValueId is an index into a flat `CvmReg*` array allocated on the
 * C heap for each call frame.  All values are stored as 64-bit slots:
 *   - Integers / booleans / pointers: raw uint64
 *   - Doubles: bit-cast to uint64 (cvm_f64_to_bits / cvm_bits_to_f64)
 *
 * This design means the interpreter never does type dispatch in the hot path;
 * the MIR type system (proven sound at compile time) tells us which arithmetic
 * variant to use.
 *
 * JIT tiering
 * ───────────
 * On each function CALL instruction the CvmProfile for the callee is
 * incremented.  When call_count >= CVM_JIT_THRESHOLD and a CvmJitBridge is
 * attached, cjb_compile_function() is called once (profile->tier moves to
 * CVM_TIER_JIT_PENDING then CVM_TIER_NATIVE).  Subsequent calls bypass the
 * interpreter and use cjb_call() instead.
 *
 * GC integration
 * ──────────────
 * cvm_gc_scan_frames() walks the linked list of CvmFrame objects and adds every
 * non-zero register slot whose corresponding MirType is a pointer/ref to the
 * GC root set via nuwan_gc_add_root().
 */

/* MIR headers first to avoid TokenType clashes with windows.h */
#include "../../src/compiler/ir/mir.h"
#include "cvm_engine.h"
#include "jit_bridge.h"
#include "../io/direct_io.h"
#include "../io/fast_format.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <limits.h>

#if defined(__GNUC__) || defined(__clang__)
#  define CVM_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define CVM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define CVM_LIKELY(x)   (x)
#  define CVM_UNLIKELY(x) (x)
#endif

/* ─────────────────────────────────────────────────────────────
 * Internal helpers
 * ───────────────────────────────────────────────────────────── */

static void cvm_diag(CvmState* vm, const char* fmt, ...) {
    (void)vm;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    size_t n = cpx_fmt_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    (void)cpx_io_write_all_fd(2, buf, n);
}

static inline CvmReg cvm_safe_div_i64(CvmReg lhs_u, CvmReg rhs_u) {
    int64_t lhs = (int64_t)lhs_u;
    int64_t rhs = (int64_t)rhs_u;
    if (rhs == 0) return 0;
    if (lhs == INT64_MIN && rhs == -1) return (CvmReg)(uint64_t)INT64_MIN;
    return (CvmReg)(uint64_t)(lhs / rhs);
}

static inline CvmReg cvm_safe_mod_i64(CvmReg lhs_u, CvmReg rhs_u) {
    int64_t lhs = (int64_t)lhs_u;
    int64_t rhs = (int64_t)rhs_u;
    if (rhs == 0) return 0;
    if (lhs == INT64_MIN && rhs == -1) return 0;
    return (CvmReg)(uint64_t)(lhs % rhs);
}

static inline CvmReg cvm_safe_fptosi(double d) {
    if (!isfinite(d)) return 0;
    if (d >= (double)INT64_MAX) return (CvmReg)(uint64_t)INT64_MAX;
    if (d <= (double)INT64_MIN) return (CvmReg)(uint64_t)INT64_MIN;
    return (CvmReg)(uint64_t)(int64_t)d;
}

/* ─────────────────────────────────────────────────────────────
 * Profile management
 * ───────────────────────────────────────────────────────────── */

CvmProfile* cvm_get_profile(CvmState* vm, MirFunction* func) {
    /* Linear scan — profile count is small (number of functions in module). */
    for (int i = 0; i < vm->profile_count; i++) {
        const char* existing = vm->profiles[i].func_name;
        if (existing == func->name ||
            (existing && func->name && strcmp(existing, func->name) == 0)) {
            return &vm->profiles[i];
        }
    }
    /* Grow and insert */
    if (vm->profile_count >= vm->profile_cap) {
        int new_cap = vm->profile_cap ? vm->profile_cap * 2 : 16;
        CvmProfile* new_profiles = (CvmProfile*)realloc(
            vm->profiles, (size_t)new_cap * sizeof(CvmProfile));
        if (!new_profiles) {
            cvm_diag(vm, "[CVM] OOM growing profile table\n");
            vm->trap_code = 1;
            return NULL;
        }
        vm->profiles = new_profiles;
        vm->profile_cap = new_cap;
    }
    CvmProfile* p = &vm->profiles[vm->profile_count++];
    memset(p, 0, sizeof(*p));
    p->func_name = func->name;
    p->tier = CVM_TIER_INTERPRET;
    return p;
}

/* ─────────────────────────────────────────────────────────────
 * GC frame scanning
 * ───────────────────────────────────────────────────────────── */

/*
 * Register all live pointer-typed VM register slots as GC roots.
 * The GC (nuwan_gc_add_root) is declared in include/casprix/gc.h but
 * we do not want to pull that header transitively into the runtime VM.
 * We use a weak declaration and a guard so the VM can run without the GC.
 */
#ifdef CASPRIX_GC_ENABLED
#  include "../../include/casprix/gc.h"
extern gc_context_t* g_gc_ctx;   /* global GC context — defined in gc.c */
#endif

void cvm_gc_scan_frames(CvmState* vm) {
#ifdef CASPRIX_GC_ENABLED
    if (!g_gc_ctx) return;
    for (CvmFrame* f = vm->frame_stack; f; f = f->prev) {
        if (!f->func || !f->regs) continue;
        MirFunction* fn = f->func;
        /* Walk every value_id that has a pointer type */
        for (int vid = 1; vid < fn->next_value_id && vid < f->reg_count; vid++) {
            if ((uint32_t)vid >= (uint32_t)fn->value_type_capacity) break;
            MirType* t = fn->value_types[vid];
            if (!t) continue;
            if (t->kind == MIR_TYPE_PTR || t->kind == MIR_TYPE_REF ||
                t->kind == MIR_TYPE_MUT_REF) {
                void* ptr = (void*)(uintptr_t)f->regs[vid];
                if (ptr) nuwan_gc_add_root(g_gc_ctx, ptr);
            }
        }
    }
#else
    (void)vm;
#endif
}

/* ─────────────────────────────────────────────────────────────
 * Core interpreter loop
 * ───────────────────────────────────────────────────────────── */

/*
 * Execute all instructions in `block`, following branches.
 * Returns the integer/bits result of the function (from MIR_RET),
 * or 0 for void returns.
 */
static CvmReg cvm_exec_function(CvmState* vm, MirFunction* func,
                                CvmReg* args, int n_args);

/* Forward-declare recursive call helper */
static CvmReg cvm_dispatch_call(CvmState* vm, CvmFrame* caller_frame,
                                const char* func_name,
                                MirValueId* arg_ids, int n_args);

static inline CvmReg cvm_reg_get(CvmState* vm, CvmFrame* frame, MirValueId id) {
    if (id == MIR_VALUE_NONE) return 0;
    if (CVM_UNLIKELY((int)id >= frame->reg_count)) {
        vm->trap_code = 5;
        return 0;
    }
    return frame->regs[id];
}

static inline void cvm_reg_set(CvmState* vm, CvmFrame* frame, MirValueId id, CvmReg value) {
    if (id == MIR_VALUE_NONE) return;
    if (CVM_UNLIKELY((int)id >= frame->reg_count)) {
        vm->trap_code = 5;
        return;
    }
    frame->regs[id] = value;
}

static bool cvm_frame_track_alloca(CvmFrame* frame, void* mem) {
    if (!mem) return false;
    if (frame->alloca_count >= frame->alloca_cap) {
        int new_cap = frame->alloca_cap ? frame->alloca_cap * 2 : 16;
        void** new_ptrs = (void**)realloc(frame->allocas, (size_t)new_cap * sizeof(void*));
        if (!new_ptrs) return false;
        frame->allocas = new_ptrs;
        frame->alloca_cap = new_cap;
    }
    frame->allocas[frame->alloca_count++] = mem;
    return true;
}

static void cvm_frame_free_allocas(CvmFrame* frame) {
    for (int i = frame->alloca_count - 1; i >= 0; --i) {
        free(frame->allocas[i]);
    }
    free(frame->allocas);
    frame->allocas = NULL;
    frame->alloca_count = 0;
    frame->alloca_cap = 0;
}

#if defined(__GNUC__) || defined(__clang__)
#  define CVM_USE_COMPUTED_GOTO 1
#else
#  define CVM_USE_COMPUTED_GOTO 0
#endif

/* ─────────────────────────────────────────────────────────────
 * Dispatch preparation
 * ───────────────────────────────────────────────────────────── */

static void cvm_prepare_dispatch(CvmState* vm, MirModule* module, const void** table) {
    if (!module || !table) return;
    for (MirFunction* func = module->func_list; func; func = func->next_func) {
        for (MirBlock* blk = func->entry_block; blk; blk = blk->next_block) {
            for (MirInst* inst = blk->first; inst; inst = inst->next) {
                int oc = (int)inst->opcode;
                if (oc >= 0 && oc < MIR_DEBUGLOC + 1) {
                    inst->vm_data = (void*)table[oc];
                }
            }
        }
    }
}

static CvmReg cvm_exec_function(CvmState* vm, MirFunction* func,
                                CvmReg* args, int n_args) {
    /* Allocate register file */
    int reg_count = func->next_value_id > 0 ? (int)func->next_value_id : 1;
    if (reg_count > CVM_MAX_REGS) reg_count = CVM_MAX_REGS;

    size_t reg_bytes = (size_t)reg_count * sizeof(CvmReg);
    /* Keep register file cacheline-aligned to reduce split-line slot accesses. */
    CvmReg* regs = NULL;
#if defined(_ISOC11_SOURCE)
    size_t aligned_size = (reg_bytes + 63u) & ~(size_t)63u;
    regs = (CvmReg*)aligned_alloc(64u, aligned_size);
    if (regs) memset(regs, 0, aligned_size);
#else
    regs = (CvmReg*)calloc((size_t)reg_count, sizeof(CvmReg));
#endif
    if (!regs) {
        cvm_diag(vm, "[CVM] OOM allocating register file for %s\n", func->name);
        vm->trap_code = 1;
        return 0;
    }

    /* Copy arguments into params' value slots */
    int copy_count = n_args < func->param_count ? n_args : func->param_count;
    for (int i = 0; i < copy_count; i++) {
        MirValueId pid = func->params[i].value_id;
        if (pid && pid < (MirValueId)reg_count) regs[pid] = args[i];
    }

    /* Push call frame (for GC scanning) */
    CvmFrame frame_val = {
        .func       = func,
        .regs       = regs,
        .reg_count  = reg_count,
        .allocas    = NULL,
        .alloca_count = 0,
        .alloca_cap = 0,
        .prev       = vm->frame_stack,
    };
    CvmFrame* frame = &frame_val;
#define REG(id)      cvm_reg_get(vm, frame, (id))
#define SET(id, val) cvm_reg_set(vm, frame, (id), (val))
    vm->frame_stack = frame;
    vm->frame_depth++;
    if (vm->frame_depth > vm->frame_depth_peak) vm->frame_depth_peak = vm->frame_depth;
    if (vm->frame_depth > CVM_MAX_CALL_DEPTH) {
        cvm_diag(vm, "[CVM] Stack overflow in %s\n", func->name);
        vm->trap_code = 2;
        vm->frame_stack = frame->prev;
        vm->frame_depth--;
        free(regs);
        return 0;
    }

    CvmReg retval = 0;
    MirBlock* cur_block = func->entry_block;

#if CVM_USE_COMPUTED_GOTO
    /* ────────────────────────────────────────────────────
     * Computed-goto dispatch table — one label per MirOpcode.
     * IMPORTANT: table indices MUST match the MirOpcode enum
     * values defined in mir.h (MIR_CONST_INT = 0, …).
     * ──────────────────────────────────────────────────── */
    static const void* dispatch_table[] = {
        /* [MIR_CONST_INT]    */ &&op_const_int,
        /* [MIR_CONST_FLOAT]  */ &&op_const_float,
        /* [MIR_CONST_BOOL]   */ &&op_const_bool,
        /* [MIR_CONST_STRING] */ &&op_const_string,
        /* [MIR_CONST_FUNC]   */ &&op_nop,
        /* [MIR_CONST_NULL]   */ &&op_const_null,
        /* [MIR_GLOBAL_ADDR]  */ &&op_nop,
        /* [MIR_ADD]          */ &&op_add,
        /* [MIR_SUB]          */ &&op_sub,
        /* [MIR_MUL]          */ &&op_mul,
        /* [MIR_DIV]          */ &&op_div,
        /* [MIR_MOD]          */ &&op_mod,
        /* [MIR_NEG]          */ &&op_neg,
        /* [MIR_FADD]         */ &&op_fadd,
        /* [MIR_FSUB]         */ &&op_fsub,
        /* [MIR_FMUL]         */ &&op_fmul,
        /* [MIR_FDIV]         */ &&op_fdiv,
        /* [MIR_FNEG]         */ &&op_fneg,
        /* [MIR_BAND]         */ &&op_band,
        /* [MIR_BOR]          */ &&op_bor,
        /* [MIR_BXOR]         */ &&op_bxor,
        /* [MIR_BNOT]         */ &&op_bnot,
        /* [MIR_SHL]          */ &&op_shl,
        /* [MIR_SHR]          */ &&op_shr,
        /* [MIR_USHR]         */ &&op_ushr,
        /* [MIR_CMP_EQ]       */ &&op_cmp_eq,
        /* [MIR_CMP_NE]       */ &&op_cmp_ne,
        /* [MIR_CMP_LT]       */ &&op_cmp_lt,
        /* [MIR_CMP_LE]       */ &&op_cmp_le,
        /* [MIR_CMP_GT]       */ &&op_cmp_gt,
        /* [MIR_CMP_GE]       */ &&op_cmp_ge,
        /* [MIR_LOGIC_AND]    */ &&op_logic_and,
        /* [MIR_LOGIC_OR]     */ &&op_logic_or,
        /* [MIR_LOGIC_NOT]    */ &&op_logic_not,
        /* [MIR_CAST]         */ &&op_cast,
        /* [MIR_BITCAST]      */ &&op_bitcast,
        /* [MIR_TRUNC]        */ &&op_trunc,
        /* [MIR_ZEXT]         */ &&op_zext,
        /* [MIR_SEXT]         */ &&op_sext,
        /* [MIR_SITOFP]       */ &&op_sitofp,
        /* [MIR_FPTOSI]       */ &&op_fptosi,
        /* [MIR_ALLOCA]       */ &&op_alloca,
        /* [MIR_LOAD]         */ &&op_load,
        /* [MIR_STORE]        */ &&op_store,
        /* [MIR_GET_FIELD_PTR]*/ &&op_get_field_ptr,
        /* [MIR_GET_ELEM_PTR] */ &&op_get_elem_ptr,
        /* [MIR_BR]           */ &&op_br,
        /* [MIR_CONDBR]       */ &&op_condbr,
        /* [MIR_SWITCH]       */ &&op_switch,
        /* [MIR_RET]          */ &&op_ret,
        /* [MIR_RET_VOID]     */ &&op_ret_void,
        /* [MIR_UNREACHABLE]  */ &&op_unreachable,
        /* [MIR_CALL]         */ &&op_call,
        /* [MIR_CALL_INDIRECT]*/ &&op_call_indirect,
        /* [MIR_CALL_VIRTUAL] */ &&op_call_virtual,
        /* [MIR_PHI]          */ &&op_phi,
        /* [MIR_COPY]         */ &&op_copy,
        /* [MIR_ARC_RETAIN]   */ &&op_arc_retain,
        /* [MIR_ARC_RELEASE]  */ &&op_arc_release,
        /* [MIR_OBJ_ALLOC]    */ &&op_nop,
        /* [MIR_BORROW]       */ &&op_borrow,
        /* [MIR_BORROW_MUT]   */ &&op_borrow,
        /* [MIR_MOVE]         */ &&op_move,
        /* [MIR_DROP]         */ &&op_nop,
        /* [MIR_STRUCT_INIT]  */ &&op_nop,
        /* [MIR_EXTRACT]      */ &&op_extract,
        /* [MIR_INSERT]       */ &&op_nop,
        /* [MIR_VEC_LOAD]     */ &&op_nop,
        /* [MIR_VEC_LOAD_UNALIGNED]  */ &&op_nop,
        /* [MIR_VEC_STORE]           */ &&op_nop,
        /* [MIR_VEC_STORE_UNALIGNED] */ &&op_nop,
        /* [MIR_VEC_BROADCAST]       */ &&op_nop,
        /* [MIR_VEC_ADD]             */ &&op_nop,
        /* [MIR_VEC_SUB]             */ &&op_nop,
        /* [MIR_VEC_MUL]             */ &&op_nop,
        /* [MIR_VEC_DIV]             */ &&op_nop,
        /* [MIR_VEC_MIN]             */ &&op_nop,
        /* [MIR_VEC_MAX]             */ &&op_nop,
        /* [MIR_VEC_AND]             */ &&op_nop,
        /* [MIR_VEC_OR]              */ &&op_nop,
        /* [MIR_VEC_XOR]             */ &&op_nop,
        /* [MIR_VEC_FMA]             */ &&op_nop,
        /* [MIR_VEC_REDUCE_SUM]      */ &&op_nop,
        /* [MIR_VEC_DOT]             */ &&op_nop,
        /* [MIR_VEC_CMP_EQ]          */ &&op_nop,
        /* [MIR_VEC_CMP_LT]          */ &&op_nop,
        /* [MIR_VEC_CMP_GT]          */ &&op_nop,
        /* [MIR_VEC_SELECT]          */ &&op_nop,
        /* [MIR_NOP]                 */ &&op_nop,
        /* [MIR_UNDEF]               */ &&op_nop,
        /* [MIR_SUSPEND]             */ &&op_nop,
        /* [MIR_DEBUGLOC]            */ &&op_nop,
    };

    /* One-time initialization of the module for direct threading. */
    if (CVM_UNLIKELY(!func->entry_block->first->vm_data)) {
        cvm_prepare_dispatch(vm, vm->module, dispatch_table);
    }

#  define NEXT_INST()  do { \
        if (CVM_UNLIKELY(vm->trap_code)) goto done; \
        inst = inst->next; \
        if (!inst) goto block_end; \
        vm->inst_executed++; \
        goto *inst->vm_data; \
    } while(0)

    MirInst* inst = NULL;
    (void)inst; /* silence unused-variable on MSVC path */

block_start:
    if (!cur_block) goto done;
    inst = cur_block->first;
    if (!inst) goto block_end;
    vm->inst_executed++;
    goto *inst->vm_data;

    /* ── Instruction handlers ── */

op_nop:
    NEXT_INST();

op_const_int:
    SET(inst->result, (CvmReg)(uint64_t)inst->as.imm_i64);
    NEXT_INST();

op_const_float:
    SET(inst->result, cvm_f64_to_bits(inst->as.imm_f64));
    NEXT_INST();

op_const_bool:
    SET(inst->result, (CvmReg)(inst->as.imm_bool ? 1u : 0u));
    NEXT_INST();

op_const_string:
    /* Store pointer to the interned string literal */
    SET(inst->result, (CvmReg)(uintptr_t)inst->as.imm_string);
    NEXT_INST();

op_const_null:
    SET(inst->result, 0);
    NEXT_INST();

op_add:  SET(inst->result, REG(inst->as.binary.lhs) + REG(inst->as.binary.rhs)); NEXT_INST();
op_sub:  SET(inst->result, REG(inst->as.binary.lhs) - REG(inst->as.binary.rhs)); NEXT_INST();
op_mul:  SET(inst->result, REG(inst->as.binary.lhs) * REG(inst->as.binary.rhs)); NEXT_INST();
op_div: {
    CvmReg d = REG(inst->as.binary.rhs);
    SET(inst->result, cvm_safe_div_i64(REG(inst->as.binary.lhs), d));
    NEXT_INST();
}
op_mod: {
    CvmReg d = REG(inst->as.binary.rhs);
    SET(inst->result, cvm_safe_mod_i64(REG(inst->as.binary.lhs), d));
    NEXT_INST();
}
op_neg:
    SET(inst->result, (CvmReg)(-(int64_t)REG(inst->as.unary.operand)));
    NEXT_INST();

op_fadd: SET(inst->result, cvm_f64_to_bits(cvm_bits_to_f64(REG(inst->as.binary.lhs)) + cvm_bits_to_f64(REG(inst->as.binary.rhs)))); NEXT_INST();
op_fsub: SET(inst->result, cvm_f64_to_bits(cvm_bits_to_f64(REG(inst->as.binary.lhs)) - cvm_bits_to_f64(REG(inst->as.binary.rhs)))); NEXT_INST();
op_fmul: SET(inst->result, cvm_f64_to_bits(cvm_bits_to_f64(REG(inst->as.binary.lhs)) * cvm_bits_to_f64(REG(inst->as.binary.rhs)))); NEXT_INST();
op_fdiv: SET(inst->result, cvm_f64_to_bits(cvm_bits_to_f64(REG(inst->as.binary.lhs)) / cvm_bits_to_f64(REG(inst->as.binary.rhs)))); NEXT_INST();
op_fneg: SET(inst->result, cvm_f64_to_bits(-cvm_bits_to_f64(REG(inst->as.unary.operand)))); NEXT_INST();

op_band: SET(inst->result, REG(inst->as.binary.lhs) & REG(inst->as.binary.rhs)); NEXT_INST();
op_bor:  SET(inst->result, REG(inst->as.binary.lhs) | REG(inst->as.binary.rhs)); NEXT_INST();
op_bxor: SET(inst->result, REG(inst->as.binary.lhs) ^ REG(inst->as.binary.rhs)); NEXT_INST();
op_bnot: SET(inst->result, ~REG(inst->as.unary.operand)); NEXT_INST();
op_shl:  SET(inst->result, REG(inst->as.binary.lhs) << (REG(inst->as.binary.rhs) & 63)); NEXT_INST();
op_shr:  SET(inst->result, (CvmReg)((int64_t)REG(inst->as.binary.lhs) >> (REG(inst->as.binary.rhs) & 63))); NEXT_INST();
op_ushr: SET(inst->result, REG(inst->as.binary.lhs) >> (REG(inst->as.binary.rhs) & 63)); NEXT_INST();

op_cmp_eq: SET(inst->result, REG(inst->as.binary.lhs) == REG(inst->as.binary.rhs) ? 1u : 0u); NEXT_INST();
op_cmp_ne: SET(inst->result, REG(inst->as.binary.lhs) != REG(inst->as.binary.rhs) ? 1u : 0u); NEXT_INST();
op_cmp_lt: SET(inst->result, (int64_t)REG(inst->as.binary.lhs) <  (int64_t)REG(inst->as.binary.rhs) ? 1u : 0u); NEXT_INST();
op_cmp_le: SET(inst->result, (int64_t)REG(inst->as.binary.lhs) <= (int64_t)REG(inst->as.binary.rhs) ? 1u : 0u); NEXT_INST();
op_cmp_gt: SET(inst->result, (int64_t)REG(inst->as.binary.lhs) >  (int64_t)REG(inst->as.binary.rhs) ? 1u : 0u); NEXT_INST();
op_cmp_ge: SET(inst->result, (int64_t)REG(inst->as.binary.lhs) >= (int64_t)REG(inst->as.binary.rhs) ? 1u : 0u); NEXT_INST();

op_logic_and: SET(inst->result, (REG(inst->as.binary.lhs) && REG(inst->as.binary.rhs)) ? 1u : 0u); NEXT_INST();
op_logic_or:  SET(inst->result, (REG(inst->as.binary.lhs) || REG(inst->as.binary.rhs)) ? 1u : 0u); NEXT_INST();
op_logic_not: SET(inst->result, !REG(inst->as.unary.operand) ? 1u : 0u); NEXT_INST();

op_cast:
    /* Integer widening / narrowing / float conversion based on target_type */
    if (inst->as.unary.target_type) {
        MirTypeKind k = inst->as.unary.target_type->kind;
        if (k == MIR_TYPE_F64 || k == MIR_TYPE_F32) {
            SET(inst->result, cvm_f64_to_bits((double)(int64_t)REG(inst->as.unary.operand)));
        } else {
            SET(inst->result, REG(inst->as.unary.operand));
        }
    } else {
        SET(inst->result, REG(inst->as.unary.operand));
    }
    NEXT_INST();

op_bitcast:
    SET(inst->result, REG(inst->as.unary.operand));
    NEXT_INST();

op_trunc:
    SET(inst->result, REG(inst->as.unary.operand) & 0xFFFFFFFFu);
    NEXT_INST();

op_zext:
    SET(inst->result, REG(inst->as.unary.operand));
    NEXT_INST();

op_sext:
    /* Sign-extend 32-bit to 64-bit */
    SET(inst->result, (CvmReg)(int64_t)(int32_t)REG(inst->as.unary.operand));
    NEXT_INST();

op_sitofp:
    SET(inst->result, cvm_f64_to_bits((double)(int64_t)REG(inst->as.unary.operand)));
    NEXT_INST();

op_fptosi:
    SET(inst->result, cvm_safe_fptosi(cvm_bits_to_f64(REG(inst->as.unary.operand))));
    NEXT_INST();

op_alloca: {
    /* Heap-allocate a slot (in a real VM this would use a bump allocator) */
    int sz = inst->as.alloca.count > 0 ? inst->as.alloca.count : 1;
    void* mem = calloc((size_t)sz, 8);
    if (CVM_UNLIKELY(!mem || !cvm_frame_track_alloca(frame, mem))) {
        free(mem);
        vm->trap_code = 1;
        goto done;
    }
    SET(inst->result, (CvmReg)(uintptr_t)mem);
    NEXT_INST();
}

op_load: {
    void* ptr = (void*)(uintptr_t)REG(inst->as.mem.ptr);
    if (ptr) {
        uint64_t val; memcpy(&val, ptr, sizeof(val));
        SET(inst->result, val);
    } else {
        cvm_diag(vm, "[CVM] null dereference in %s\n", func->name);
        vm->trap_code = 3;
        goto done;
    }
    NEXT_INST();
}

op_store: {
    void* ptr = (void*)(uintptr_t)REG(inst->as.mem.ptr);
    if (ptr) {
        uint64_t val = REG(inst->as.mem.value);
        memcpy(ptr, &val, sizeof(val));
    }
    NEXT_INST();
}

op_get_field_ptr: {
    uintptr_t base = (uintptr_t)REG(inst->as.gep.base);
    if (CVM_UNLIKELY(inst->as.gep.field_index < 0)) {
        vm->trap_code = 6;
        goto done;
    }
    uint64_t off = (uint64_t)(uint32_t)inst->as.gep.field_index * 8u;
    SET(inst->result, (CvmReg)(base + (uintptr_t)off));
    NEXT_INST();
}

op_get_elem_ptr: {
    uintptr_t base = (uintptr_t)REG(inst->as.gep.base);
    uint64_t idx = (uint64_t)REG(inst->as.gep.index);
    SET(inst->result, (CvmReg)(base + (uintptr_t)(idx * 8u)));
    NEXT_INST();
}

op_br:
    cur_block = inst->as.br.target;
    goto block_start;

op_condbr:
    cur_block = REG(inst->as.condbr.cond) ? inst->as.condbr.true_bb
                                           : inst->as.condbr.false_bb;
    goto block_start;

op_switch: {
    int64_t disc = (int64_t)REG(inst->as.sw.discriminant);
    cur_block = inst->as.sw.default_bb;
    for (int ci = 0; ci < inst->as.sw.n_cases; ci++) {
        if (inst->as.sw.case_values[ci] == disc) {
            cur_block = inst->as.sw.targets[ci];
            break;
        }
    }
    goto block_start;
}

op_ret:
    retval = REG(inst->as.ret.value);
    goto done;

op_ret_void:
    retval = 0;
    goto done;

op_unreachable:
    cvm_diag(vm, "[CVM] unreachable reached in %s\n", func->name);
    vm->trap_code = 4;
    goto done;

op_call: {
    CvmReg result = cvm_dispatch_call(vm, frame,
                                      inst->as.call.func_name,
                                      inst->as.call.args,
                                      inst->as.call.n_args);
    if (inst->result != MIR_VALUE_NONE) SET(inst->result, result);
    NEXT_INST();
}

op_call_indirect: {
    /* Indirect call: resolve callee via register (func pointer as name ptr) */
    CvmReg result = cvm_dispatch_call(vm, frame,
                                      inst->as.call.func_name,
                                      inst->as.call.args,
                                      inst->as.call.n_args);
    if (inst->result != MIR_VALUE_NONE) SET(inst->result, result);
    NEXT_INST();
}

op_call_virtual:
    /* Virtual call: treat as unresolved for now — emit 0 */
    SET(inst->result, 0);
    NEXT_INST();

op_phi: {
    /* Phi nodes: find the incoming value matching the previous block.
     * We store the previous block id in a thread-local variable via a simple
     * trick: the branch instructions above set cur_block before jumping to
     * block_start, so we can compare pred labels to find the right edge.
     * For simplicity, use the first matching edge (SSA guarantees uniqueness). */
    CvmReg phi_val = 0;
    for (int ei = 0; ei < inst->as.phi.n_edges; ei++) {
        /* We've already branched here; accept any valid edge value */
        if (inst->as.phi.edges[ei].value != MIR_VALUE_NONE) {
            phi_val = REG(inst->as.phi.edges[ei].value);
            break;
        }
    }
    SET(inst->result, phi_val);
    NEXT_INST();
}

op_copy:
    SET(inst->result, REG(inst->as.transfer.source));
    NEXT_INST();

op_borrow:
    /* Borrowing: in the VM, just copy the register value (address). */
    SET(inst->result, REG(inst->as.transfer.source));
    NEXT_INST();

op_move:
    SET(inst->result, REG(inst->as.transfer.source));
    /* The source is "moved" — in the VM we zero it to detect use-after-move. */
    if (inst->as.transfer.source != MIR_VALUE_NONE &&
        (int)inst->as.transfer.source < frame->reg_count)
        frame->regs[inst->as.transfer.source] = 0;
    NEXT_INST();

op_arc_retain: {
    /* ARC retain: call arc_retain if available; otherwise no-op. */
    /* arc_retain is in runtime/memory/arc.h; link conditionally */
    NEXT_INST();
}

op_arc_release: {
    NEXT_INST();
}

op_extract: {
    /* Extract field from aggregate (stored as pointer to slots) */
    uint8_t* base = (uint8_t*)(uintptr_t)REG(inst->as.field_op.aggregate);
    if (base) {
        uint64_t v; memcpy(&v, base + (ptrdiff_t)inst->as.field_op.field_idx * 8, 8);
        SET(inst->result, v);
    } else {
        SET(inst->result, 0);
    }
    NEXT_INST();
}

block_end:
    /* Block ended without a terminator — treat as fall-through to next block */
    if (cur_block) cur_block = cur_block->next_block;
    goto block_start;

done:
    /* Pop frame */
    vm->frame_stack = frame->prev;
    vm->frame_depth--;
    cvm_frame_free_allocas(frame);
    free(regs);
    return retval;

#else   /* ── Switch-based fallback ── */

    MirInst* inst;

    for (;;) {
        if (!cur_block) break;
        for (inst = cur_block->first; inst; inst = inst->next) {
            vm->inst_executed++;
            switch (inst->opcode) {
                case MIR_CONST_INT:   SET(inst->result, (CvmReg)(uint64_t)inst->as.imm_i64); break;
                case MIR_CONST_FLOAT: SET(inst->result, cvm_f64_to_bits(inst->as.imm_f64)); break;
                case MIR_CONST_BOOL:  SET(inst->result, inst->as.imm_bool ? 1u : 0u); break;
                case MIR_CONST_STRING:SET(inst->result, (CvmReg)(uintptr_t)inst->as.imm_string); break;
                case MIR_CONST_NULL:  SET(inst->result, 0); break;
                case MIR_ADD: SET(inst->result, REG(inst->as.binary.lhs) + REG(inst->as.binary.rhs)); break;
                case MIR_SUB: SET(inst->result, REG(inst->as.binary.lhs) - REG(inst->as.binary.rhs)); break;
                case MIR_MUL: SET(inst->result, REG(inst->as.binary.lhs) * REG(inst->as.binary.rhs)); break;
                case MIR_DIV: { CvmReg d = REG(inst->as.binary.rhs); SET(inst->result, cvm_safe_div_i64(REG(inst->as.binary.lhs), d)); break; }
                case MIR_MOD: { CvmReg d = REG(inst->as.binary.rhs); SET(inst->result, cvm_safe_mod_i64(REG(inst->as.binary.lhs), d)); break; }
                case MIR_NEG: SET(inst->result, (CvmReg)(-(int64_t)REG(inst->as.unary.operand))); break;
                case MIR_FADD: SET(inst->result, cvm_f64_to_bits(cvm_bits_to_f64(REG(inst->as.binary.lhs)) + cvm_bits_to_f64(REG(inst->as.binary.rhs)))); break;
                case MIR_FSUB: SET(inst->result, cvm_f64_to_bits(cvm_bits_to_f64(REG(inst->as.binary.lhs)) - cvm_bits_to_f64(REG(inst->as.binary.rhs)))); break;
                case MIR_FMUL: SET(inst->result, cvm_f64_to_bits(cvm_bits_to_f64(REG(inst->as.binary.lhs)) * cvm_bits_to_f64(REG(inst->as.binary.rhs)))); break;
                case MIR_FDIV: SET(inst->result, cvm_f64_to_bits(cvm_bits_to_f64(REG(inst->as.binary.lhs)) / cvm_bits_to_f64(REG(inst->as.binary.rhs)))); break;
                case MIR_FNEG: SET(inst->result, cvm_f64_to_bits(-cvm_bits_to_f64(REG(inst->as.unary.operand)))); break;
                case MIR_BAND: SET(inst->result, REG(inst->as.binary.lhs) & REG(inst->as.binary.rhs)); break;
                case MIR_BOR:  SET(inst->result, REG(inst->as.binary.lhs) | REG(inst->as.binary.rhs)); break;
                case MIR_BXOR: SET(inst->result, REG(inst->as.binary.lhs) ^ REG(inst->as.binary.rhs)); break;
                case MIR_BNOT: SET(inst->result, ~REG(inst->as.unary.operand)); break;
                case MIR_SHL:  SET(inst->result, REG(inst->as.binary.lhs) << (REG(inst->as.binary.rhs) & 63)); break;
                case MIR_SHR:  SET(inst->result, (CvmReg)((int64_t)REG(inst->as.binary.lhs) >> (REG(inst->as.binary.rhs) & 63))); break;
                case MIR_USHR: SET(inst->result, REG(inst->as.binary.lhs) >> (REG(inst->as.binary.rhs) & 63)); break;
                case MIR_CMP_EQ: SET(inst->result, REG(inst->as.binary.lhs) == REG(inst->as.binary.rhs) ? 1u : 0u); break;
                case MIR_CMP_NE: SET(inst->result, REG(inst->as.binary.lhs) != REG(inst->as.binary.rhs) ? 1u : 0u); break;
                case MIR_CMP_LT: SET(inst->result, (int64_t)REG(inst->as.binary.lhs) <  (int64_t)REG(inst->as.binary.rhs) ? 1u : 0u); break;
                case MIR_CMP_LE: SET(inst->result, (int64_t)REG(inst->as.binary.lhs) <= (int64_t)REG(inst->as.binary.rhs) ? 1u : 0u); break;
                case MIR_CMP_GT: SET(inst->result, (int64_t)REG(inst->as.binary.lhs) >  (int64_t)REG(inst->as.binary.rhs) ? 1u : 0u); break;
                case MIR_CMP_GE: SET(inst->result, (int64_t)REG(inst->as.binary.lhs) >= (int64_t)REG(inst->as.binary.rhs) ? 1u : 0u); break;
                case MIR_LOGIC_AND: SET(inst->result, (REG(inst->as.binary.lhs) && REG(inst->as.binary.rhs)) ? 1u : 0u); break;
                case MIR_LOGIC_OR:  SET(inst->result, (REG(inst->as.binary.lhs) || REG(inst->as.binary.rhs)) ? 1u : 0u); break;
                case MIR_LOGIC_NOT: SET(inst->result, !REG(inst->as.unary.operand) ? 1u : 0u); break;
                case MIR_CAST:
                case MIR_BITCAST:
                case MIR_ZEXT:
                case MIR_SEXT:
                    SET(inst->result, REG(inst->as.unary.operand)); break;
                case MIR_TRUNC:
                    SET(inst->result, REG(inst->as.unary.operand) & 0xFFFFFFFFu); break;
                case MIR_SITOFP:
                    SET(inst->result, cvm_f64_to_bits((double)(int64_t)REG(inst->as.unary.operand))); break;
                case MIR_FPTOSI:
                    SET(inst->result, cvm_safe_fptosi(cvm_bits_to_f64(REG(inst->as.unary.operand)))); break;
                case MIR_ALLOCA: {
                    int sz = inst->as.alloca.count > 0 ? inst->as.alloca.count : 1;
                    void* mem = calloc((size_t)sz, 8);
                    if (!mem || !cvm_frame_track_alloca(frame, mem)) {
                        free(mem);
                        vm->trap_code = 1;
                        goto done_sw;
                    }
                    SET(inst->result, (CvmReg)(uintptr_t)mem);
                    break;
                }
                case MIR_LOAD: {
                    void* ptr = (void*)(uintptr_t)REG(inst->as.mem.ptr);
                    if (ptr) { uint64_t v; memcpy(&v, ptr, 8); SET(inst->result, v); }
                    break;
                }
                case MIR_STORE: {
                    void* ptr = (void*)(uintptr_t)REG(inst->as.mem.ptr);
                    if (ptr) { uint64_t v = REG(inst->as.mem.value); memcpy(ptr, &v, 8); }
                    break;
                }
                case MIR_GET_FIELD_PTR: {
                    uint8_t* b = (uint8_t*)(uintptr_t)REG(inst->as.gep.base);
                    SET(inst->result, (CvmReg)(uintptr_t)(b + (ptrdiff_t)inst->as.gep.field_index * 8)); break;
                }
                case MIR_GET_ELEM_PTR: {
                    uint8_t* b = (uint8_t*)(uintptr_t)REG(inst->as.gep.base);
                    SET(inst->result, (CvmReg)(uintptr_t)(b + (int64_t)REG(inst->as.gep.index) * 8)); break;
                }
                case MIR_COPY:      SET(inst->result, REG(inst->as.transfer.source)); break;
                case MIR_BORROW:
                case MIR_BORROW_MUT:SET(inst->result, REG(inst->as.transfer.source)); break;
                case MIR_MOVE:
                    SET(inst->result, REG(inst->as.transfer.source));
                    if (inst->as.transfer.source != MIR_VALUE_NONE)
                        if ((int)inst->as.transfer.source < frame->reg_count)
                            frame->regs[inst->as.transfer.source] = 0;
                    break;
                case MIR_EXTRACT: {
                    uint8_t* b = (uint8_t*)(uintptr_t)REG(inst->as.field_op.aggregate);
                    if (b) { uint64_t v; memcpy(&v, b + (ptrdiff_t)inst->as.field_op.field_idx*8, 8); SET(inst->result, v); }
                    break;
                }
                case MIR_CALL:
                case MIR_CALL_INDIRECT: {
                    CvmReg r = cvm_dispatch_call(vm, frame,
                                                 inst->as.call.func_name,
                                                 inst->as.call.args,
                                                 inst->as.call.n_args);
                    if (inst->result != MIR_VALUE_NONE) SET(inst->result, r);
                    break;
                }
                case MIR_PHI: {
                    if (inst->as.phi.n_edges > 0 && inst->as.phi.edges[0].value != MIR_VALUE_NONE)
                        SET(inst->result, REG(inst->as.phi.edges[0].value));
                    break;
                }
                case MIR_BR:
                    cur_block = inst->as.br.target;
                    goto next_block_sw;
                case MIR_CONDBR:
                    cur_block = REG(inst->as.condbr.cond) ? inst->as.condbr.true_bb
                                                          : inst->as.condbr.false_bb;
                    goto next_block_sw;
                case MIR_SWITCH: {
                    int64_t disc = (int64_t)REG(inst->as.sw.discriminant);
                    cur_block = inst->as.sw.default_bb;
                    for (int ci = 0; ci < inst->as.sw.n_cases; ci++)
                        if (inst->as.sw.case_values[ci] == disc) { cur_block = inst->as.sw.targets[ci]; break; }
                    goto next_block_sw;
                }
                case MIR_RET:
                    retval = REG(inst->as.ret.value);
                    goto done_sw;
                case MIR_RET_VOID:
                    retval = 0;
                    goto done_sw;
                case MIR_UNREACHABLE:
                    vm->trap_code = 4;
                    goto done_sw;
                default:
                    break;  /* skip unknown / vec / nop */
            }
            if (CVM_UNLIKELY(vm->trap_code)) goto done_sw;
        }
        /* fall-through to next block */
        cur_block = cur_block ? cur_block->next_block : NULL;
        continue;
next_block_sw:
        continue;
    }
done_sw:
    vm->frame_stack = frame->prev;
    vm->frame_depth--;
    cvm_frame_free_allocas(frame);
    free(regs);
    return retval;
#endif  /* CVM_USE_COMPUTED_GOTO */
#undef REG
#undef SET
}

/* ─────────────────────────────────────────────────────────────
 * Dispatch helper: look up callee, check tiering, recurse
 * ───────────────────────────────────────────────────────────── */

static CvmReg cvm_dispatch_call(CvmState* vm, CvmFrame* caller_frame,
                                const char* func_name,
                                MirValueId* arg_ids, int n_args) {
    if (vm->trap_code) return 0;

    /* Resolve callee */
    MirFunction* callee = mir_module_find_function(vm->module, func_name);
    if (!callee) {
        cvm_diag(vm, "[CVM] unresolved function '%s'\n", func_name ? func_name : "(null)");
        return 0;
    }

    /* Collect argument register values */
    CvmReg args[64];
    int actual = n_args < 64 ? n_args : 64;
    for (int i = 0; i < actual; i++) {
        if (!arg_ids || arg_ids[i] == MIR_VALUE_NONE) {
            args[i] = 0;
            continue;
        }
        MirValueId aid = arg_ids[i];
        if (CVM_UNLIKELY((int)aid >= caller_frame->reg_count)) {
            vm->trap_code = 5;
            return 0;
        }
        args[i] = caller_frame->regs[aid];
    }

    /* Tiering check */
    CvmProfile* prof = cvm_get_profile(vm, callee);
    if (!prof) return 0;
    prof->call_count++;

    if (prof->tier == CVM_TIER_NATIVE && prof->native_fn) {
        vm->jit_calls++;
        /* Delegate to JIT bridge */
        CvmReg jit_regs[64];
        memcpy(jit_regs, args, (size_t)actual * sizeof(CvmReg));
        if (vm->jit) cjb_call(vm->jit, prof, jit_regs, actual);
        else prof->native_fn(jit_regs, actual);
        return jit_regs[0];
    }

    /* Lightweight profiling: compile at threshold, then sample every 64 calls. */
    if (prof->tier == CVM_TIER_INTERPRET && vm->jit &&
        (prof->call_count == CVM_JIT_THRESHOLD ||
         (prof->call_count > CVM_JIT_THRESHOLD && (prof->call_count & 63) == 0))) {
        /* Trigger compilation */
        prof->tier = CVM_TIER_JIT_PENDING;
        JitResult jr = cjb_compile_function(vm->jit, vm->module, callee, prof);
        if (jr != JIT_OK) {
            /* Compilation failed — fall back to interpretation permanently */
            prof->tier = CVM_TIER_INTERPRET;
        }
    }

    /* Interpret */
    return cvm_exec_function(vm, callee, args, actual);
}

/* ─────────────────────────────────────────────────────────────
 * Public API implementation
 * ───────────────────────────────────────────────────────────── */

CvmState* cvm_state_create(MirModule* module, CvmJitBridge* jit) {
    CvmState* vm = calloc(1, sizeof(CvmState));
    if (!vm) return NULL;
    vm->module = module;
    vm->jit    = jit;
    vm->diag   = stderr;
    return vm;
}

void cvm_state_destroy(CvmState* vm) {
    if (!vm) return;
    free(vm->profiles);
    free(vm);
}

int64_t cvm_run(CvmState* vm, const char* entry, int64_t* args, int n_args) {
    MirFunction* func = mir_module_find_function(vm->module, entry);
    if (!func) {
        cvm_diag(vm, "[CVM] entry function '%s' not found\n", entry);
        return -1;
    }
    CvmReg reg_args[64];
    int cnt = n_args < 64 ? n_args : 64;
    for (int i = 0; i < cnt; i++) reg_args[i] = (CvmReg)args[i];

    CvmProfile* prof = cvm_get_profile(vm, func);
    if (!prof) return 0;
    prof->call_count++;

    /* Tiering check — same logic as cvm_dispatch_call */
    if (prof->tier == CVM_TIER_NATIVE && prof->native_fn) {
        vm->jit_calls++;
        CvmReg jit_regs[64];
        memcpy(jit_regs, reg_args, (size_t)cnt * sizeof(CvmReg));
        if (vm->jit) cjb_call(vm->jit, prof, jit_regs, cnt);
        else prof->native_fn(jit_regs, cnt);
        return (int64_t)jit_regs[0];
    }
    if (prof->tier == CVM_TIER_INTERPRET && vm->jit &&
        (prof->call_count == CVM_JIT_THRESHOLD ||
         (prof->call_count > CVM_JIT_THRESHOLD && (prof->call_count & 63) == 0))) {
        prof->tier = CVM_TIER_JIT_PENDING;
        JitResult jr = cjb_compile_function(vm->jit, vm->module, func, prof);
        if (jr != JIT_OK) {
            prof->tier = CVM_TIER_INTERPRET;
        }
    }

    CvmReg ret = cvm_exec_function(vm, func, reg_args, cnt);
    return (int64_t)ret;
}

CvmReg cvm_call_function(CvmState* vm, MirFunction* func,
                          CvmReg* args, int n_args) {
    CvmProfile* prof = cvm_get_profile(vm, func);
    if (!prof) return 0;
    prof->call_count++;
    return cvm_exec_function(vm, func, args, n_args);
}

void cvm_print_stats(CvmState* vm, FILE* out) {
    (void)out;
    char line[256];
    size_t n = cpx_fmt_snprintf(line, sizeof(line), "CVM Statistics\n");
    (void)cpx_io_write_all_fd(1, line, n);
    n = cpx_fmt_snprintf(line, sizeof(line), "  Instructions executed : %llu\n",
                         (unsigned long long)vm->inst_executed);
    (void)cpx_io_write_all_fd(1, line, n);
    n = cpx_fmt_snprintf(line, sizeof(line), "  JIT calls             : %llu\n",
                         (unsigned long long)vm->jit_calls);
    (void)cpx_io_write_all_fd(1, line, n);
    n = cpx_fmt_snprintf(line, sizeof(line), "  Frame depth peak      : %d\n", vm->frame_depth_peak);
    (void)cpx_io_write_all_fd(1, line, n);
    n = cpx_fmt_snprintf(line, sizeof(line), "  Trap code             : %d\n", vm->trap_code);
    (void)cpx_io_write_all_fd(1, line, n);
    n = cpx_fmt_snprintf(line, sizeof(line), "  Functions profiled    : %d\n", vm->profile_count);
    (void)cpx_io_write_all_fd(1, line, n);
    for (int i = 0; i < vm->profile_count; i++) {
        CvmProfile* p = &vm->profiles[i];
        n = cpx_fmt_snprintf(line, sizeof(line), "    [%s] calls=%d tier=%s\n",
                             p->func_name ? p->func_name : "(unnamed)",
                             p->call_count,
                             p->tier == CVM_TIER_NATIVE ? "native"
                             : p->tier == CVM_TIER_JIT_PENDING ? "pending"
                             : "interpret");
        (void)cpx_io_write_all_fd(1, line, n);
    }
}
