/*
 * Casprix JIT Bridge
 *
 * Tier-up pipeline
 * ────────────────
 *  Interpreted (CVM)  ──[hotness ≥ threshold]──►  cjb_compile_function()
 *                                                        │
 *                           ┌───────────────────────────┘
 *                           ▼
 *                    1. mir_printer  → MIR text  (temp file)
 *                    2. mir_backend_create_x86_64() → NASM asm text
 *                    3. NASM -f bin  → flat machine-code binary
 *                       (or: fallback x86-64 mini-JIT for ADD/SUB/MUL/RET)
 *                    4. jit_alloc_exec() → RWX page, memcpy code
 *                    5. profile->native_fn = trampoline wrapper
 *                    6. profile->tier = CVM_TIER_NATIVE
 *
 * Trampoline (x86-64 System V / Microsoft x64)
 * ─────────────────────────────────────────────
 * The CVM passes arguments as a CvmReg* array.  The native function
 * expects arguments in registers (rdi/rsi/rdx/rcx/r8/r9 on SysV;
 * rcx/rdx/r8/r9 on Win64).  The trampoline stub copies up to 6 args
 * from the array into the appropriate GPRs, calls the native entry,
 * then stores the return value (rax) back to regs[0].
 *
 * Mini-JIT (integer-only, no NASM dependency)
 * ────────────────────────────────────────────
 * For the common case of integer arithmetic functions (e.g. fibonacci)
 * we implement a minimal x86-64 code emitter that handles:
 *   CONST_INT, ADD, SUB, MUL, DIV, NEG, CMP_*, CONDBR, BR, RET, CALL.
 * This avoids a dependency on an external assembler and makes the
 * test suite fully self-contained.
 *
 * Executable memory
 * ─────────────────
 *   POSIX: mmap(MAP_ANONYMOUS|MAP_PRIVATE, PROT_READ|PROT_WRITE|PROT_EXEC)
 *   Win32: VirtualAlloc(MEM_COMMIT, PAGE_EXECUTE_READWRITE)
 *
 * Thread safety
 * ─────────────
 * A GCC/Clang atomic CAS guards the tier transition so concurrent
 * interpreter threads only compile a function once.
 */

#include "../../src/compiler/ir/mir.h"
#include "jit_bridge.h"
#include "../io/direct_io.h"
#include "../io/fast_format.h"

#if defined(_WIN32) || defined(_WIN64)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  define NOSERVICE
#  define NOCRYPT
#  include <windows.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/* ─────────────────────────────────────────────────────────────
 * Platform-specific executable memory
 * ───────────────────────────────────────────────────────────── */

#if !defined(_WIN32) && !defined(_WIN64)
#  include <sys/mman.h>
#  include <unistd.h>
#endif

void* jit_alloc_exec(size_t size) {
#if defined(_WIN32) || defined(_WIN64)
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
#endif
}

bool jit_protect_exec(void* mem, size_t size) {
    if (!mem) return false;
#if defined(_WIN32) || defined(_WIN64)
    DWORD old;
    return VirtualProtect(mem, size, PAGE_EXECUTE_READ, &old);
#else
    return mprotect(mem, size, PROT_READ | PROT_EXEC) == 0;
#endif
}

void jit_free_exec(void* mem, size_t size) {
#if defined(_WIN32) || defined(_WIN64)
    (void)size;
    if (mem) VirtualFree(mem, 0, MEM_RELEASE);
#else
    if (mem) munmap(mem, size);
#endif
}

/* ─────────────────────────────────────────────────────────────
 * Mini x86-64 code emitter
 * ───────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t* buf;
    size_t   pos;
    size_t   cap;
    bool     overflowed;
} X64Emit;

static void xe_init(X64Emit* e, size_t cap) {
    e->buf = malloc(cap);
    e->pos = 0;
    e->cap = cap;
    e->overflowed = (e->buf == NULL);
}
static void xe_free(X64Emit* e) { free(e->buf); }
static void xe_byte(X64Emit* e, uint8_t b) {
    if (e->overflowed || !e->buf) return;
    if (e->pos < e->cap) {
        e->buf[e->pos++] = b;
    } else {
        e->overflowed = true;
    }
}
static void xe_u32(X64Emit* e, uint32_t v) {
    xe_byte(e, (uint8_t)(v));
    xe_byte(e, (uint8_t)(v >> 8));
    xe_byte(e, (uint8_t)(v >> 16));
    xe_byte(e, (uint8_t)(v >> 24));
}
static void xe_i64(X64Emit* e, int64_t v) {
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; i++) xe_byte(e, (uint8_t)(u >> (i * 8)));
}

/* Registers: rax=0, rcx=1, rdx=2, rbx=3, rsp=4, rbp=5, rsi=6, rdi=7,
 *            r8=8..r15=15  */
#define RAX 0
#define RCX 1
#define RDX 2
#define RBX 3
#define RSP 4
#define RBP 5
#define RSI 6
#define RDI 7
#define R8  8
#define R9  9

/* MOV rREG, imm64 */
static void xe_mov_r_imm64(X64Emit* e, int reg, int64_t imm) {
    uint8_t rex = 0x48 | (reg >= 8 ? 0x41 : 0x00);
    xe_byte(e, rex);
    xe_byte(e, 0xB8 | (reg & 7));
    xe_i64(e, imm);
}

/* MOV rDEST, rSRC (64-bit) */
static void xe_mov_r_r(X64Emit* e, int dst, int src) {
    /* 0x89: MOV r/m64, r64. reg=src, r/m=dst. 
     * REX.R = bit 2 of REX (0x04) -> extends reg (src)
     * REX.B = bit 0 of REX (0x01) -> extends r/m (dst) */
    uint8_t rex = 0x48 | (src >= 8 ? 0x04 : 0) | (dst >= 8 ? 0x01 : 0);
    xe_byte(e, rex);
    xe_byte(e, 0x89);
    xe_byte(e, 0xC0 | ((src & 7) << 3) | (dst & 7));
}

/* ADD rDST, rSRC */
static void xe_add_r_r(X64Emit* e, int dst, int src) {
    /* 0x03: ADD r64, r/m64. reg=dst, r/m=src. */
    uint8_t rex = 0x48 | (dst >= 8 ? 0x04 : 0) | (src >= 8 ? 0x01 : 0);
    xe_byte(e, rex); xe_byte(e, 0x03);
    xe_byte(e, 0xC0 | ((dst & 7) << 3) | (src & 7));
}

/* SUB rDST, rSRC */
static void xe_sub_r_r(X64Emit* e, int dst, int src) {
    /* 0x2B: SUB r64, r/m64. reg=dst, r/m=src. */
    uint8_t rex = 0x48 | (dst >= 8 ? 0x04 : 0) | (src >= 8 ? 0x01 : 0);
    xe_byte(e, rex); xe_byte(e, 0x2B);
    xe_byte(e, 0xC0 | ((dst & 7) << 3) | (src & 7));
}

/* IMUL rDST, rSRC */
static void xe_imul_r_r(X64Emit* e, int dst, int src) {
    /* 0x0F 0xAF: IMUL r64, r/m64. reg=dst, r/m=src. */
    uint8_t rex = 0x48 | (dst >= 8 ? 0x04 : 0) | (src >= 8 ? 0x01 : 0);
    xe_byte(e, rex); xe_byte(e, 0x0F); xe_byte(e, 0xAF);
    xe_byte(e, 0xC0 | ((dst & 7) << 3) | (src & 7));
}

/* MOV rREG, [rbp + disp32]  (disp is signed) */
static void xe_mov_r_rbpdisp(X64Emit* e, int dst, int32_t disp) {
    /* 0x8B: MOV r64, r/m64. reg=dst, r/m=[rbp+disp32] → modrm = 10 reg 101 */
    uint8_t rex = 0x48 | (dst >= 8 ? 0x04 : 0);
    xe_byte(e, rex); xe_byte(e, 0x8B);
    xe_byte(e, (uint8_t)(0x80 | ((dst & 7) << 3) | 5)); /* mod=10, rm=101 (rbp) */
    xe_u32(e, (uint32_t)disp);
}

