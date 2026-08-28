/*
 * test_jit_regalloc — the mini-JIT's linear-scan register allocator (task B).
 *
 * Verifies that jit_compile_function() + its trampoline now:
 *   (a) compile a 15-instruction arithmetic chain with ~6 live values that the
 *       OLD next_value_id<=12 cap rejected,
 *   (b) compile a function forcing >13 simultaneously-live values, using
 *       spill slots — checked by inspecting the generated prologue for a
 *       `sub rsp, imm32` frame larger than the no-spill baseline (8),
 *   (c) compile a branch-containing function correctly,
 *   (d) still bail on a truly enormous function (>JIT_MAX_SPILL_SLOTS spills).
 *
 * CRITICAL: every fixture runs a DIFFERENTIAL check — the interpreter result
 * vs the JIT (native trampoline) result — and asserts they are equal. A
 * register-clobber bug in the allocator shows up as a wrong number, not a
 * crash, so this is the real correctness gate.
 *
 * Built MIR-subsystem-only (no lexer/parser/sema), same pattern as
 * test_vm_jit.c / test_jit_caps.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "compiler/ir/mir.h"
#include "../../runtime/vm/cvm_engine.h"
#include "../../runtime/vm/jit_bridge.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", (msg)); } \
    else      { printf("  [FAIL] %s\n", (msg)); failures++; } \
} while (0)

/* ── Fixtures ────────────────────────────────────────────────────────────── */

/* (a) 15-instruction arithmetic chain over 4 params. Interleaves several
 * temporaries so ~6 are live at once; next_value_id ends well past 12. */
static MirFunction* build_chain15(MirModule* m) {
    MirType* i64t = mir_type_i64(m);
    MirParam p[4];
    for (int i = 0; i < 4; i++) { p[i].name = "p"; p[i].type = i64t; p[i].value_id = 0; }
    MirFunction* f = mir_module_add_function(m, "chain15", i64t, p, 4);
    MirBlock* entry = mir_function_add_block(f, "entry");
    MirBuilder b; mir_builder_init(&b, m, f); mir_builder_set_block(&b, entry);

    MirValueId a = f->params[0].value_id;
    MirValueId c = f->params[1].value_id;
    MirValueId d = f->params[2].value_id;
    MirValueId g = f->params[3].value_id;

    MirValueId t1 = mir_build_add(&b, a, c);      /* p0+p1                 */
    MirValueId t2 = mir_build_mul(&b, d, g);      /* p2*p3                 */
    MirValueId t3 = mir_build_sub(&b, t1, d);     /* (p0+p1)-p2           */
    MirValueId t4 = mir_build_add(&b, t2, a);     /* p2*p3+p0            */
    MirValueId t5 = mir_build_mul(&b, t3, c);     /* ((p0+p1)-p2)*p1     */
    MirValueId t6 = mir_build_sub(&b, t4, g);     /* p2*p3+p0-p3         */
    MirValueId t7 = mir_build_add(&b, t5, t6);
    MirValueId t8 = mir_build_mul(&b, t7, a);
    MirValueId t9 = mir_build_sub(&b, t8, t1);
    MirValueId t10 = mir_build_add(&b, t9, t2);
    MirValueId t11 = mir_build_neg(&b, t10);
    MirValueId t12 = mir_build_add(&b, t11, t3);
    MirValueId t13 = mir_build_mul(&b, t12, c);
    MirValueId t14 = mir_build_sub(&b, t13, t4);
    mir_build_ret(&b, t14);
    return f;
}
static int64_t ref_chain15(int64_t p0, int64_t p1, int64_t p2, int64_t p3) {
    int64_t t1 = p0 + p1;
    int64_t t2 = p2 * p3;
    int64_t t3 = t1 - p2;
    int64_t t4 = t2 + p0;
    int64_t t5 = t3 * p1;
    int64_t t6 = t4 - p3;
    int64_t t7 = t5 + t6;
    int64_t t8 = t7 * p0;
    int64_t t9 = t8 - t1;
    int64_t t10 = t9 + t2;
    int64_t t11 = -t10;
    int64_t t12 = t11 + t3;
    int64_t t13 = t12 * p1;
    int64_t t14 = t13 - t4;
    return t14;
}

