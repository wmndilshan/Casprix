/*
 * test_jit_trampoline — task A: ABI-correct stack-argument spilling in
 * jit_gen_trampoline.
 *
 * Fixtures: functions with 5, 6, 7, 8, 12 int params, each returning an
 * alternating-sign fold of ALL its arguments
 *     p0 - p1 + p2 - p3 + p4 - ...
 * so an argument that lands in the wrong slot (or is dropped) produces a
 * wrong result. Each is run through the JIT trampoline and DIFFERENTIALLY
 * compared to the CVM interpreter (assert equal).
 *
 * Built MIR-subsystem-only, same pattern as test_vm_jit.c / test_jit_regalloc.c.
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

/* f(p0..p{n-1}) = p0 - p1 + p2 - p3 + ... */
static MirFunction* build_altsum(MirModule* m, const char* name, int n) {
    MirType* i64t = mir_type_i64(m);
    MirParam* p = calloc((size_t)n, sizeof(MirParam));
    for (int i = 0; i < n; i++) { p[i].name = "p"; p[i].type = i64t; p[i].value_id = 0; }
    MirFunction* f = mir_module_add_function(m, name, i64t, p, n);
    MirBlock* entry = mir_function_add_block(f, "entry");
    MirBuilder b; mir_builder_init(&b, m, f); mir_builder_set_block(&b, entry);

    MirValueId acc = f->params[0].value_id;
    for (int i = 1; i < n; i++) {
        if (i & 1) acc = mir_build_sub(&b, acc, f->params[i].value_id);
        else       acc = mir_build_add(&b, acc, f->params[i].value_id);
    }
    mir_build_ret(&b, acc);
    free(p);
    return f;
}
static int64_t ref_altsum(const int64_t* a, int n) {
    int64_t acc = a[0];
    for (int i = 1; i < n; i++) acc = (i & 1) ? acc - a[i] : acc + a[i];
    return acc;
}

static int64_t run_interp(int n, const int64_t* args) {
    MirModule* m = mir_module_create("i");
    build_altsum(m, "altsum", n);
    CvmState* vm = cvm_state_create(m, NULL);
    int64_t r = cvm_run(vm, "altsum", (int64_t*)args, n);
    cvm_state_destroy(vm);
    mir_module_destroy(m);
    return r;
}

static bool run_jit(int n, const int64_t* args, int64_t* out) {
    MirModule* m = mir_module_create("j");
    build_altsum(m, "altsum", n);
    CvmJitBridge* jit = cjb_create();
    MirFunction* t = mir_module_find_function(m, "altsum");
    CvmProfile pr; memset(&pr, 0, sizeof pr);
    JitResult jr = cjb_compile_function(jit, m, t, &pr);
    bool ok = false;
    if (jr == JIT_OK && pr.tier == CVM_TIER_NATIVE && pr.native_fn) {
        CvmReg regs[32]; memset(regs, 0, sizeof regs);
        for (int i = 0; i < n && i < 32; i++) regs[i] = (CvmReg)args[i];
        pr.native_fn(regs, n);
        *out = (int64_t)regs[0];
        ok = true;
    }
    cjb_destroy(jit);
    mir_module_destroy(m);
    return ok;
}

static void do_n(int n) {
    /* distinct, sign-varied args so a swapped/dropped slot is detectable */
    int64_t args[12];
    for (int i = 0; i < n; i++) args[i] = (int64_t)((i + 1) * 1000 + i) * ((i % 3 == 0) ? -1 : 1);
    int64_t want = ref_altsum(args, n);
    int64_t iv = run_interp(n, args);
    int64_t jv = 0;
    bool jok = run_jit(n, args, &jv);

    char buf[160];
    snprintf(buf, sizeof buf, "%d-param altsum: JIT compiled", n);
    CHECK(jok, buf);
    snprintf(buf, sizeof buf, "%d-param altsum: interpreter == reference (%lld)", n, (long long)want);
    CHECK(iv == want, buf);
    snprintf(buf, sizeof buf, "%d-param altsum: DIFFERENTIAL interpreter == JIT (%lld == %lld)",
             n, (long long)iv, (long long)jv);
    CHECK(jok && iv == jv, buf);
}

int main(void) {
    printf("=== JIT trampoline stack-argument spilling tests ===\n");
    do_n(5);
    do_n(6);
    do_n(7);
    do_n(8);
    do_n(12);
    if (failures == 0) { printf("=== ALL TESTS PASSED ===\n"); return 0; }
    printf("=== %d FAILURE(S) ===\n", failures);
    return 1;
}