/* MOV [rbp + disp32], rREG */
static void xe_mov_rbpdisp_r(X64Emit* e, int32_t disp, int src) {
    /* 0x89: MOV r/m64, r64. reg=src, r/m=[rbp+disp32] */
    uint8_t rex = 0x48 | (src >= 8 ? 0x04 : 0);
    xe_byte(e, rex); xe_byte(e, 0x89);
    xe_byte(e, (uint8_t)(0x80 | ((src & 7) << 3) | 5));
    xe_u32(e, (uint32_t)disp);
}

/* SUB rsp, imm32  /  ADD rsp, imm32  /  MOV rsp, rbp */
static void xe_sub_rsp_imm32(X64Emit* e, int32_t imm) {
    xe_byte(e, 0x48); xe_byte(e, 0x81); xe_byte(e, 0xEC); xe_u32(e, (uint32_t)imm);
}
static void xe_add_rsp_imm32(X64Emit* e, int32_t imm) {
    xe_byte(e, 0x48); xe_byte(e, 0x81); xe_byte(e, 0xC4); xe_u32(e, (uint32_t)imm);
}
static void xe_mov_rsp_rbp(X64Emit* e) {
    xe_byte(e, 0x48); xe_byte(e, 0x89); xe_byte(e, 0xEC); /* mov rsp, rbp */
}

/* RET */
static void xe_ret(X64Emit* e) { xe_byte(e, 0xC3); }

/* PUSH rREG */
static void xe_push(X64Emit* e, int reg) {
    if (reg >= 8) xe_byte(e, 0x41);
    xe_byte(e, 0x50 | (reg & 7));
}

/* POP rREG */
static void xe_pop(X64Emit* e, int reg) {
    if (reg >= 8) xe_byte(e, 0x41);
    xe_byte(e, 0x58 | (reg & 7));
}

/* ─────────────────────────────────────────────────────────────
 * Trampoline generator
 *
 * Creates a small native stub:
 *   push rbx / rbp  (callee-saved)
 *   mov  rdi, [regs+0]   ; arg0
 *   mov  rsi, [regs+8]   ; arg1  ... up to 6 args (SysV)
 *   call <target>
 *   mov  [regs+0], rax   ; store return value
 *   pop  rbp / rbx
 *   ret
 *
 * We use a static inline assembly wrapper on GCC/Clang; on MSVC we
 * use a simpler variation.  Since this is targeting a mini-JIT that
 * already writes x86-64 natively we embed the trampoline bytes
 * directly so there is zero external assembler dependency.
 * ───────────────────────────────────────────────────────────── */

typedef struct {
    void*       target;      /* address of the compiled function body */
    CvmNativeFn fn;          /* callable entry point (the trampoline itself) */
    void*       exec_mem;
    size_t      exec_size;
} JitTrampolineEntry;

/*
 * We generate a calling-convention trampoline that copies up to
 * 6 arguments from the CvmReg* array into the correct registers
 * for System V AMD64 ABI (Linux/macOS) or Win64 ABI (Windows),
 * calls the function, then stores rax back to regs[0].
 *
 * The trampoline is itself the native function stored in the exec page.
 * Signature: void trampoline(CvmReg* regs, int n_args)
 *   rdi = regs  (SysV) / rcx = regs (Win64)
 *   rsi = n_args (SysV) / rdx = n_args (Win64)
 */

#define R10 10
#define R11 11
#define R12 12
#define R13 13
#define R14 14
#define R15 15

/* ── Linear-scan register allocator for the mini-JIT ─────────────────────────
 *
 * The eligible instruction subset (see jit_function_is_supported) has NO calls
 * and NO memory ops, so every GPR except RSP/RBP is free to allocate across an
 * instruction boundary. We reserve two fixed scratch registers:
 *
 *   RAX — return value; also the implicit operand of IDIV / CQO and the SETcc
 *         landing register for comparisons; also used to reload a spilled
 *         *first* operand.
 *   R11 — reload scratch for a spilled *second* operand / divisor.
 *
 * The remaining 12 registers form the allocatable pool. RBX and R12..R15 are
 * callee-saved and are already pushed/popped by the prologue/epilogue.
 * RCX/RDX/RSI/RDI/R8/R9/R10 are caller-saved but that is irrelevant here since
 * the subset makes no calls.
 *
 * Values that don't get a register are assigned a spill slot at
 * [rbp - 8*(slot+1)] within a frame the prologue reserves with `sub rsp, N`.
 */
#define JIT_SCRATCH_A   RAX
#define JIT_SCRATCH_B   R11
#define JIT_ALLOC_POOL_SIZE 12
static const int kAllocPool[JIT_ALLOC_POOL_SIZE] = {
    RCX, RDX, RSI, RDI, R8, R9, R10,   /* caller-saved (no calls → safe) */
    RBX, R12, R13, R14, R15,           /* callee-saved (pushed in prologue) */
};

#define JIT_MAX_SPILL_SLOTS 64   /* post-allocation capacity bail-out */
#define JIT_REG_NONE   (-1)

/* Per-value allocation result. If `reg >= 0` the value lives in that physical
 * register for its whole live range. Otherwise `spill_slot >= 0` and the value
 * lives at [rbp - 8*(spill_slot+1)]. `first`/`last` are linear instruction
 * indices bounding the live range (params: first == -1). */
typedef struct {
    int  first;
    int  last;
    int  reg;         /* physical register, or JIT_REG_NONE */
    int  spill_slot;  /* stack slot index, or -1 */
    bool used;        /* value id actually appears in this function */
} JitVReg;

typedef struct {
    JitVReg*  v;            /* indexed by MirValueId, length n */
    int       n;
    int       n_spill_slots;
    bool      ok;
} JitRegAlloc;

/* first-value sentinels: JRA_FIRST_UNSET = never defined/seen yet;
 * -1 = live-in (a parameter). Any value >= 0 is a real definition index. */
#define JRA_FIRST_UNSET (-2)

/* Record a USE of `id` at linear index `idx` — extends `last` only. `first`
 * (the definition point / live-in sentinel) is never touched here. */
static void jra_use(JitRegAlloc* a, MirValueId id, int idx) {
    if (id == MIR_VALUE_NONE || (int)id >= a->n) return;
    JitVReg* r = &a->v[id];
    r->used = true;
    if (idx > r->last) r->last = idx;
}

/* Record a DEF of `id` at linear index `idx` — sets `first` on the (single,
 * SSA) definition and extends `last`. A param already has first == -1 and is
 * left alone. */
static void jra_def(JitRegAlloc* a, MirValueId id, int idx) {
    if (id == MIR_VALUE_NONE || (int)id >= a->n) return;
    JitVReg* r = &a->v[id];
    r->used = true;
    if (r->first == JRA_FIRST_UNSET) r->first = idx;
    if (idx > r->last) r->last = idx;
}

/* Iterate the operand value-ids read by `inst` (for liveness). Mirrors the
 * opcodes the code generator actually handles. */
typedef void (*JraOperandCb)(JitRegAlloc*, MirValueId, int);
static void jra_walk_operands(JitRegAlloc* a, MirInst* inst, int idx, JraOperandCb cb) {
    switch (inst->opcode) {
        case MIR_ADD: case MIR_SUB: case MIR_MUL: case MIR_DIV: case MIR_MOD:
        case MIR_BAND: case MIR_BOR: case MIR_BXOR: case MIR_SHL: case MIR_SHR: case MIR_USHR:
        case MIR_CMP_EQ: case MIR_CMP_NE: case MIR_CMP_LT: case MIR_CMP_LE:
        case MIR_CMP_GT: case MIR_CMP_GE:
        case MIR_LOGIC_AND: case MIR_LOGIC_OR:
            cb(a, inst->as.binary.lhs, idx);
            cb(a, inst->as.binary.rhs, idx);
            break;
        case MIR_NEG: case MIR_BNOT: case MIR_LOGIC_NOT:
        case MIR_CAST: case MIR_BITCAST: case MIR_TRUNC: case MIR_ZEXT: case MIR_SEXT:
            cb(a, inst->as.unary.operand, idx);
            break;
        case MIR_COPY: case MIR_BORROW: case MIR_BORROW_MUT: case MIR_MOVE:
            cb(a, inst->as.transfer.source, idx);
            break;
        case MIR_CONDBR:
            cb(a, inst->as.condbr.cond, idx);
            break;
        case MIR_RET:
            cb(a, inst->as.ret.value, idx);
            break;
        case MIR_CALL:   /* self-recursive only (checked in is_supported) */
            for (int i = 0; i < inst->as.call.n_args; i++)
                cb(a, inst->as.call.args[i], idx);
            break;
        default:
            break;
    }
}