/* (b) One param; build N temporaries that are ALL live simultaneously (each
 * used only in the final fold), forcing the allocator past its 12-register
 * pool into spill slots. N=22 → ~22 live at the fold. */
#define WIDE_N 22
static MirFunction* build_wide(MirModule* m) {
    MirType* i64t = mir_type_i64(m);
    MirParam p[1]; p[0].name = "n"; p[0].type = i64t; p[0].value_id = 0;
    MirFunction* f = mir_module_add_function(m, "wide", i64t, p, 1);
    MirBlock* entry = mir_function_add_block(f, "entry");
    MirBuilder b; mir_builder_init(&b, m, f); mir_builder_set_block(&b, entry);

    MirValueId n = f->params[0].value_id;
    MirValueId one = mir_build_const_int(&b, 1, i64t);
    MirValueId t[WIDE_N];
    /* t[i] = n + (i+1)  — each independent, all live until the fold */
    MirValueId k = one;
    for (int i = 0; i < WIDE_N; i++) {
        MirValueId ki = (i == 0) ? one : mir_build_add(&b, k, one);
        k = ki;
        t[i] = mir_build_add(&b, n, ki);
    }
    /* fold: acc = t0 + t1 + ... + t[N-1] */
    MirValueId acc = t[0];
    for (int i = 1; i < WIDE_N; i++) acc = mir_build_add(&b, acc, t[i]);
    mir_build_ret(&b, acc);
    return f;
}
static int64_t ref_wide(int64_t n) {
    int64_t acc = 0;
    for (int i = 0; i < WIDE_N; i++) acc += n + (int64_t)(i + 1);
    return acc;
}

/* (c) Branch: max(a,b) via CMP + CONDBR + two return blocks. */
static MirFunction* build_max2(MirModule* m) {
    MirType* i64t = mir_type_i64(m);
    MirParam p[2];
    p[0].name = "a"; p[0].type = i64t; p[0].value_id = 0;
    p[1].name = "b"; p[1].type = i64t; p[1].value_id = 0;
    MirFunction* f = mir_module_add_function(m, "max2", i64t, p, 2);
    MirBlock* entry = mir_function_add_block(f, "entry");
    MirBlock* rb    = mir_function_add_block(f, "ret_b");
    MirBlock* ra    = mir_function_add_block(f, "ret_a");
    MirBuilder b; mir_builder_init(&b, m, f);

    mir_builder_set_block(&b, entry);
    MirValueId a = f->params[0].value_id;
    MirValueId bb = f->params[1].value_id;
    MirValueId lt = mir_build_cmp_lt(&b, a, bb);   /* a < b ? */
    mir_build_condbr(&b, lt, rb, ra);

    mir_builder_set_block(&b, rb);
    mir_build_ret(&b, bb);

    mir_builder_set_block(&b, ra);
    mir_build_ret(&b, a);
    return f;
}
static int64_t ref_max2(int64_t a, int64_t b) { return a < b ? b : a; }

