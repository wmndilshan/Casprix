/*
 * test_jit_selfcall — task C: self-recursive functions tier up, and a
 * permanently-ineligible hot function is never re-attempted (JIT_DENIED).
 *
 * (1) A hand-built recursive fib is run > CVM_JIT_THRESHOLD times through the
 *     CVM with a JIT bridge attached; assert its profile reaches
 *     CVM_TIER_NATIVE and every result matches the interpreter.
 * (2) A hot function containing a LOAD (never JIT-eligible) is run well past
 *     several 64-call resample intervals; assert the JIT bridge's
 *     functions_compiled counter never moves after the first denied attempt
 *     — i.e. CVM_TIER_JIT_DENIED stops the retry loop.
 *
 * Built MIR-subsystem-only, same pattern as test_vm_jit.c.
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

/* fib(n) = n < 2 ? n : fib(n-1) + fib(n-2)   — self-recursive, integer-only. */
static MirFunction* build_fib(MirModule* m) {
    MirType* i64t = mir_type_i64(m);
    MirParam p[1]; p[0].name = "n"; p[0].type = i64t; p[0].value_id = 0;
    MirFunction* f = mir_module_add_function(m, "fib", i64t, p, 1);
    MirBlock* entry = mir_function_add_block(f, "entry");
    MirBlock* base  = mir_function_add_block(f, "base");
    MirBlock* rec   = mir_function_add_block(f, "rec");
    MirBuilder b; mir_builder_init(&b, m, f);

    mir_builder_set_block(&b, entry);
    MirValueId n   = f->params[0].value_id;
    MirValueId two = mir_build_const_int(&b, 2, i64t);
    MirValueId lt  = mir_build_cmp_lt(&b, n, two);
    mir_build_condbr(&b, lt, base, rec);

    mir_builder_set_block(&b, base);
    mir_build_ret(&b, n);

    mir_builder_set_block(&b, rec);
    MirValueId one = mir_build_const_int(&b, 1, i64t);
    MirValueId nm1 = mir_build_sub(&b, n, one);
    MirValueId a1[1] = { nm1 };
    MirValueId r1 = mir_build_call(&b, "fib", a1, 1, i64t);
    MirValueId nm2 = mir_build_sub(&b, n, two);
    MirValueId a2[1] = { nm2 };
    MirValueId r2 = mir_build_call(&b, "fib", a2, 1, i64t);
    MirValueId sum = mir_build_add(&b, r1, r2);
    mir_build_ret(&b, sum);
    return f;
}
static int64_t ref_fib(int64_t n) { return n < 2 ? n : ref_fib(n-1) + ref_fib(n-2); }

/* A function with a LOAD → never JIT-eligible. loadconst(p) = *(&stackslot) ,
 * i.e. alloca+store+load returning a constant; keeps call_count climbing. */
static MirFunction* build_loady(MirModule* m) {
    MirType* i64t = mir_type_i64(m);
    MirParam p[1]; p[0].name = "p"; p[0].type = i64t; p[0].value_id = 0;
    MirFunction* f = mir_module_add_function(m, "loady", i64t, p, 1);
    MirBlock* entry = mir_function_add_block(f, "entry");
    MirBuilder b; mir_builder_init(&b, m, f); mir_builder_set_block(&b, entry);
    MirValueId slot = mir_build_alloca(&b, i64t);
    mir_build_store(&b, slot, f->params[0].value_id);
    MirValueId v = mir_build_load(&b, slot, i64t);
    mir_build_ret(&b, v);
    return f;
}

int main(void) {
    printf("=== JIT self-recursive-call + JIT_DENIED tests ===\n");

    /* (1) self-recursive fib tiers to native, results match interpreter. */
    {
        MirModule* m = mir_module_create("selfcall_fib");
        build_fib(m);
        CvmJitBridge* jit = cjb_create();
        CvmState* vm = cvm_state_create(m, jit);
        MirFunction* fn = mir_module_find_function(m, "fib");

        int64_t arg = 5;
        int64_t want = ref_fib(arg);
        int64_t last = 0;
        bool all_ok = true;
        /* > CVM_JIT_THRESHOLD calls so the outer entry tiers up */
        for (int i = 0; i < CVM_JIT_THRESHOLD + 200; i++) {
            last = cvm_run(vm, "fib", &arg, 1);
            if (last != want) { all_ok = false; break; }
        }
        CvmProfile* pr = cvm_get_profile(vm, fn);
        printf("       fib profile tier = %d (expected %d = NATIVE), calls = %d\n",
               (int)pr->tier, (int)CVM_TIER_NATIVE, pr->call_count);
        printf("       fib(5) last result = %lld (expected %lld)\n",
               (long long)last, (long long)want);

        CHECK(all_ok, "self-recursive fib: every result matches interpreter");
        CHECK(pr->tier == CVM_TIER_NATIVE, "self-recursive fib: profile reached CVM_TIER_NATIVE");
        CHECK(last == want, "self-recursive fib: final native result correct");

        /* a few larger inputs through the now-native code */
        bool big_ok = true;
        for (int64_t v = 6; v <= 16; v++) {
            int64_t r = cvm_run(vm, "fib", &v, 1);
            if (r != ref_fib(v)) { big_ok = false; break; }
        }
        CHECK(big_ok, "self-recursive fib: native results correct for n=6..16");

        cvm_state_destroy(vm);
        cjb_destroy(jit);
        mir_module_destroy(m);
    }

    /* (2) JIT_DENIED — a hot LOAD-containing function is attempted at most once. */
    {
        MirModule* m = mir_module_create("denied");
        build_loady(m);
        CvmJitBridge* jit = cjb_create();
        CvmState* vm = cvm_state_create(m, jit);

        int64_t arg = 7;
        /* run far past several 64-call resample intervals */
        for (int i = 0; i < CVM_JIT_THRESHOLD + 64 * 20; i++) {
            (void)cvm_run(vm, "loady", &arg, 1);
        }
        MirFunction* fn = mir_module_find_function(m, "loady");
        CvmProfile* pr = cvm_get_profile(vm, fn);
        printf("       loady tier = %d (DENIED=%d), calls = %d, jit.functions_compiled = %d\n",
               (int)pr->tier, (int)CVM_TIER_JIT_DENIED, pr->call_count, jit->functions_compiled);

        CHECK(pr->tier == CVM_TIER_JIT_DENIED,
              "hot LOAD fn: tier is CVM_TIER_JIT_DENIED after first failed attempt");
        CHECK(jit->functions_compiled == 0,
              "hot LOAD fn: functions_compiled stays 0 despite ~20 resample intervals "
              "(no repeated compile attempts)");
        CHECK(pr->call_count > CVM_JIT_THRESHOLD + 64 * 10,
              "hot LOAD fn: call_count kept climbing (function still runs, just not JITd)");

        cvm_state_destroy(vm);
        cjb_destroy(jit);
        mir_module_destroy(m);
    }

    if (failures == 0) { printf("=== ALL TESTS PASSED ===\n"); return 0; }
    printf("=== %d FAILURE(S) ===\n", failures);
    return 1;
}