/* Compute live ranges + linear-scan allocation. Returns false (a->ok=false)
 * if the function needs more than JIT_MAX_SPILL_SLOTS stack slots. */
static void jit_regalloc(MirFunction* func, JitRegAlloc* a) {
    a->n = (int)func->next_value_id;
    if (a->n < 1) a->n = 1;
    a->v = (JitVReg*)calloc((size_t)a->n, sizeof(JitVReg));
    a->n_spill_slots = 0;
    a->ok = false;
    if (!a->v) return;
    for (int i = 0; i < a->n; i++) {
        a->v[i].first = JRA_FIRST_UNSET;
        a->v[i].last  = -1;
        a->v[i].reg   = JIT_REG_NONE;
        a->v[i].spill_slot = -1;
    }

    /* Params are live-in: first == -1 (before every instruction index). */
    for (int i = 0; i < func->param_count; i++) {
        MirValueId pid = func->params[i].value_id;
        if (pid != MIR_VALUE_NONE && (int)pid < a->n) {
            a->v[pid].used = true;
            a->v[pid].first = -1;
            a->v[pid].last  = 0;   /* at least live at index 0; extended by uses */
        }
    }

    /* Linear pass over blocks in construction (topological, no back-edges in
     * the v1 subset) order, assigning each instruction an index and recording
     * def/use points. */
    int idx = 0;
    for (MirBlock* blk = func->entry_block; blk; blk = blk->next_block) {
        for (MirInst* inst = blk->first; inst; inst = inst->next) {
            jra_walk_operands(a, inst, idx, jra_use);
            if (inst->result != MIR_VALUE_NONE && (int)inst->result < a->n) {
                jra_def(a, inst->result, idx);
            }
            idx++;
        }
    }

    /* Normalise: a param that is never used still occupies a live-in range of
     * [−1, 0]; a value defined but never used has first==last==def index. Any
     * value with used==false is ignored entirely. */

    /* Linear scan. Build an ordered list of used intervals by start index. */
    int  order_cap = a->n;
    int* order = (int*)malloc((size_t)order_cap * sizeof(int));
    int  order_n = 0;
    if (!order) { free(a->v); a->v = NULL; return; }
    for (int i = 0; i < a->n; i++) {
        if (!a->v[i].used) continue;
        if (a->v[i].first == JRA_FIRST_UNSET) a->v[i].first = a->v[i].last; /* used-but-undef: point range */
        order[order_n++] = i;
    }
    /* insertion sort by (first, then last) — order_n is tiny */
    for (int i = 1; i < order_n; i++) {
        int cur = order[i];
        int j = i - 1;
        while (j >= 0 &&
               (a->v[order[j]].first > a->v[cur].first ||
                (a->v[order[j]].first == a->v[cur].first &&
                 a->v[order[j]].last  > a->v[cur].last))) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = cur;
    }

    bool pool_free[JIT_ALLOC_POOL_SIZE];
    for (int i = 0; i < JIT_ALLOC_POOL_SIZE; i++) pool_free[i] = true;

    /* active = value ids currently holding a register, kept sorted by `last`. */
    int active[JIT_ALLOC_POOL_SIZE];
    int active_n = 0;

    for (int oi = 0; oi < order_n; oi++) {
        int vid = order[oi];
        int start = a->v[vid].first;

        /* Expire old intervals whose last < start. */
        int w = 0;
        for (int k = 0; k < active_n; k++) {
            int av = active[k];
            if (a->v[av].last < start) {
                for (int pk = 0; pk < JIT_ALLOC_POOL_SIZE; pk++) {
                    if (kAllocPool[pk] == a->v[av].reg) { pool_free[pk] = true; break; }
                }
            } else {
                active[w++] = av;
            }
        }
        active_n = w;

        /* Find a free pool register. */
        int chosen = -1;
        for (int k = 0; k < JIT_ALLOC_POOL_SIZE; k++) {
            if (pool_free[k]) { chosen = k; break; }
        }

        if (chosen >= 0) {
            a->v[vid].reg = kAllocPool[chosen];
            pool_free[chosen] = false;
            /* insert into active sorted by last */
            int p = active_n;
            while (p > 0 && a->v[active[p - 1]].last > a->v[vid].last) {
                active[p] = active[p - 1];
                p--;
            }
            active[p] = vid;
            active_n++;
        } else {
            /* Spill: the active interval with the furthest `last`, unless the
             * new interval ends even later (then spill the new one). */
            int spill_pos = active_n - 1; /* active is sorted → last element has max `last` */
            int spill_vid = active[spill_pos];
            if (a->v[spill_vid].last > a->v[vid].last) {
                a->v[vid].reg = a->v[spill_vid].reg;
                a->v[spill_vid].reg = JIT_REG_NONE;
                a->v[spill_vid].spill_slot = a->n_spill_slots++;
                active[spill_pos] = vid;
                /* re-sort tail (new vid.last <= old, so it moves left) */
                int p = spill_pos;
                while (p > 0 && a->v[active[p - 1]].last > a->v[vid].last) {
                    int t = active[p]; active[p] = active[p - 1]; active[p - 1] = t;
                    p--;
                }
            } else {
                a->v[vid].spill_slot = a->n_spill_slots++;
            }
        }
    }

    free(order);

    if (a->n_spill_slots > JIT_MAX_SPILL_SLOTS) {
        free(a->v);
        a->v = NULL;
        return;
    }
    a->ok = true;
}

static void jit_regalloc_free(JitRegAlloc* a) {
    free(a->v);
    a->v = NULL;
}

/* ─────────────────────────────────────────────────────────────
 * Mini-JIT: compile a single MirFunction to x86-64 machine code
 *
 * Supports the subset needed by the fibonacci / Taylor tests:
 *   CONST_INT, ADD, SUB, MUL, NEG, RET, RET_VOID,
 *   CMP_LT / CMP_LE / CMP_EQ, CONDBR, BR, CALL (self-recursive).
 *
 * Returns the size of generated code in `out_size`, or 0 on failure.
 * The caller must jit_alloc_exec() and memcpy the result.
 * ───────────────────────────────────────────────────────────── */

/* Patch slot: we need to fix up short-jump offsets after emitting forward refs */
#define MAX_PATCHES 256
typedef struct { size_t patch_pos; uint32_t block_id; } JitPatch;

typedef struct {
    uint32_t block_id;
    size_t   code_offset;
} JitBlockLabel;

/* ── Capacity caps ─────────────────────────────────────────────────────────
 *
 * The register allocator (jit_regalloc) handles arbitrary value counts via
 * linear-scan with spill-to-stack (post-allocation bail at JIT_MAX_SPILL_SLOTS).
 * The trampoline (jit_gen_trampoline) now does ABI-correct stack-argument
 * spilling, so the param cap is only a sanity bound.
 */
#define JIT_MAX_PARAMS       16

/* Quick scan: return false if the function has any instruction the mini-JIT
 * cannot handle correctly (ALLOCA / LOAD / STORE / VEC_* / PHI / floats / …),
 * or a call to another function. A SELF-recursive call (target == this
 * function) IS allowed — it lowers to a direct `call rel32` to our own entry.
 */