/* (d) Absurdly wide function — should exceed JIT_MAX_SPILL_SLOTS and bail. */
#define HUGE_N 200
static MirFunction* build_huge(MirModule* m) {
    MirType* i64t = mir_type_i64(m);
    MirParam p[1]; p[0].name = "n"; p[0].type = i64t; p[0].value_id = 0;
    MirFunction* f = mir_module_add_function(m, "huge", i64t, p, 1);
    MirBlock* entry = mir_function_add_block(f, "entry");
    MirBuilder b; mir_builder_init(&b, m, f); mir_builder_set_block(&b, entry);
    MirValueId n = f->params[0].value_id;
    MirValueId one = mir_build_const_int(&b, 1, i64t);
    MirValueId t[HUGE_N];
    MirValueId k = one;
    for (int i = 0; i < HUGE_N; i++) {
        MirValueId ki = (i == 0) ? one : mir_build_add(&b, k, one);
        k = ki;
        t[i] = mir_build_add(&b, n, ki);
    }
    MirValueId acc = t[0];
    for (int i = 1; i < HUGE_N; i++) acc = mir_build_add(&b, acc, t[i]);
    mir_build_ret(&b, acc);
    return f;
}

/* ── Differential runner ─────────────────────────────────────────────────── */

/* Run `fname` through the pure interpreter (jit=NULL). */
static int64_t run_interp(MirFunction* (*build)(MirModule*), const char* fname,
                          int64_t* args, int n_args) {
    MirModule* m = mir_module_create("interp");
    build(m);
    CvmState* vm = cvm_state_create(m, NULL);
    int64_t r = cvm_run(vm, fname, args, n_args);
    cvm_state_destroy(vm);
    mir_module_destroy(m);
    return r;
}

/* Compile `fname` with the JIT directly and invoke the native trampoline. */
static bool run_jit(MirFunction* (*build)(MirModule*), const char* fname,
                    int64_t* args, int n_args, int64_t* out, size_t* code_size) {
    MirModule* m = mir_module_create("jit");
    MirFunction* f = build(m);
    (void)f;
    CvmJitBridge* jit = cjb_create();
    MirFunction* target = mir_module_find_function(m, fname);
    CvmProfile prof; memset(&prof, 0, sizeof(prof));
    JitResult jr = cjb_compile_function(jit, m, target, &prof);
    bool ok = false;
    if (jr == JIT_OK && prof.tier == CVM_TIER_NATIVE && prof.native_fn) {
        CvmReg regs[16]; memset(regs, 0, sizeof(regs));
        for (int i = 0; i < n_args && i < 16; i++) regs[i] = (CvmReg)args[i];
        prof.native_fn(regs, n_args);
        *out = (int64_t)regs[0];
        if (code_size) *code_size = prof.native_size;
        ok = true;
    }
    cjb_destroy(jit);
    mir_module_destroy(m);
    return ok;
}

static void differential(MirFunction* (*build)(MirModule*), const char* fname,
                         int64_t (*ref)(int64_t,int64_t,int64_t,int64_t),
                         int64_t* args, int n_args, const char* label) {
    int64_t a4[4] = {0,0,0,0};
    for (int i = 0; i < n_args && i < 4; i++) a4[i] = args[i];
    int64_t want = ref(a4[0], a4[1], a4[2], a4[3]);

    int64_t interp = run_interp(build, fname, args, n_args);
    int64_t jit = 0; size_t csz = 0;
    bool jok = run_jit(build, fname, args, n_args, &jit, &csz);

    char buf[256];
    snprintf(buf, sizeof(buf),
             "%s: JIT compiled", label);
    CHECK(jok, buf);

    snprintf(buf, sizeof(buf),
             "%s: interpreter == reference (%lld == %lld)",
             label, (long long)interp, (long long)want);
    CHECK(interp == want, buf);

    snprintf(buf, sizeof(buf),
             "%s: DIFFERENTIAL interpreter == JIT (%lld == %lld)",
             label, (long long)interp, (long long)jit);
    CHECK(jok && interp == jit, buf);
}

/* ref adapters (fixed 4-arg signature for `differential`) */
static int64_t r_chain15(int64_t a,int64_t b,int64_t c,int64_t d){ return ref_chain15(a,b,c,d); }
static int64_t r_wide  (int64_t a,int64_t b,int64_t c,int64_t d){ (void)b;(void)c;(void)d; return ref_wide(a); }
static int64_t r_max2  (int64_t a,int64_t b,int64_t c,int64_t d){ (void)c;(void)d; return ref_max2(a,b); }

