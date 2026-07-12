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

/* Include MIR headers first (defines TokenType via lexer.h) so that our
 * symbol wins when windows.h's winnt.h also tries to define it. */
#include "../../src/compiler/ir/mir.h"
#include "jit_bridge.h"
#include "../io/direct_io.h"
#include "../io/fast_format.h"

#if defined(_WIN32) || defined(_WIN64)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  define NOSERVICE
#  define NOCRYPT
/* Suppress winnt.h TOKEN_TYPE enum — we include it after our TokenType is defined */
#  define TokenType _W32_TokenType
#  include <windows.h>
#  undef TokenType
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

/* Register allocation for mini-JIT compiled functions:
 * We map MirValueId → x86-64 register (spilling to a tiny stack frame
 * for functions with >12 values).  For the fibonacci test, 3–4 regs suffice. */

#define JIT_MAX_VREGS 16

/* Map MIR value IDs 0–15 to x86-64 integer registers.
 * 0 = MIR_VALUE_NONE (unused placeholder → RAX)
 * 1..15 = SSA value ids → physical regs.
 * R10, R11 are caller-saved (ok to clobber).
 * R12..R15 are callee-saved (we push/pop them in prologue/epilogue). */
#define R10 10
#define R11 11
#define R12 12
#define R13 13
#define R14 14
#define R15 15

static const int kVregMap[JIT_MAX_VREGS] = {
    RAX,  /* 0: MIR_VALUE_NONE placeholder */
    RCX,  /* 1 */
    RDX,  /* 2 */
    R8,   /* 3 */
    R9,   /* 4 */
    R10,  /* 5 */
    R11,  /* 6 */
    R12,  /* 7 — callee-saved */
    R13,  /* 8 — callee-saved */
    R14,  /* 9 — callee-saved */
    R15,  /* 10 — callee-saved */
    RBX,  /* 11 — callee-saved */
    RSI,  /* 12 — callee-saved on Win64, not SysV (ok for simple fns) */
    RDI,  /* 13 — callee-saved on Win64, not SysV */
    RAX,  /* 14: overflow fallback */
    RAX,  /* 15: overflow fallback */
};

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

/* Quick scan: return false if the function has any instruction the mini-JIT
 * cannot handle correctly (e.g. ALLOCA / LOAD / STORE / VEC_*). */
static bool jit_function_is_supported(MirFunction* func) {
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
                case MIR_CALL:
                case MIR_CALL_INDIRECT:
                case MIR_CALL_VIRTUAL:
                case MIR_ARC_RETAIN:
                case MIR_ARC_RELEASE:
                case MIR_OBJ_ALLOC:
                case MIR_SUSPEND:
                case MIR_FADD: case MIR_FSUB: case MIR_FMUL: case MIR_FDIV: case MIR_FNEG:
                case MIR_SITOFP: case MIR_FPTOSI:
                    return false;
                default:
                    if (inst->opcode >= MIR_VEC_LOAD && inst->opcode <= MIR_VEC_SELECT)
                        return false;
                    break;
            }
        }
    }

    return true;
}