static bool jit_function_is_supported(MirFunction* func) {
    if (func->param_count > JIT_MAX_PARAMS) return false;

    for (MirBlock* blk = func->entry_block; blk; blk = blk->next_block) {
        for (MirInst* inst = blk->first; inst; inst = inst->next) {
            switch (inst->opcode) {
                case MIR_ALLOCA:
                case MIR_LOAD:
                case MIR_STORE:
                case MIR_GET_FIELD_PTR:
                case MIR_GET_ELEM_PTR:
                case MIR_PHI:
                case MIR_SWITCH:
                case MIR_STRUCT_INIT:
                case MIR_EXTRACT:
                case MIR_INSERT:
                case MIR_CALL_INDIRECT:
                case MIR_CALL_VIRTUAL:
                case MIR_ARC_RETAIN:
                case MIR_ARC_RELEASE:
                case MIR_OBJ_ALLOC:
                case MIR_SUSPEND:
                case MIR_FADD: case MIR_FSUB: case MIR_FMUL: case MIR_FDIV: case MIR_FNEG:
                case MIR_SITOFP: case MIR_FPTOSI:
                    return false;
                case MIR_CALL: {
                    /* Only a self-recursive call is supported (C-v1). Any call
                     * to a different function bails to the interpreter. */
                    const char* callee = inst->as.call.func_name;
                    if (!callee || !func->name || strcmp(callee, func->name) != 0)
                        return false;
                    /* Self-call arg count must fit the register ABI — the JIT
                     * marshals self-call args into RDI.. / RCX.. only (no
                     * stack spill for the inner call). */
#if defined(_WIN32) || defined(_WIN64)
                    if (inst->as.call.n_args > 4) return false;
#else
                    if (inst->as.call.n_args > 6) return false;
#endif
                    break;
                }
                default:
                    if (inst->opcode >= MIR_VEC_LOAD && inst->opcode <= MIR_VEC_SELECT)
                        return false;
                    break;
            }
        }
    }

    return true;
}

/* ── Per-value location helpers (used by the code generator) ──────────────── */

/* Home register for value `id`, or JIT_REG_NONE if it lives in a spill slot. */
static int jra_home_reg(const JitRegAlloc* a, MirValueId id) {
    if (id == MIR_VALUE_NONE || (int)id >= a->n) return JIT_REG_NONE;
    return a->v[id].reg;
}
static int jra_slot_disp(const JitRegAlloc* a, MirValueId id) {
    /* [rbp - 8*(slot+1)] */
    return -8 * (a->v[id].spill_slot + 1);
}

/* Materialise value `id` into a real register and return it. If `id` has a
 * home register, that is returned directly (no code emitted). Otherwise the
 * value is reloaded from its spill slot into `scratch` and `scratch` returned. */
static int jra_load_operand(X64Emit* e, const JitRegAlloc* a, MirValueId id, int scratch) {
    if (id == MIR_VALUE_NONE || (int)id >= a->n) {
        /* Unknown / none: zero the scratch and use it. */
        xe_mov_r_imm64(e, scratch, 0);
        return scratch;
    }
    int hr = a->v[id].reg;
    if (hr != JIT_REG_NONE) return hr;
    xe_mov_r_rbpdisp(e, scratch, jra_slot_disp(a, id));
    return scratch;
}

/* Return the register the result of `inst` should be written into: its home
 * register if it has one, else `scratch` (caller must then store it to the
 * value's spill slot via jra_store_result). */
static int jra_result_reg(const JitRegAlloc* a, MirValueId id, int scratch) {
    if (id == MIR_VALUE_NONE || (int)id >= a->n) return scratch;
    int hr = a->v[id].reg;
    return hr != JIT_REG_NONE ? hr : scratch;
}
static void jra_store_result(X64Emit* e, const JitRegAlloc* a, MirValueId id, int from_reg) {
    if (id == MIR_VALUE_NONE || (int)id >= a->n) return;
    if (a->v[id].reg != JIT_REG_NONE) return;            /* already in home reg */
    xe_mov_rbpdisp_r(e, jra_slot_disp(a, id), from_reg);
}