int main(void) {
    printf("=== JIT linear-scan register allocator tests ===\n");

    /* (a) 15-instruction chain — previously rejected by next_value_id<=12. */
    {
        int64_t args[4] = { 7, 3, 5, 2 };
        differential(build_chain15, "chain15", r_chain15, args, 4,
                     "(a) chain15 [15 insts, ~6 live]");
    }

    /* (b) Wide function — forces spill slots. Verify a real stack frame in the
     * generated prologue (sub rsp, imm32 with imm32 > 8). */
    {
        int64_t args[4] = { 100, 0, 0, 0 };
        differential(build_wide, "wide", r_wide, args, 1,
                     "(b) wide [22 simultaneously-live → spills]");

        /* Inspect generated code for the spill frame. */
        MirModule* m = mir_module_create("wide_inspect");
        build_wide(m);
        CvmJitBridge* jit = cjb_create();
        MirFunction* t = mir_module_find_function(m, "wide");
        CvmProfile prof; memset(&prof, 0, sizeof(prof));
        cjb_compile_function(jit, m, t, &prof);
        /* native_mem holds the compiled body; scan the first ~40 bytes for the
         * `48 81 EC imm32` (sub rsp, imm32) sequence and check imm32 > 8. */
        bool frame_ok = false;
        if (prof.native_mem) {
            const uint8_t* c = (const uint8_t*)prof.native_mem;
            size_t lim = prof.native_size < 48 ? prof.native_size : 48;
            for (size_t i = 0; i + 7 <= lim; i++) {
                if (c[i] == 0x48 && c[i+1] == 0x81 && c[i+2] == 0xEC) {
                    uint32_t imm; memcpy(&imm, c + i + 3, 4);
                    if (imm > 8) frame_ok = true;
                    break;
                }
            }
        }
        CHECK(frame_ok, "(b) wide: generated prologue reserves a spill frame (sub rsp, >8)");
        cjb_destroy(jit);
        mir_module_destroy(m);
    }

    /* (c) Branch-containing function. */
    {
        int64_t args[4] = { 41, 99, 0, 0 };
        differential(build_max2, "max2", r_max2, args, 2,
                     "(c) max2 [CMP + CONDBR]");
        int64_t args2[4] = { 99, 41, 0, 0 };
        differential(build_max2, "max2", r_max2, args2, 2,
                     "(c) max2 [other branch]");
    }

    /* (d) Absurd width — must exceed JIT_MAX_SPILL_SLOTS and bail (stay
     * interpreted). Replaces test_jit_caps' old "17 value ids → bail" case,
     * whose premise (a fixed 12-slot register map) no longer exists.  See the
     * report note about this test-semantics change. */
    {
        MirModule* m = mir_module_create("huge");
        build_huge(m);
        CvmJitBridge* jit = cjb_create();
        MirFunction* t = mir_module_find_function(m, "huge");
        CvmProfile prof; memset(&prof, 0, sizeof(prof));
        JitResult jr = cjb_compile_function(jit, m, t, &prof);
        CHECK(jr != JIT_OK && prof.tier != CVM_TIER_NATIVE,
              "(d) huge [200 live → >64 spill slots] refused, stays interpreted");
        /* And it still runs correctly under the interpreter. */
        int64_t args[1] = { 10 };
        int64_t interp = run_interp(build_huge, "huge", args, 1);
        int64_t want = 0;
        for (int i = 0; i < HUGE_N; i++) want += 10 + (int64_t)(i + 1);
        CHECK(interp == want, "(d) huge: interpreter result still correct");
        cjb_destroy(jit);
        mir_module_destroy(m);
    }

    if (failures == 0) { printf("=== ALL TESTS PASSED ===\n"); return 0; }
    printf("=== %d FAILURE(S) ===\n", failures);
    return 1;
}