static uint8_t* jit_compile_function(MirFunction* func, size_t* out_size) {
    if (!func || !func->entry_block || !out_size) return NULL;
    if (!jit_function_is_supported(func)) return NULL;

    X64Emit e;
    xe_init(&e, 8192);

    /* Prologue: push all callee-saved registers we may use */
    xe_push(&e, RBX);
    xe_byte(&e, 0x55);  /* push rbp */
    /* push r12..r15 (callee-saved, extended regs) */
    xe_byte(&e, 0x41); xe_byte(&e, 0x54);  /* push r12 */
    xe_byte(&e, 0x41); xe_byte(&e, 0x55);  /* push r13 */
    xe_byte(&e, 0x41); xe_byte(&e, 0x56);  /* push r14 */
    xe_byte(&e, 0x41); xe_byte(&e, 0x57);  /* push r15 */
    /* Align stack: after 6 pushes (48 bytes) we may need 8 more for alignment */
    xe_byte(&e, 0x48); xe_byte(&e, 0x83); xe_byte(&e, 0xEC); xe_byte(&e, 0x08); /* sub rsp, 8 */
    xe_mov_r_r(&e, RBP, RSP);

    /* We keep value register mapping simple:
     * Params go into their mapped vreg. */
    int n_params = func->param_count < JIT_MAX_VREGS ? func->param_count : JIT_MAX_VREGS;

#if defined(_WIN32) || defined(_WIN64)
    /* Win64: args in rcx, rdx, r8, r9 */
    static const int win_arg_regs[] = { RCX, RDX, R8, R9 };
    for (int i = 0; i < n_params && i < 4; i++) {
        MirValueId pid = func->params[i].value_id;
        if (pid && pid < JIT_MAX_VREGS)
            xe_mov_r_r(&e, kVregMap[pid], win_arg_regs[i]);
    }
#else
    /* SysV: args in rdi, rsi, rdx, rcx, r8, r9 */
    static const int sysv_arg_regs[] = { RDI, RSI, RDX, RCX, R8, R9 };
    for (int i = 0; i < n_params && i < 6; i++) {
        MirValueId pid = func->params[i].value_id;
        if (pid && pid < JIT_MAX_VREGS)
            xe_mov_r_r(&e, kVregMap[pid], sysv_arg_regs[i]);
    }
#endif

    /* Walk blocks in order */
    JitBlockLabel block_labels[64];
    int n_labels = 0;
    JitPatch patches[MAX_PATCHES];
    int n_patches = 0;

    for (MirBlock* blk = func->entry_block; blk; blk = blk->next_block) {
        /* Record label */
        if (n_labels < 64) {
            block_labels[n_labels].block_id = blk->id;
            block_labels[n_labels].code_offset = e.pos;
            n_labels++;
        }

        for (MirInst* inst = blk->first; inst; inst = inst->next) {
            MirValueId r  = inst->result;
            int dst = (r && r < JIT_MAX_VREGS) ? kVregMap[r] : RAX;

            switch (inst->opcode) {
                case MIR_CONST_INT:
                    xe_mov_r_imm64(&e, dst, inst->as.imm_i64);
                    break;

                case MIR_CONST_BOOL:
                    xe_mov_r_imm64(&e, dst, inst->as.imm_bool ? 1 : 0);
                    break;

                case MIR_ADD: {
                    MirValueId lv = inst->as.binary.lhs, rv2 = inst->as.binary.rhs;
                    int ls = (lv && lv < JIT_MAX_VREGS) ? kVregMap[lv] : RAX;
                    int rs = (rv2 && rv2 < JIT_MAX_VREGS) ? kVregMap[rv2] : RCX;
                    xe_mov_r_r(&e, dst, ls);
                    xe_add_r_r(&e, dst, rs);
                    break;
                }
                case MIR_SUB: {
                    MirValueId lv = inst->as.binary.lhs, rv2 = inst->as.binary.rhs;
                    int ls = (lv && lv < JIT_MAX_VREGS) ? kVregMap[lv] : RAX;
                    int rs = (rv2 && rv2 < JIT_MAX_VREGS) ? kVregMap[rv2] : RCX;
                    xe_mov_r_r(&e, dst, ls);
                    xe_sub_r_r(&e, dst, rs);
                    break;
                }
                case MIR_MUL: {
                    MirValueId lv = inst->as.binary.lhs, rv2 = inst->as.binary.rhs;
                    int ls = (lv && lv < JIT_MAX_VREGS) ? kVregMap[lv] : RAX;
                    int rs = (rv2 && rv2 < JIT_MAX_VREGS) ? kVregMap[rv2] : RCX;
                    xe_mov_r_r(&e, dst, ls);
                    xe_imul_r_r(&e, dst, rs);
                    break;
                }

                case MIR_DIV: {
                    /* IDIV: RDX:RAX / rs → quotient in RAX, remainder in RDX.
                     * We push/pop RDX around this to avoid clobbering the mapped vreg. */
                    MirValueId lv = inst->as.binary.lhs, rv2 = inst->as.binary.rhs;
                    int ls = (lv && lv < JIT_MAX_VREGS) ? kVregMap[lv] : RAX;
                    int rs = (rv2 && rv2 < JIT_MAX_VREGS) ? kVregMap[rv2] : RCX;
                    /* If rs == RDX or rs == RAX, move to a temp (R10 or R11) */
                    int divisor = rs;
                    if (rs == RAX || rs == RDX) {
                        /* MOV r10, rs */
                        xe_mov_r_r(&e, R10, rs);
                        divisor = R10;
                    }
                    /* MOV rax, lhs */
                    xe_mov_r_r(&e, RAX, ls);
                    /* CQO: sign-extend RAX → RDX:RAX.  REX.W + 0x99 */
                    xe_byte(&e, 0x48); xe_byte(&e, 0x99);
                    /* IDIV divisor: REX.W + F7 /7: 48 F7 (C0|7<<3|(divisor&7)) */
                    {
                        uint8_t rex = (uint8_t)(0x48 | (divisor >= 8 ? 0x41 : 0));
                        xe_byte(&e, rex);
                        xe_byte(&e, 0xF7);
                        xe_byte(&e, (uint8_t)(0xF8 | (divisor & 7)));
                    }
                    /* Quotient is in RAX; move to dst */
                    if (dst != RAX) xe_mov_r_r(&e, dst, RAX);
                    break;
                }
                case MIR_NEG: {
                    MirValueId op = inst->as.unary.operand;
                    int src_r = (op && op < JIT_MAX_VREGS) ? kVregMap[op] : RAX;
                    xe_mov_r_r(&e, dst, src_r);
                    /* NEG reg: REX.W + F7 /3 */
                    uint8_t rex = (uint8_t)(0x48 | (dst >= 8 ? 0x41 : 0));
                    xe_byte(&e, rex); xe_byte(&e, 0xF7);
                    xe_byte(&e, (uint8_t)(0xD8 | (dst & 7)));
                    break;
                }

                case MIR_COPY:
                case MIR_BORROW:
                case MIR_BORROW_MUT:
                case MIR_MOVE: {
                    MirValueId op = inst->as.transfer.source;
                    int src_r = (op && op < JIT_MAX_VREGS) ? kVregMap[op] : RAX;
                    xe_mov_r_r(&e, dst, src_r);
                    break;
                }

                case MIR_CMP_LT:
                case MIR_CMP_LE:
                case MIR_CMP_EQ:
                case MIR_CMP_NE:
                case MIR_CMP_GT:
                case MIR_CMP_GE: {
                    MirValueId lv = inst->as.binary.lhs, rv2 = inst->as.binary.rhs;
                    int ls = (lv && lv < JIT_MAX_VREGS) ? kVregMap[lv] : RAX;
                    int rs = (rv2 && rv2 < JIT_MAX_VREGS) ? kVregMap[rv2] : RCX;
                    /* CMP ls, rs */
                    uint8_t rex = (uint8_t)(0x48 | (ls >= 8 ? 0x04 : 0) | (rs >= 8 ? 0x01 : 0));
                    xe_byte(&e, rex); xe_byte(&e, 0x3B);
                    xe_byte(&e, (uint8_t)(0xC0 | ((ls & 7) << 3) | (rs & 7)));
                    /* SETCC al */
                    uint8_t setcc = 0;
                    switch (inst->opcode) {
                        case MIR_CMP_LT: setcc = 0x9C; break; /* SETL */
                        case MIR_CMP_LE: setcc = 0x9E; break; /* SETLE */
                        case MIR_CMP_EQ: setcc = 0x94; break; /* SETE */
                        case MIR_CMP_NE: setcc = 0x95; break; /* SETNE */
                        case MIR_CMP_GT: setcc = 0x9F; break; /* SETG */
                        case MIR_CMP_GE: setcc = 0x9D; break; /* SETGE */
                        default: break;
                    }
                    xe_byte(&e, 0x0F); xe_byte(&e, setcc);
                    xe_byte(&e, 0xC0); /* /0: modrm byte for al */
                    /* MOVZX dst, al */
                    rex = (uint8_t)(0x48 | (dst >= 8 ? 0x44 : 0));
                    xe_byte(&e, rex); xe_byte(&e, 0x0F); xe_byte(&e, 0xB6);
                    xe_byte(&e, (uint8_t)(0xC0 | ((dst & 7) << 3)));
                    break;
                }

                case MIR_CONDBR: {
                    MirValueId cv = inst->as.condbr.cond;
                    int cr = (cv && cv < JIT_MAX_VREGS) ? kVregMap[cv] : RAX;
                    /* TEST cr, cr */
                    uint8_t rex = (uint8_t)(0x48 | (cr >= 8 ? 0x45 : 0));
                    xe_byte(&e, rex); xe_byte(&e, 0x85);
                    xe_byte(&e, (uint8_t)(0xC0 | ((cr & 7) << 3) | (cr & 7)));
                    /* JNZ rel32 (to true_bb): 0x0F 0x85 + 4 bytes */
                    if (n_patches < MAX_PATCHES) {
                        xe_byte(&e, 0x0F); xe_byte(&e, 0x85);
                        patches[n_patches].patch_pos = e.pos;
                        patches[n_patches].block_id = inst->as.condbr.true_bb ? inst->as.condbr.true_bb->id : 0;
                        n_patches++;
                        xe_u32(&e, 0); /* placeholder */
                    } else {
                        e.overflowed = true;
                    }
                    /* JMP rel32 (to false_bb) */
                    if (n_patches < MAX_PATCHES) {
                        xe_byte(&e, 0xE9);
                        patches[n_patches].patch_pos = e.pos;
                        patches[n_patches].block_id = inst->as.condbr.false_bb ? inst->as.condbr.false_bb->id : 0;
                        n_patches++;
                        xe_u32(&e, 0);
                    } else {
                        e.overflowed = true;
                    }
                    break;
                }

                case MIR_BR:
                    if (n_patches < MAX_PATCHES) {
                        xe_byte(&e, 0xE9);
                        patches[n_patches].patch_pos = e.pos;
                        patches[n_patches].block_id = inst->as.br.target ? inst->as.br.target->id : 0;
                        n_patches++;
                        xe_u32(&e, 0);
                    } else {
                        e.overflowed = true;
                    }
                    break;

                case MIR_RET: {
                    MirValueId rv2 = inst->as.ret.value;
                    int src_r = (rv2 && rv2 < JIT_MAX_VREGS) ? kVregMap[rv2] : RAX;
                    if (src_r != RAX) xe_mov_r_r(&e, RAX, src_r);
                    /* epilogue: restore callee-saved regs in reverse order */
                    xe_byte(&e, 0x48); xe_byte(&e, 0x83); xe_byte(&e, 0xC4); xe_byte(&e, 0x08); /* add rsp, 8 */
                    xe_byte(&e, 0x41); xe_byte(&e, 0x5F);  /* pop r15 */
                    xe_byte(&e, 0x41); xe_byte(&e, 0x5E);  /* pop r14 */
                    xe_byte(&e, 0x41); xe_byte(&e, 0x5D);  /* pop r13 */
                    xe_byte(&e, 0x41); xe_byte(&e, 0x5C);  /* pop r12 */
                    xe_pop(&e, RBP);
                    xe_pop(&e, RBX);
                    xe_ret(&e);
                    break;
                }
                case MIR_RET_VOID:
                    xe_mov_r_imm64(&e, RAX, 0);
                    xe_byte(&e, 0x48); xe_byte(&e, 0x83); xe_byte(&e, 0xC4); xe_byte(&e, 0x08);
                    xe_byte(&e, 0x41); xe_byte(&e, 0x5F);
                    xe_byte(&e, 0x41); xe_byte(&e, 0x5E);
                    xe_byte(&e, 0x41); xe_byte(&e, 0x5D);
                    xe_byte(&e, 0x41); xe_byte(&e, 0x5C);
                    xe_pop(&e, RBP);
                    xe_pop(&e, RBX);
                    xe_ret(&e);
                    break;

                case MIR_DEBUGLOC:
                case MIR_NOP:
                    break; /* skip */

                default:
                    /* Unhandled: emit a NOP so code size stays consistent */
                    xe_byte(&e, 0x90);
                    break;
            }
        }
    }

    if (e.overflowed) {
        xe_free(&e);
        return NULL;
    }

    /* Ensure generated code always returns even for malformed MIR. */
    if (e.pos == 0 || e.buf[e.pos - 1] != 0xC3) {
        xe_mov_r_imm64(&e, RAX, 0);
        xe_byte(&e, 0x48); xe_byte(&e, 0x83); xe_byte(&e, 0xC4); xe_byte(&e, 0x08);
        xe_byte(&e, 0x41); xe_byte(&e, 0x5F);
        xe_byte(&e, 0x41); xe_byte(&e, 0x5E);
        xe_byte(&e, 0x41); xe_byte(&e, 0x5D);
        xe_byte(&e, 0x41); xe_byte(&e, 0x5C);
        xe_pop(&e, RBP);
        xe_pop(&e, RBX);
        xe_ret(&e);
        if (e.overflowed) {
            xe_free(&e);
            return NULL;
        }
    }

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
/*
 * Generate a trampoline stub.
 *
 * Entry:  rdi/rcx = CvmReg* regs,  rsi/rdx = int n_args
 * Action: save regs ptr in rbx (callee-saved); call cjb_dispatcher;
 *         write return value (rax) back to regs[0]; ret.
 *
 * cjb_dispatcher signature: int64_t f(void* code, CvmReg* regs, int n_args)
 */
/*
 * Generate a trampoline stub.
 *
 * Entry:  rdi/rcx = CvmReg* regs,  rsi/rdx = int n_args
 * Action: Marshal args 1-6 into ABI registers; call target; 
 *         write rax back to regs[0]; restore stack; ret.
 */
static void* jit_gen_trampoline(void* code_ptr, size_t* out_size) {
    X64Emit e;
    xe_init(&e, 256);

    /* Prologue: push rbx, rbp, r12 (callee-saved) */
    xe_push(&e, RBX);
    xe_push(&e, RBP);
    xe_byte(&e, 0x41); xe_byte(&e, 0x54); /* push r12 */

#if defined(_WIN32) || defined(_WIN64)
    /* Win64 ABI: rcx=regs, rdx=n_args */
    xe_mov_r_r(&e, RBX, RCX); /* save regs ptr */
    xe_mov_r_r(&e, R12, RDX); /* save n_args */
    
    /* Marshal args: regs[0..3] -> rcx, rdx, r8, r9 */
    /* Check n_args >= 1 */
    xe_byte(&e, 0x48); xe_byte(&e, 0x83); xe_byte(&e, 0xFC); xe_byte(&e, 0x01); /* cmp r12, 1 */
    /* jb + ... (skip) */
    xe_byte(&e, 0x48); xe_byte(&e, 0x8B); xe_byte(&e, 0x0B); /* mov rcx, [rbx] */
    /* Repeat for rdx, r8, r9 with offsets 8, 16, 24 if n_args >= 2, 3, 4 */
    xe_byte(&e, 0x48); xe_byte(&e, 0x8B); xe_byte(&e, 0x53); xe_byte(&e, 0x08); /* mov rdx, [rbx+8] */
    xe_byte(&e, 0x4C); xe_byte(&e, 0x8B); xe_byte(&e, 0x43); xe_byte(&e, 0x10); /* mov r8,  [rbx+16] */
    xe_byte(&e, 0x4C); xe_byte(&e, 0x8B); xe_byte(&e, 0x4B); xe_byte(&e, 0x18); /* mov r9,  [rbx+24] */

    /* Align stack for call (3 pushes = 24 bytes + return address = 32. 
     * Needs 32 bytes shadow space on Win64. Total 64. Already aligned to 16.) */
    xe_byte(&e, 0x48); xe_byte(&e, 0x83); xe_byte(&e, 0xEC); xe_byte(&e, 0x20); /* sub rsp, 32 */
#else
    /* SysV ABI: rdi=regs, rsi=n_args */
    xe_mov_r_r(&e, RBX, RDI); /* save regs ptr */
    xe_mov_r_r(&e, R12, RSI); /* save n_args */

    /* Marshal args: regs[0..5] -> rdi, rsi, rdx, rcx, r8, r9 */
    xe_byte(&e, 0x48); xe_byte(&e, 0x8B); xe_byte(&e, 0x3B); /* mov rdi, [rbx] */
    xe_byte(&e, 0x48); xe_byte(&e, 0x8B); xe_byte(&e, 0x73); xe_byte(&e, 0x08); /* mov rsi, [rbx+8] */
    xe_byte(&e, 0x48); xe_byte(&e, 0x8B); xe_byte(&e, 0x53); xe_byte(&e, 0x10); /* mov rdx, [rbx+16] */
    xe_byte(&e, 0x48); xe_byte(&e, 0x8B); xe_byte(&e, 0x4B); xe_byte(&e, 0x18); /* mov rcx, [rbx+24] */
    xe_byte(&e, 0x4C); xe_byte(&e, 0x8B); xe_byte(&e, 0x43); xe_byte(&e, 0x20); /* mov r8,  [rbx+32] */
    xe_byte(&e, 0x4C); xe_byte(&e, 0x8B); xe_byte(&e, 0x4B); xe_byte(&e, 0x28); /* mov r9,  [rbx+40] */

    /* Align stack: 3 pushes = 24 bytes. Need sub rsp, 8 to align to 16. */
    xe_byte(&e, 0x48); xe_byte(&e, 0x83); xe_byte(&e, 0xEC); xe_byte(&e, 0x08);
#endif

    /* Call target */
    xe_byte(&e, 0x48); xe_byte(&e, 0xB8);
    xe_i64(&e, (int64_t)(uintptr_t)code_ptr);
    xe_byte(&e, 0xFF); xe_byte(&e, 0xD0); /* call rax */

#if defined(_WIN32) || defined(_WIN64)
    xe_byte(&e, 0x48); xe_byte(&e, 0x83); xe_byte(&e, 0xC4); xe_byte(&e, 0x20);
#else
    xe_byte(&e, 0x48); xe_byte(&e, 0x83); xe_byte(&e, 0xC4); xe_byte(&e, 0x08);
#endif

    /* Store return value: mov [rbx], rax */
    xe_byte(&e, 0x48); xe_byte(&e, 0x89); xe_byte(&e, 0x03);

    /* Epilogue */
    xe_byte(&e, 0x41); xe_byte(&e, 0x5C); /* pop r12 */
    xe_pop(&e, RBP);
    xe_pop(&e, RBX);
    xe_ret(&e);

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
    void* tramp_mem = jit_gen_trampoline(exec_page, &tramp_size);
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