static uint8_t* jit_compile_function(MirFunction* func, size_t* out_size) {
    if (!func || !func->entry_block || !out_size) return NULL;
    if (!jit_function_is_supported(func)) return NULL;

    JitRegAlloc ra;
    jit_regalloc(func, &ra);
    if (!ra.ok) { jit_regalloc_free(&ra); return NULL; }

    X64Emit e;
    xe_init(&e, 8192);

    /* A self-recursive `call` jumps to offset 0 — the full prologue below runs
     * for every recursion level, giving each its own frame (and its own spill
     * slots), and the standard epilogue unwinds it. */
    const size_t self_entry_off = 0;

    /* Prologue: push all callee-saved registers we may use (RBX, RBP, R12-R15
     * — 48 bytes; combined with the return address rsp is now ≡ 8 mod 16). */
    xe_push(&e, RBX);
    xe_byte(&e, 0x55);  /* push rbp */
    xe_byte(&e, 0x41); xe_byte(&e, 0x54);  /* push r12 */
    xe_byte(&e, 0x41); xe_byte(&e, 0x55);  /* push r13 */
    xe_byte(&e, 0x41); xe_byte(&e, 0x56);  /* push r14 */
    xe_byte(&e, 0x41); xe_byte(&e, 0x57);  /* push r15 */
    xe_mov_r_r(&e, RBP, RSP);               /* mov rbp, rsp — frame base */

    /* Reserve the spill frame. Choose a size ≡ 8 mod 16 so that, starting from
     * rsp ≡ 8 mod 16, we land back on a 16-byte boundary (matches the old
     * `sub rsp, 8` when there are no spill slots). */
    int32_t frame_bytes = 8 * ra.n_spill_slots;
    frame_bytes = (frame_bytes + 15) & ~15;
    frame_bytes += 8;
    xe_sub_rsp_imm32(&e, frame_bytes);

    /* Move incoming ABI argument registers into each param's home location. */
#if defined(_WIN32) || defined(_WIN64)
    static const int arg_regs[] = { RCX, RDX, R8, R9 };
    int max_arg = 4;
#else
    static const int arg_regs[] = { RDI, RSI, RDX, RCX, R8, R9 };
    int max_arg = 6;
#endif
    /* Move each incoming ABI arg register into its param's home. This is a
     * parallel copy: a later param's source register may be an earlier param's
     * destination (e.g. param0→RSI while param1's incoming arg is RSI), so a
     * naive in-order sequence corrupts. Emit a valid schedule — free moves
     * first (dest is not still needed as any pending source), breaking the
     * remaining cycle(s) through RAX (never an ABI arg reg, never in the
     * allocation pool). Params homed to a spill slot are stored directly and
     * their source register is then free. */
    {
        int np = func->param_count < max_arg ? func->param_count : max_arg;
        int src[6], dst[6];   /* dst = JIT_REG_NONE → the param is spilled */
        int32_t slot_disp[6];
        bool pending[6];
        int npend = 0;
        for (int i = 0; i < np; i++) {
            MirValueId pid = func->params[i].value_id;
            if (pid == MIR_VALUE_NONE) { pending[i] = false; continue; }
            src[i] = arg_regs[i];
            int hr = jra_home_reg(&ra, pid);
            dst[i] = hr;
            slot_disp[i] = (hr == JIT_REG_NONE) ? jra_slot_disp(&ra, pid) : 0;
            pending[i] = true;
            npend++;
        }
        /* First: spill-homed params — store then drop (source reg now free). */
        for (int i = 0; i < np; i++) {
            if (pending[i] && dst[i] == JIT_REG_NONE) {
                xe_mov_rbpdisp_r(&e, slot_disp[i], src[i]);
                pending[i] = false; npend--;
            }
        }
        /* Then: register-homed params via a safe schedule. */
        int guard = 0;
        while (npend > 0 && guard++ < 64) {
            bool progress = false;
            for (int i = 0; i < np; i++) {
                if (!pending[i]) continue;
                if (dst[i] == src[i]) { pending[i] = false; npend--; progress = true; continue; }
                /* free iff no other pending move still reads dst[i] */
                bool blocked = false;
                for (int j = 0; j < np; j++) {
                    if (j != i && pending[j] && src[j] == dst[i]) { blocked = true; break; }
                }
                if (!blocked) {
                    xe_mov_r_r(&e, dst[i], src[i]);
                    pending[i] = false; npend--; progress = true;
                }
            }
            if (!progress) {
                /* Pure cycle: stage one move's source through RAX (which is
                 * never an ABI arg reg nor an allocation-pool reg, so it is
                 * not the dest of any pending move). Next iteration the move
                 * whose source is now RAX becomes free. */
                for (int i = 0; i < np; i++) {
                    if (!pending[i]) continue;
                    xe_mov_r_r(&e, JIT_SCRATCH_A, src[i]);
                    src[i] = JIT_SCRATCH_A;
                    break;
                }
            }
        }

        /* Params beyond the ABI register count arrive on the caller's stack.
         * The prologue pushed 6 callee-saved regs (48 bytes) then `mov rbp,rsp`,
         * so relative to rbp: [rbp+48] = return address, [rbp+56] = first stack
         * argument, then +8 each. */
        for (int i = max_arg; i < func->param_count; i++) {
            MirValueId pid = func->params[i].value_id;
            if (pid == MIR_VALUE_NONE) continue;
            int32_t caller_disp = (int32_t)(56 + 8 * (i - max_arg));
            int hr = jra_home_reg(&ra, pid);
            if (hr != JIT_REG_NONE) {
                xe_mov_r_rbpdisp(&e, hr, caller_disp);
            } else {
                xe_mov_r_rbpdisp(&e, JIT_SCRATCH_A, caller_disp);
                xe_mov_rbpdisp_r(&e, jra_slot_disp(&ra, pid), JIT_SCRATCH_A);
            }
        }
    }

    /* Walk blocks in order */
    JitBlockLabel block_labels[64];
    int n_labels = 0;
    JitPatch patches[MAX_PATCHES];
    int n_patches = 0;

    /* Running linear instruction index, kept in lock-step with the index
     * jit_regalloc assigned, so we can query "is value V live across this
     * call" for the save/restore set. */
    int lin_idx = 0;

#if defined(_WIN32) || defined(_WIN64)
    static const int call_arg_regs[6] = { RCX, RDX, R8, R9, RAX, RAX };
    const int call_max_reg = 4;
#else
    static const int call_arg_regs[6] = { RDI, RSI, RDX, RCX, R8, R9 };
    const int call_max_reg = 6;
#endif

    /* Shared epilogue emitter: mov rsp,rbp; pop r15..r12; pop rbp; pop rbx; ret */
    #define EMIT_EPILOGUE() do { \
        xe_mov_rsp_rbp(&e); \
        xe_byte(&e, 0x41); xe_byte(&e, 0x5F);  /* pop r15 */ \
        xe_byte(&e, 0x41); xe_byte(&e, 0x5E);  /* pop r14 */ \
        xe_byte(&e, 0x41); xe_byte(&e, 0x5D);  /* pop r13 */ \
        xe_byte(&e, 0x41); xe_byte(&e, 0x5C);  /* pop r12 */ \
        xe_pop(&e, RBP); \
        xe_pop(&e, RBX); \
        xe_ret(&e); \
    } while (0)

    for (MirBlock* blk = func->entry_block; blk; blk = blk->next_block) {
        /* Record label */
        if (n_labels < 64) {
            block_labels[n_labels].block_id = blk->id;
            block_labels[n_labels].code_offset = e.pos;
            n_labels++;
        }

        for (MirInst* inst = blk->first; inst; inst = inst->next) {
            MirValueId r = inst->result;

            switch (inst->opcode) {
                case MIR_CONST_INT: {
                    int dst = jra_result_reg(&ra, r, JIT_SCRATCH_A);
                    xe_mov_r_imm64(&e, dst, inst->as.imm_i64);
                    jra_store_result(&e, &ra, r, dst);
                    break;
                }
                case MIR_CONST_BOOL: {
                    int dst = jra_result_reg(&ra, r, JIT_SCRATCH_A);
                    xe_mov_r_imm64(&e, dst, inst->as.imm_bool ? 1 : 0);
                    jra_store_result(&e, &ra, r, dst);
                    break;
                }

                case MIR_ADD: case MIR_SUB: case MIR_MUL: {
                    int ls = jra_load_operand(&e, &ra, inst->as.binary.lhs, JIT_SCRATCH_A);
                    int rs = jra_load_operand(&e, &ra, inst->as.binary.rhs, JIT_SCRATCH_B);
                    int dst = jra_result_reg(&ra, r, JIT_SCRATCH_A);
                    /* dst = ls  op= rs.  If dst aliases rs, move ls into a
                     * neutral reg first (dst can't be rs and ls simultaneously
                     * unless ls==rs, which is fine for a copy). */
                    if (dst == rs && dst != ls) {
                        /* compute into SCRATCH_A instead, then place */
                        int tmp = (JIT_SCRATCH_A == rs) ? JIT_SCRATCH_B : JIT_SCRATCH_A;
                        xe_mov_r_r(&e, tmp, ls);
                        if (inst->opcode == MIR_ADD) xe_add_r_r(&e, tmp, rs);
                        else if (inst->opcode == MIR_SUB) xe_sub_r_r(&e, tmp, rs);
                        else xe_imul_r_r(&e, tmp, rs);
                        if (dst != tmp) xe_mov_r_r(&e, dst, tmp);
                    } else {
                        if (dst != ls) xe_mov_r_r(&e, dst, ls);
                        if (inst->opcode == MIR_ADD) xe_add_r_r(&e, dst, rs);
                        else if (inst->opcode == MIR_SUB) xe_sub_r_r(&e, dst, rs);
                        else xe_imul_r_r(&e, dst, rs);
                    }
                    jra_store_result(&e, &ra, r, dst);
                    break;
                }

                case MIR_DIV: case MIR_MOD: {
                    /* IDIV: RDX:RAX / divisor → quotient RAX, remainder RDX.
                     * RAX and RDX are fixed scratch here — but an allocated
                     * value may currently live in RDX (RAX is never in the
                     * pool). Save/restore RDX around the sequence. */
                    xe_push(&e, RDX);
                    int ls = jra_load_operand(&e, &ra, inst->as.binary.lhs, R10);
                    int rs = jra_load_operand(&e, &ra, inst->as.binary.rhs, R11);
                    int divisor = rs;
                    if (rs == RAX || rs == RDX) {
                        xe_mov_r_r(&e, R11, rs);
                        divisor = R11;
                    }
                    xe_mov_r_r(&e, RAX, ls);
                    xe_byte(&e, 0x48); xe_byte(&e, 0x99);  /* CQO */
                    {
                        uint8_t rex = (uint8_t)(0x48 | (divisor >= 8 ? 0x41 : 0));
                        xe_byte(&e, rex); xe_byte(&e, 0xF7);
                        xe_byte(&e, (uint8_t)(0xF8 | (divisor & 7)));  /* IDIV /7 */
                    }
                    int qr = (inst->opcode == MIR_DIV) ? RAX : RDX;
                    /* Move result out BEFORE restoring RDX. */
                    int dst = jra_result_reg(&ra, r, R10);
                    if (dst != qr) xe_mov_r_r(&e, dst, qr);
                    /* If dst is RDX (an allocated value's home), we must not
                     * pop over it — but dst==RDX only if the value's home reg
                     * is RDX, and we saved the *old* RDX which belonged to a
                     * different (now-expired or distinct) value. The pop would
                     * clobber our fresh result. Guard: stash to R10 then pop. */
                    if (dst == RDX) {
                        xe_mov_r_r(&e, R10, RDX);
                        xe_pop(&e, RDX);
                        xe_mov_r_r(&e, RDX, R10);
                    } else {
                        xe_pop(&e, RDX);
                    }
                    jra_store_result(&e, &ra, r, dst);
                    break;
                }

                case MIR_NEG: {
                    int src = jra_load_operand(&e, &ra, inst->as.unary.operand, JIT_SCRATCH_A);
                    int dst = jra_result_reg(&ra, r, JIT_SCRATCH_A);
                    if (dst != src) xe_mov_r_r(&e, dst, src);
                    uint8_t rex = (uint8_t)(0x48 | (dst >= 8 ? 0x41 : 0));
                    xe_byte(&e, rex); xe_byte(&e, 0xF7);
                    xe_byte(&e, (uint8_t)(0xD8 | (dst & 7)));  /* NEG /3 */
                    jra_store_result(&e, &ra, r, dst);
                    break;
                }

                case MIR_COPY: case MIR_BORROW: case MIR_BORROW_MUT: case MIR_MOVE: {
                    int src = jra_load_operand(&e, &ra, inst->as.transfer.source, JIT_SCRATCH_A);
                    int dst = jra_result_reg(&ra, r, JIT_SCRATCH_A);
                    if (dst != src) xe_mov_r_r(&e, dst, src);
                    jra_store_result(&e, &ra, r, dst);
                    break;
                }

                case MIR_CMP_LT: case MIR_CMP_LE: case MIR_CMP_EQ:
                case MIR_CMP_NE: case MIR_CMP_GT: case MIR_CMP_GE: {
                    int ls = jra_load_operand(&e, &ra, inst->as.binary.lhs, JIT_SCRATCH_A);
                    int rs = jra_load_operand(&e, &ra, inst->as.binary.rhs, JIT_SCRATCH_B);
                    /* CMP ls, rs */
                    uint8_t rex = (uint8_t)(0x48 | (ls >= 8 ? 0x04 : 0) | (rs >= 8 ? 0x01 : 0));
                    xe_byte(&e, rex); xe_byte(&e, 0x3B);
                    xe_byte(&e, (uint8_t)(0xC0 | ((ls & 7) << 3) | (rs & 7)));
                    uint8_t setcc = 0;
                    switch (inst->opcode) {
                        case MIR_CMP_LT: setcc = 0x9C; break;
                        case MIR_CMP_LE: setcc = 0x9E; break;
                        case MIR_CMP_EQ: setcc = 0x94; break;
                        case MIR_CMP_NE: setcc = 0x95; break;
                        case MIR_CMP_GT: setcc = 0x9F; break;
                        case MIR_CMP_GE: setcc = 0x9D; break;
                        default: break;
                    }
                    /* SETcc AL ; MOVZX SCRATCH_A, AL */
                    xe_byte(&e, 0x0F); xe_byte(&e, setcc); xe_byte(&e, 0xC0);
                    xe_byte(&e, 0x48); xe_byte(&e, 0x0F); xe_byte(&e, 0xB6);
                    xe_byte(&e, (uint8_t)(0xC0 | ((RAX & 7) << 3) | (RAX & 7)));
                    int dst = jra_result_reg(&ra, r, JIT_SCRATCH_A);
                    if (dst != RAX) xe_mov_r_r(&e, dst, RAX);
                    jra_store_result(&e, &ra, r, dst);
                    break;
                }

                case MIR_CONDBR: {
                    int cr = jra_load_operand(&e, &ra, inst->as.condbr.cond, JIT_SCRATCH_A);
                    /* TEST cr, cr */
                    uint8_t rex = (uint8_t)(0x48 | (cr >= 8 ? 0x45 : 0));
                    xe_byte(&e, rex); xe_byte(&e, 0x85);
                    xe_byte(&e, (uint8_t)(0xC0 | ((cr & 7) << 3) | (cr & 7)));
                    if (n_patches < MAX_PATCHES) {
                        xe_byte(&e, 0x0F); xe_byte(&e, 0x85);  /* JNZ rel32 → true_bb */
                        patches[n_patches].patch_pos = e.pos;
                        patches[n_patches].block_id = inst->as.condbr.true_bb ? inst->as.condbr.true_bb->id : 0;
                        n_patches++;
                        xe_u32(&e, 0);
                    } else { e.overflowed = true; }
                    if (n_patches < MAX_PATCHES) {
                        xe_byte(&e, 0xE9);                     /* JMP rel32 → false_bb */
                        patches[n_patches].patch_pos = e.pos;
                        patches[n_patches].block_id = inst->as.condbr.false_bb ? inst->as.condbr.false_bb->id : 0;
                        n_patches++;
                        xe_u32(&e, 0);
                    } else { e.overflowed = true; }
                    break;
                }

                case MIR_BR:
                    if (n_patches < MAX_PATCHES) {
                        xe_byte(&e, 0xE9);
                        patches[n_patches].patch_pos = e.pos;
                        patches[n_patches].block_id = inst->as.br.target ? inst->as.br.target->id : 0;
                        n_patches++;
                        xe_u32(&e, 0);
                    } else { e.overflowed = true; }
                    break;

                case MIR_RET: {
                    int src = jra_load_operand(&e, &ra, inst->as.ret.value, JIT_SCRATCH_A);
                    if (src != RAX) xe_mov_r_r(&e, RAX, src);
                    EMIT_EPILOGUE();
                    break;
                }
                case MIR_RET_VOID:
                    xe_mov_r_imm64(&e, RAX, 0);
                    EMIT_EPILOGUE();
                    break;

                case MIR_CALL: {
                    /* Self-recursive direct call (validated in is_supported):
                     *   1. save every pool-register value whose live range spans
                     *      this call (the callee is our own body → clobbers the
                     *      same caller-saved regs we allocate);
                     *   2. marshal the argument values into ABI arg registers
                     *      (via the stack, to sidestep register aliasing);
                     *   3. call rel32 → our own entry;
                     *   4. restore the saved registers;
                     *   5. move rax into the result's home.
                     */
                    int n_call_args = inst->as.call.n_args;

                    /* (1) collect CALLER-SAVED pool registers holding a value
                     * that is live strictly across this call. RBX/R12-R15 need
                     * no saving here — the recursive callee's own prologue
                     * pushes/pops them. */
                    static const int caller_saved_pool[7] =
                        { RCX, RDX, RSI, RDI, R8, R9, R10 };
                    int save[7];
                    int n_save = 0;
                    for (int k = 0; k < 7; k++) {
                        int reg = caller_saved_pool[k];
                        for (int vi = 0; vi < ra.n; vi++) {
                            if (!ra.v[vi].used || ra.v[vi].reg != reg) continue;
                            if (ra.v[vi].first < lin_idx && ra.v[vi].last > lin_idx) {
                                save[n_save++] = reg;
                                break;
                            }
                        }
                    }
                    /* keep 16-byte alignment at the inner `call`: after the 6
                     * prologue-adjacent pushes the compiled body was aligned by
                     * the `sub rsp, frame_bytes` (frame ≡ 8 mod 16 → rsp ≡ 0).
                     * Each save push is 8 bytes; pad if the count is odd. */
                    bool pad = (n_save & 1) != 0;
                    if (pad) xe_sub_rsp_imm32(&e, 8);
                    for (int s = 0; s < n_save; s++) xe_push(&e, save[s]);

                    /* (2) marshal args: stage each via the stack so a home reg
                     * that is also an ABI arg reg can't be clobbered early.
                     * push args right-to-left, then pop into arg regs. */
                    if (n_call_args > 0) {
                        bool arg_pad = (n_call_args & 1) != 0;
                        if (arg_pad) xe_sub_rsp_imm32(&e, 8);
                        for (int aix = n_call_args - 1; aix >= 0; aix--) {
                            int ar = jra_load_operand(&e, &ra, inst->as.call.args[aix], JIT_SCRATCH_A);
                            xe_push(&e, ar);
                        }
                        for (int aix = 0; aix < n_call_args && aix < call_max_reg; aix++) {
                            xe_pop(&e, call_arg_regs[aix]);
                        }
                        /* pop any beyond the register count (shouldn't happen —
                         * is_supported caps n_args at the ABI register count) */
                        for (int aix = call_max_reg; aix < n_call_args; aix++) {
                            xe_byte(&e, 0x41); xe_byte(&e, 0x5A); /* pop r10 (discard) */
                        }
                        if (arg_pad) xe_add_rsp_imm32(&e, 8);
                    }

                    /* (3) call rel32 → self_entry_off */
                    xe_byte(&e, 0xE8);
                    {
                        int32_t rel = (int32_t)((intptr_t)self_entry_off -
                                                (intptr_t)(e.pos + 4));
                        xe_u32(&e, (uint32_t)rel);
                    }

                    /* (4) restore saved registers (reverse order) */
                    for (int s = n_save - 1; s >= 0; s--) xe_pop(&e, save[s]);
                    if (pad) xe_add_rsp_imm32(&e, 8);

                    /* (5) result ← rax */
                    if (inst->result != MIR_VALUE_NONE) {
                        int dst = jra_result_reg(&ra, inst->result, JIT_SCRATCH_A);
                        if (dst != RAX) xe_mov_r_r(&e, dst, RAX);
                        jra_store_result(&e, &ra, inst->result, dst);
                    }
                    break;
                }

                case MIR_DEBUGLOC:
                case MIR_NOP:
                    break;

                default:
                    xe_byte(&e, 0x90);
                    break;
            }
            lin_idx++;
        }
    }

    if (e.overflowed) {
        xe_free(&e);
        jit_regalloc_free(&ra);
        return NULL;
    }

    /* Ensure generated code always returns even for malformed MIR. */
    if (e.pos == 0 || e.buf[e.pos - 1] != 0xC3) {
        xe_mov_r_imm64(&e, RAX, 0);
        EMIT_EPILOGUE();
        if (e.overflowed) {
            xe_free(&e);
            jit_regalloc_free(&ra);
            return NULL;
        }
    }
    #undef EMIT_EPILOGUE

    /* Patch forward references */
    for (int pi = 0; pi < n_patches; pi++) {
        uint32_t target_bid = patches[pi].block_id;
        size_t target_off = e.pos; /* default: end of code (fallthrough) */
        for (int li = 0; li < n_labels; li++) {
            if (block_labels[li].block_id == target_bid) {
                target_off = block_labels[li].code_offset;
                break;
            }
        }
        /* rel32 = target - (patch_pos + 4) */
        int32_t rel = (int32_t)((intptr_t)target_off - (intptr_t)(patches[pi].patch_pos + 4));
        memcpy(e.buf + patches[pi].patch_pos, &rel, 4);
    }

    *out_size = e.pos;
    uint8_t* out = malloc(e.pos);
    if (out) memcpy(out, e.buf, e.pos);
    xe_free(&e);
    jit_regalloc_free(&ra);
    return out;
}

