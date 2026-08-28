/*
 * test_jit_caps — the mini-JIT's capacity limits.
 *
 * HISTORY of removed caps:
 *   - task B (linear-scan allocator): the "next_value_id > 12 → bail" cap was
 *     removed. A long arithmetic chain now COMPILES (with spill slots) —
 *     see test_jit_regalloc.c for the spill path + differential coverage.
 *   - task A (trampoline stack-arg spilling): the "> 4 params → bail" cap was
 *     removed. Functions with 5..16 params now COMPILE and their args are
 *     marshalled ABI-correctly (register + stack) — see test_jit_trampoline.c.
 *
 * What still bails: functions exceeding the JIT_MAX_SPILL_SLOTS spill cap
 * (test_jit_regalloc.c fixture (d)), functions past JIT_MAX_PARAMS (16), and
 * functions containing non-eligible opcodes (LOAD/STORE/PHI/float/…) or a call
 * to another function (self-recursion is allowed — test_jit_selfcall.c).
 *
 * This file now only asserts the still-true positives.
 *
 * Built MIR-subsystem-only (no lexer/parser/sema), same pattern as
 * test_vm_jit.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/ir/mir.h"
#include "../../runtime/vm/jit_bridge.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else      { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

/* A function with `n` int params that returns their sum — exercises the
 * trampoline arg-count cap when n > 4. Kept to few value ids so ONLY the
 * param cap is what rejects it. */
static MirFunction* build_nparam_sum(MirModule* m, const char* name, int n) {
    MirType* i64t = mir_type_i64(m);
    MirParam* params = calloc(n, sizeof(MirParam));
    for (int i = 0; i < n; i++) {
        params[i].name = "p"; params[i].type = i64t; params[i].value_id = 0;
    }
    MirFunction* f = mir_module_add_function(m, name, i64t, params, n);
    MirBlock* entry = mir_function_add_block(f, "entry");
    MirBuilder b;
    mir_builder_init(&b, m, f);
    mir_builder_set_block(&b, entry);

    MirValueId acc = f->params[0].value_id;
    for (int i = 1; i < n; i++) {
        acc = mir_build_add(&b, acc, f->params[i].value_id);
    }
    mir_build_ret(&b, acc);
    free(params);
    return f;
}

/* A 1-param function that chains `depth` adds, forcing next_value_id past the
 * register-map cap when depth is large — exercises the value-id cap. */
static MirFunction* build_long_chain(MirModule* m, const char* name, int depth) {
    MirType* i64t = mir_type_i64(m);
    MirParam params[1];
    params[0].name = "n"; params[0].type = i64t; params[0].value_id = 0;
    MirFunction* f = mir_module_add_function(m, name, i64t, params, 1);
    MirBlock* entry = mir_function_add_block(f, "entry");
    MirBuilder b;
    mir_builder_init(&b, m, f);
    mir_builder_set_block(&b, entry);

    MirValueId acc = f->params[0].value_id;
    MirValueId one = mir_build_const_int(&b, 1, i64t);
    for (int i = 0; i < depth; i++) {
        acc = mir_build_add(&b, acc, one);
    }
    mir_build_ret(&b, acc);
    return f;
}

int main(void) {
    printf("=== JIT capacity bail-out tests ===\n");

    MirModule* m = mir_module_create("jit_caps");
    CvmJitBridge* jit = cjb_create();

    /* (1a) 4-param function compiles. */
    MirFunction* p4 = build_nparam_sum(m, "sum4", 4);
    CvmProfile pr_p4 = {0};
    JitResult r_p4 = cjb_compile_function(jit, m, p4, &pr_p4);
    CHECK(r_p4 == JIT_OK, "4-param arithmetic fn compiles");

    /* (1b) 7-param function — NOW COMPILES (task A: trampoline stack-arg
     * spilling; old ">4 params → bail" cap removed). Correctness of the
     * marshalling is covered by test_jit_trampoline.c. */
    MirFunction* p7 = build_nparam_sum(m, "sum7", 7);
    CvmProfile pr_p7 = {0};
    JitResult r_p7 = cjb_compile_function(jit, m, p7, &pr_p7);
    CHECK(r_p7 == JIT_OK,
          "7-param fn now COMPILES (trampoline stack-arg spilling; old param cap removed)");

    /* (2a) short chain — compiles (as it always did). */
    MirFunction* c_ok = build_long_chain(m, "chain_ok", 6);   /* ~8 value ids */
    CvmProfile pr_cok = {0};
    JitResult r_cok = cjb_compile_function(jit, m, c_ok, &pr_cok);
    CHECK(r_cok == JIT_OK, "short arithmetic chain compiles");

    /* (2b) longer chain — many value ids, but each temp dies immediately so
     * register pressure stays low. Under the OLD next_value_id<=12 cap this
     * bailed; with the linear-scan allocator it now COMPILES. (Full
     * spill-path + differential coverage is in test_jit_regalloc.c.) */
    MirFunction* c_big = build_long_chain(m, "chain_big", 40); /* ~42 value ids */
    CvmProfile pr_cbig = {0};
    JitResult r_cbig = cjb_compile_function(jit, m, c_big, &pr_cbig);
    CHECK(r_cbig == JIT_OK,
          "40-add chain now COMPILES via linear-scan allocator "
          "(old next_value_id<=12 cap removed)");

    cjb_destroy(jit);
    mir_module_destroy(m);

    if (failures == 0) { printf("=== ALL TESTS PASSED ===\n"); return 0; }
    printf("=== %d FAILURE(S) ===\n", failures);
    return 1;
}