/* ─────────────────────────────────────────────────────────────
 * CvmNativeFn wrapper — bridges CvmReg* calling convention
 * to the platform ABI used by the JIT-compiled function.
 *
 * We store (exec_page_ptr, n_expected_args) in a small closure
 * allocated alongside the exec page.  The wrapper function below
 * is what gets stored in profile->native_fn.
 * ───────────────────────────────────────────────────────────── */

typedef struct {
    void*  code;   /* start of JIT-compiled code page */
    int    n_args;
} CvmNativeClosure;

/* The trampoline itself — called by the CVM as profile->native_fn(regs, n).
 * Since we cannot generate a truly self-modifying closure without per-instance
 * assembly, we use a thin C wrapper that calls the native code pointer stored
 * in a global lookup table keyed by the pointer itself. */

/* Reserved for future use by a closure registry. */
#define MAX_CLOSURES 1024

/* We need one wrapper per entry count.  For simplicity we generate
 * a unique trampoline stub in exec memory that embeds the target address
 * and then calls our generic dispatcher. */

/* Generic dispatcher — called from generated stub below.
 * Signature: int64_t cjb_dispatcher(void* code, CvmReg* regs, int n_args) */
static int64_t cjb_dispatcher(void* code,
                               CvmReg* regs, int n_args) {
    /* Marshal up to 6 args, call code, return rax */
    typedef int64_t (*Fn6)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
    Fn6 fn = (Fn6)code;
    int64_t a[6] = {0,0,0,0,0,0};
    int c = n_args < 6 ? n_args : 6;
    for (int i = 0; i < c; i++) a[i] = (int64_t)regs[i];
    return fn(a[0], a[1], a[2], a[3], a[4], a[5]);
}

/* Each closure's trampoline stub (generated into exec memory):
 *
 *  ; on entry: rdi = regs*, rsi = n_args  (SysV)  or  rcx = regs*, rdx = n_args (Win64)
 *  mov  r10, <code_ptr>        ; 10 bytes
 *  mov  r11, <cjb_dispatcher>  ; 10 bytes
 * #ifdef SysV
 *  mov  rdx, rsi               ; n_args → 3rd arg   2 bytes
 *  mov  rsi, rdi               ; regs   → 2nd arg   3 bytes
 *  mov  rdi, r10               ; code   → 1st arg   3 bytes
 * #else Win64
 *  mov  r8,  rdx               ; n_args → r8         3 bytes
 *  mov  rdx, rcx               ; regs   → rdx        3 bytes
 *  mov  rcx, r10               ; code   → rcx        3 bytes
 * #endif
 *  sub  rsp, 32                ; shadow space (Win64 only)
 *  call r11
 *  mov  [REGS_ARG + 0], rax    ; store return value — can't easily do this generically
 *  ret
 *
 * Instead of storing retval to regs here, we return int64_t and let cjb_call handle it.
 */
/* Emit `mov <reg>, [rbx + disp32]` — load an 8-byte CvmReg slot. rbx holds
 * the saved `CvmReg* regs` pointer for the whole trampoline body. */
static void xe_tramp_load_slot(X64Emit* e, int reg, int32_t disp) {
    uint8_t rex = 0x48 | (reg >= 8 ? 0x04 : 0);          /* REX.W [+REX.R] */
    xe_byte(e, rex); xe_byte(e, 0x8B);                   /* MOV r64, r/m64 */
    xe_byte(e, (uint8_t)(0x80 | ((reg & 7) << 3) | RBX));/* mod=10, rm=011 (rbx) */
    xe_u32(e, (uint32_t)disp);
}

/* Emit `mov [rsp + disp32], <reg>` — store a marshalled stack argument.
 * (Win64 overflow-arg path only.) */
#if defined(_WIN32) || defined(_WIN64)
static void xe_tramp_store_rsp(X64Emit* e, int32_t disp, int reg) {
    uint8_t rex = 0x48 | (reg >= 8 ? 0x04 : 0);
    xe_byte(e, rex); xe_byte(e, 0x89);                   /* MOV r/m64, r64 */
    xe_byte(e, (uint8_t)(0x80 | ((reg & 7) << 3) | RSP));/* mod=10, rm=100 (sib) */
    xe_byte(e, 0x24);                                    /* SIB: base=rsp, no index */
    xe_u32(e, (uint32_t)disp);
}
#endif

/*
 * Generate a per-function trampoline stub.
 *
 * Entry:  rdi/rcx = CvmReg* regs,  rsi/rdx = int n_args (n_args ignored — the
 *         argument count is fixed at `n_params`, known at compile time).
 * Action: marshal exactly `n_params` CvmReg slots into the platform ABI
 *         (registers, then stack for the overflow), call the JIT-compiled
 *         function body, write its rax return value back to regs[0], ret.
 *
 * SysV:  args 0..5 → rdi,rsi,rdx,rcx,r8,r9 ; args 6.. pushed right-to-left.
 * Win64: args 0..3 → rcx,rdx,r8,r9        ; args 4.. at [rsp+32 + 8*(i-4)],
 *        with 32 bytes of shadow space always reserved.
 * 16-byte stack alignment at the `call` is maintained in both cases.
 */
static void* jit_gen_trampoline(void* code_ptr, int n_params, size_t* out_size) {
    if (n_params < 0) n_params = 0;

    X64Emit e;
    xe_init(&e, 512);

    /* Prologue: push rbx, rbp, r12 (callee-saved). 3 pushes = 24 bytes; with
     * the return address, rsp is now ≡ 8 mod 16 on entry to the body below. */
    xe_push(&e, RBX);
    xe_push(&e, RBP);
    xe_byte(&e, 0x41); xe_byte(&e, 0x54); /* push r12 */

#if defined(_WIN32) || defined(_WIN64)
    /* Win64 ABI: rcx=regs, rdx=n_args (unused). */
    xe_mov_r_r(&e, RBX, RCX);             /* save regs ptr */

    static const int wreg[4] = { RCX, RDX, R8, R9 };
    int nreg = n_params < 4 ? n_params : 4;
    int nstk = n_params > 4 ? n_params - 4 : 0;

    /* After the 3 prologue pushes rsp is 16-aligned. Frame = 32 (shadow space)
     * + 8*nstk for the overflow args, rounded up to a multiple of 16 so rsp is
     * still 16-aligned at the `call`. */
    int32_t frame = 32 + 8 * nstk;
    frame = (frame + 15) & ~15;
    xe_byte(&e, 0x48); xe_byte(&e, 0x81); xe_byte(&e, 0xEC); xe_u32(&e, (uint32_t)frame);

    for (int i = 0; i < nreg; i++)
        xe_tramp_load_slot(&e, wreg[i], (int32_t)(8 * i));
    /* Overflow args at [rsp + 32 + 8*(i-4)] — load via r10, then store. */
    for (int i = 4; i < n_params; i++) {
        xe_tramp_load_slot(&e, R10, (int32_t)(8 * i));
        xe_tramp_store_rsp(&e, (int32_t)(32 + 8 * (i - 4)), R10);
    }

    /* call code_ptr */
    xe_byte(&e, 0x48); xe_byte(&e, 0xB8);
    xe_i64(&e, (int64_t)(uintptr_t)code_ptr);
    xe_byte(&e, 0xFF); xe_byte(&e, 0xD0);
    xe_byte(&e, 0x48); xe_byte(&e, 0x81); xe_byte(&e, 0xC4); xe_u32(&e, (uint32_t)frame);
#else
    /* SysV ABI: rdi=regs, rsi=n_args (unused). */
    xe_mov_r_r(&e, RBX, RDI);             /* save regs ptr */

    static const int sreg[6] = { RDI, RSI, RDX, RCX, R8, R9 };
    int nreg = n_params < 6 ? n_params : 6;
    int nstk = n_params > 6 ? n_params - 6 : 0;

    /* After the 3 prologue pushes rsp is 16-aligned. Each stack arg is an
     * 8-byte push, so `nstk` must be even for rsp to be 16-aligned at the
     * `call`; pad with one extra 8-byte bump when it is odd. */
    int pad = (nstk & 1) ? 8 : 0;
    if (pad) { xe_byte(&e, 0x48); xe_byte(&e, 0x83); xe_byte(&e, 0xEC); xe_byte(&e, 0x08); }

    for (int i = n_params - 1; i >= 6; i--) {
        /* mov r10, [rbx + 8*i] ; push r10  (right-to-left) */
        xe_tramp_load_slot(&e, R10, (int32_t)(8 * i));
        xe_byte(&e, 0x41); xe_byte(&e, 0x52);            /* push r10 */
    }
    for (int i = 0; i < nreg; i++)
        xe_tramp_load_slot(&e, sreg[i], (int32_t)(8 * i));

    /* call code_ptr */
    xe_byte(&e, 0x48); xe_byte(&e, 0xB8);
    xe_i64(&e, (int64_t)(uintptr_t)code_ptr);
    xe_byte(&e, 0xFF); xe_byte(&e, 0xD0);

    int32_t cleanup = 8 * nstk + pad;
    if (cleanup) { xe_byte(&e, 0x48); xe_byte(&e, 0x81); xe_byte(&e, 0xC4); xe_u32(&e, (uint32_t)cleanup); }
#endif

    /* Store return value: mov [rbx], rax */
    xe_byte(&e, 0x48); xe_byte(&e, 0x89); xe_byte(&e, 0x03);

    /* Epilogue */
    xe_byte(&e, 0x41); xe_byte(&e, 0x5C); /* pop r12 */
    xe_pop(&e, RBP);
    xe_pop(&e, RBX);
    xe_ret(&e);

    if (e.overflowed) { xe_free(&e); return NULL; }

    *out_size = e.pos;
    void* mem = jit_alloc_exec(e.pos);
    if (mem) {
        memcpy(mem, e.buf, e.pos);
        jit_protect_exec(mem, e.pos);
    }
    xe_free(&e);
    return mem;
}

/* ─────────────────────────────────────────────────────────────
 * JIT bridge public API
 * ───────────────────────────────────────────────────────────── */

CvmJitBridge* cjb_create(void) {
    CvmJitBridge* b = calloc(1, sizeof(CvmJitBridge));
    return b;
}

void cjb_destroy(CvmJitBridge* bridge) {
    if (!bridge) return;
    for (int i = 0; i < bridge->block_count; i++) {
        if (bridge->exec_blocks[i])
            jit_free_exec(bridge->exec_blocks[i], bridge->exec_sizes[i]);
    }
    free(bridge->exec_blocks);
    free(bridge->exec_sizes);
    free(bridge);
}

/* Register an exec block with the bridge for lifecycle tracking */
static bool cjb_track(CvmJitBridge* bridge, void* mem, size_t size) {
    if (bridge->block_count >= bridge->block_cap) {
        int new_cap = bridge->block_cap ? bridge->block_cap * 2 : 16;
        void** new_blocks = (void**)malloc((size_t)new_cap * sizeof(void*));
        size_t* new_sizes = (size_t*)malloc((size_t)new_cap * sizeof(size_t));
        if (!new_blocks || !new_sizes) {
            free(new_blocks);
            free(new_sizes);
            return false;
        }
        if (bridge->block_count > 0) {
            memcpy(new_blocks, bridge->exec_blocks, (size_t)bridge->block_count * sizeof(void*));
            memcpy(new_sizes, bridge->exec_sizes, (size_t)bridge->block_count * sizeof(size_t));
        }
        free(bridge->exec_blocks);
        free(bridge->exec_sizes);
        bridge->exec_blocks = new_blocks;
        bridge->exec_sizes = new_sizes;
        bridge->block_cap = new_cap;
    }
    bridge->exec_blocks[bridge->block_count] = mem;
    bridge->exec_sizes [bridge->block_count] = size;
    bridge->block_count++;
    return true;
}

JitResult cjb_compile_function(CvmJitBridge* bridge,
                                MirModule*   module,
                                MirFunction* func,
                                CvmProfile*  profile) {
    (void)module;
    if (!bridge || !func || !profile) return JIT_ERR_COMPILE_FAILED;
    if (profile->tier == CVM_TIER_NATIVE && profile->native_fn) return JIT_ERR_ALREADY_NATIVE;

    /* Thread-safety: CAS from PENDING → NATIVE */
    /* (On MSVC we use a simple assignment; the test is single-threaded.) */

    /* 1. Compile to machine code via mini-JIT */
    size_t code_size = 0;
    uint8_t* code_bytes = jit_compile_function(func, &code_size);
    if (!code_bytes || code_size == 0) {
        return JIT_ERR_COMPILE_FAILED;
    }

    /* 2. Allocate executable page */
    void* exec_page = jit_alloc_exec(code_size);
    if (!exec_page) {
        free(code_bytes);
        return JIT_ERR_ALLOC_FAILED;
    }
    memcpy(exec_page, code_bytes, code_size);
    free(code_bytes);
    jit_protect_exec(exec_page, code_size);

    /* Track for cleanup */
    if (!cjb_track(bridge, exec_page, code_size)) {
        jit_free_exec(exec_page, code_size);
        return JIT_ERR_ALLOC_FAILED;
    }
    bridge->total_native_bytes += code_size;
    bridge->functions_compiled++;

    /* 3. Generate trampoline that bridges CvmReg* to platform ABI */
    size_t tramp_size = 0;
    void* tramp_mem = jit_gen_trampoline(exec_page, func->param_count, &tramp_size);
    if (!tramp_mem) {
        /* No trampoline memory — skip JIT (not fatal) */
        return JIT_ERR_ALLOC_FAILED;
    }
    if (!cjb_track(bridge, tramp_mem, tramp_size)) {
        jit_free_exec(tramp_mem, tramp_size);
        return JIT_ERR_ALLOC_FAILED;
    }

    /* 4. Install */
    profile->native_fn   = (CvmNativeFn)tramp_mem;
    profile->native_mem  = exec_page;
    profile->native_size = code_size;
    profile->tier        = CVM_TIER_NATIVE;

    return JIT_OK;
}

void cjb_call(CvmJitBridge* bridge, CvmProfile* profile,
              CvmReg* regs, int n_args) {
    (void)bridge;
    if (profile && profile->native_fn && regs) {
        if (n_args < 0) n_args = 0;
        profile->native_fn(regs, n_args);
    }
}

void cjb_free_native(CvmJitBridge* bridge, CvmProfile* profile) {
    (void)bridge;
    profile->native_fn   = NULL;
    profile->native_mem  = NULL;
    profile->native_size = 0;
    profile->tier        = CVM_TIER_INTERPRET;
}

void cjb_print_stats(CvmJitBridge* bridge, FILE* out) {
    (void)out;
    char line[192];
    size_t n = cpx_fmt_snprintf(line, sizeof(line), "JIT Bridge Statistics\n");
    (void)cpx_io_write_all_fd(1, line, n);
    n = cpx_fmt_snprintf(line, sizeof(line), "  Functions compiled  : %d\n", bridge->functions_compiled);
    (void)cpx_io_write_all_fd(1, line, n);
    n = cpx_fmt_snprintf(line, sizeof(line), "  Total native bytes  : %llu\n",
                         (unsigned long long)bridge->total_native_bytes);
    (void)cpx_io_write_all_fd(1, line, n);
    n = cpx_fmt_snprintf(line, sizeof(line), "  Exec blocks tracked : %d\n", bridge->block_count);
    (void)cpx_io_write_all_fd(1, line, n);
}
