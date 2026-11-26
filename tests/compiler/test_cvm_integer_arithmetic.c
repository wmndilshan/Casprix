#include "compiler/ir/mir.h"
#include "compiler/ir/mir_opt.h"
#include "compiler/ir/mir_consteval.h"
#include "vm/cvm_engine.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static MirType* int_type(MirModule* m, unsigned width, bool uns) {
    switch (width) {
        case 8: return uns ? mir_type_u8(m) : mir_type_i8(m);
        case 16: return uns ? mir_type_u16(m) : mir_type_i16(m);
        case 32: return uns ? mir_type_u32(m) : mir_type_i32(m);
        default: return uns ? mir_type_u64(m) : mir_type_i64(m);
    }
}

static void run_binary(unsigned width, bool uns, MirOpcode op, int64_t a, int64_t b,
                       int64_t expected, int trap) {
    MirModule* m = mir_module_create("integer_arithmetic");
    MirType* t = int_type(m, width, uns);
    MirFunction* f = mir_module_add_function(m, "main", t, NULL, 0);
    MirBlock* bb = mir_function_add_block(f, "entry");
    MirBuilder builder; mir_builder_init(&builder, m, f); mir_builder_set_block(&builder, bb);
    MirValueId lhs = mir_build_const_int(&builder, a, t);
    MirValueId rhs = mir_build_const_int(&builder, b, t);
    MirValueId result;
    switch (op) {
        case MIR_DIV: result = mir_build_div(&builder, lhs, rhs); break;
        case MIR_MOD: result = mir_build_mod(&builder, lhs, rhs); break;
        case MIR_SHL: result = mir_build_shl(&builder, lhs, rhs); break;
        case MIR_SHR: result = mir_build_shr(&builder, lhs, rhs); break;
        default: result = mir_build_ushr(&builder, lhs, rhs); break;
    }
    mir_build_ret(&builder, result);
    CvmState* vm = cvm_state_create(m, NULL);
    CHECK(vm != NULL);
    if (vm) {
        int64_t got = cvm_run(vm, "main", NULL, 0);
        CHECK(vm->trap_code == trap);
        if (!trap) {
            checks++;
            if (got != expected) {
                failures++;
                fprintf(stderr, "FAIL width=%u unsigned=%d opcode=%d lhs=%lld rhs=%lld expected=%lld got=%lld\n",
                        width, uns, (int)op, (long long)a, (long long)b,
                        (long long)expected, (long long)got);
            }
        }

        /* Run the same MIR again after O2 and compare its observable result. */
        cvm_state_destroy(vm);
        MirOptStats opt_stats = {0};
        mir_optimize_function(f, MIR_OPT_STANDARD, &opt_stats);
        /* The VM caches dispatch labels in vm_data. The MIR was mutated after
         * the O0 run, so invalidate those test-only cached labels. */
        for (MirBlock* bb = f->entry_block; bb; bb = bb->next_block)
            for (MirInst* inst = bb->first; inst; inst = inst->next)
                inst->vm_data = NULL;
        vm = cvm_state_create(m, NULL);
        CHECK(vm != NULL);
        if (!vm) { mir_module_destroy(m); return; }
        int64_t optimized = cvm_run(vm, "main", NULL, 0);
        CHECK(vm->trap_code == trap);
        if (!trap) {
            checks++;
            if (optimized != got || optimized != expected) {
                failures++;
                fprintf(stderr, "O0/O2 mismatch width=%u unsigned=%d opcode=%d O0=%lld O2=%lld expected=%lld\n",
                        width, uns, (int)op, (long long)got,
                        (long long)optimized, (long long)expected);
            }
        }
        cvm_state_destroy(vm);
    }
    mir_module_destroy(m);
}

static void test_optimizer_preserves_traps(void) {
    MirModule* m = mir_module_create("optimizer_trap");
    MirType* t = mir_type_i32(m);
    MirParam param = { .name = "divisor", .type = t };
    MirFunction* f = mir_module_add_function(m, "main", t, &param, 1);
    MirBlock* bb = mir_function_add_block(f, "entry");
    MirBuilder b; mir_builder_init(&b, m, f); mir_builder_set_block(&b, bb);
    MirValueId zero = mir_build_const_int(&b, 0, t);
    MirValueId quotient = mir_build_div(&b, zero, f->params[0].value_id);
    mir_build_ret(&b, quotient);
    MirOptStats stats = {0};
    mir_optimize_function(f, MIR_OPT_STANDARD, &stats);
    CvmState* vm = cvm_state_create(m, NULL);
    CvmReg zero_arg = 0;
    CHECK(vm != NULL);
    if (vm) {
        (void)cvm_call_function(vm, f, &zero_arg, 1);
        CHECK(vm->trap_code == CVM_TRAP_DIVISION_BY_ZERO);
        cvm_state_destroy(vm);
    }
    mir_module_destroy(m);

    /* DCE must retain an invalid operation even when its result is unused. */
    m = mir_module_create("unused_optimizer_trap"); t = mir_type_i32(m);
    f = mir_module_add_function(m, "main", t, NULL, 0);
    bb = mir_function_add_block(f, "entry");
    mir_builder_init(&b, m, f); mir_builder_set_block(&b, bb);
    MirValueId one = mir_build_const_int(&b, 1, t), z = mir_build_const_int(&b, 0, t);
    (void)mir_build_div(&b, one, z);
    mir_build_ret(&b, mir_build_const_int(&b, 42, t));
    memset(&stats, 0, sizeof(stats));
    mir_optimize_function(f, MIR_OPT_STANDARD, &stats);
    vm = cvm_state_create(m, NULL);
    CHECK(vm != NULL);
    if (vm) {
        (void)cvm_run(vm, "main", NULL, 0);
        CHECK(vm->trap_code == CVM_TRAP_DIVISION_BY_ZERO);
        cvm_state_destroy(vm);
    }
    mir_module_destroy(m);
}

static void test_signed_remainder_strength_reduction(void) {
    MirModule* m = mir_module_create("signed_mod_strength");
    MirType* t = mir_type_i32(m);
    MirParam param = { .name = "value", .type = t };
    MirFunction* f = mir_module_add_function(m, "main", t, &param, 1);
    MirBlock* bb = mir_function_add_block(f, "entry");
    MirBuilder b; mir_builder_init(&b, m, f); mir_builder_set_block(&b, bb);
    MirValueId four = mir_build_const_int(&b, 4, t);
    mir_build_ret(&b, mir_build_mod(&b, f->params[0].value_id, four));
    MirOptStats stats = {0};
    mir_optimize_function(f, MIR_OPT_STANDARD, &stats);
    bool has_mod = false;
    for (MirInst* inst = bb->first; inst; inst = inst->next)
        if (inst->opcode == MIR_MOD) has_mod = true;
    CHECK(has_mod);
    CvmState* vm = cvm_state_create(m, NULL);
    CvmReg arg = (CvmReg)-5;
    CHECK(vm != NULL);
    if (vm) {
        int64_t result = cvm_call_function(vm, f, &arg, 1);
        CHECK(vm->trap_code == 0 && result == -1);
        cvm_state_destroy(vm);
    }
    mir_module_destroy(m);
}

static void test_consteval_integer_semantics(void) {
    MirModule* m = mir_module_create("consteval_integer");
    MirType* t = mir_type_i8(m);
    MirFunction* f = mir_module_add_function(m, "wrap", t, NULL, 0);
    MirBlock* bb = mir_function_add_block(f, "entry");
    MirBuilder b; mir_builder_init(&b, m, f); mir_builder_set_block(&b, bb);
    MirValueId max = mir_build_const_int(&b, 127, t);
    MirValueId one = mir_build_const_int(&b, 1, t);
    mir_build_ret(&b, mir_build_add(&b, max, one));
    MirConstEval ce; mir_consteval_init(&ce, m);
    MirConstValue result;
    CHECK(mir_consteval_call(&ce, f, NULL, 0, &result));
    CHECK(result.kind == MIR_CVAL_INT && result.as.i64 == -128);

    MirFunction* not_fn = mir_module_add_function(m, "complement", t, NULL, 0);
    MirBlock* not_bb = mir_function_add_block(not_fn, "entry");
    mir_builder_init(&b, m, not_fn); mir_builder_set_block(&b, not_bb);
    MirValueId zero = mir_build_const_int(&b, 0, t);
    mir_build_ret(&b, mir_build_not(&b, zero));
    CHECK(mir_consteval_call(&ce, not_fn, NULL, 0, &result));
    CHECK(result.kind == MIR_CVAL_INT && result.as.i64 == -1);
    mir_consteval_destroy(&ce);
    mir_module_destroy(m);

    m = mir_module_create("consteval_div_zero"); t = mir_type_i32(m);
    f = mir_module_add_function(m, "bad", t, NULL, 0);
    bb = mir_function_add_block(f, "entry");
    mir_builder_init(&b, m, f); mir_builder_set_block(&b, bb);
    MirValueId n = mir_build_const_int(&b, 7, t), z = mir_build_const_int(&b, 0, t);
    mir_build_ret(&b, mir_build_div(&b, n, z));
    mir_consteval_init(&ce, m);
    CHECK(!mir_consteval_call(&ce, f, NULL, 0, &result));
    CHECK(ce.error_msg && strstr(ce.error_msg, "division by zero") != NULL);
    mir_consteval_destroy(&ce);
    mir_module_destroy(m);

    m = mir_module_create("consteval_invalid_shift"); t = mir_type_i32(m);
    f = mir_module_add_function(m, "bad_shift", t, NULL, 0);
    bb = mir_function_add_block(f, "entry");
    mir_builder_init(&b, m, f); mir_builder_set_block(&b, bb);
    n = mir_build_const_int(&b, 1, t);
    MirValueId count = mir_build_const_int(&b, 32, t);
    mir_build_ret(&b, mir_build_shl(&b, n, count));
    mir_consteval_init(&ce, m);
    CHECK(!mir_consteval_call(&ce, f, NULL, 0, &result));
    CHECK(ce.error_msg && strstr(ce.error_msg, "invalid shift") != NULL);
    mir_consteval_destroy(&ce);
    mir_module_destroy(m);
}

static void test_call_propagation(void) {
    MirModule* m = mir_module_create("trap_call"); MirType* t = mir_type_i64(m);
    MirFunction* callee = mir_module_add_function(m, "callee", t, NULL, 0);
    MirBlock* cb = mir_function_add_block(callee, "entry"); MirBuilder b;
    mir_builder_init(&b, m, callee); mir_builder_set_block(&b, cb);
    MirValueId x = mir_build_const_int(&b, 9, t), z = mir_build_const_int(&b, 0, t);
    mir_build_ret(&b, mir_build_div(&b, x, z));
    MirFunction* caller = mir_module_add_function(m, "main", t, NULL, 0);
    MirBlock* mb = mir_function_add_block(caller, "entry");
    mir_builder_init(&b, m, caller); mir_builder_set_block(&b, mb);
    MirValueId call = mir_build_call(&b, "callee", NULL, 0, t);
    MirValueId after = mir_build_const_int(&b, 123, t);
    mir_build_ret(&b, after);
    CvmState* vm = cvm_state_create(m, NULL); CHECK(vm != NULL);
    if (vm) {
        (void)cvm_call_function(vm, callee, NULL, 0);
        CHECK(vm->trap_code == CVM_TRAP_DIVISION_BY_ZERO);
        vm->trap_code = 0;
        int64_t caller_result = cvm_run(vm, "main", NULL, 0);
        CHECK(vm->trap_code == CVM_TRAP_DIVISION_BY_ZERO);
        CHECK(caller_result != 123);
        CHECK(call != MIR_VALUE_NONE);
        vm->trap_code = 0;
        (void)cvm_call_function(vm, caller, NULL, 0);
        CHECK(vm->trap_code == CVM_TRAP_DIVISION_BY_ZERO);
        cvm_state_destroy(vm);
    }
    mir_module_destroy(m);
}

int main(void) {
    for (unsigned w = 8; w <= 64; w *= 2) {
        int64_t signed_min = w == 64 ? INT64_MIN : -(INT64_C(1) << (w - 1));
        run_binary(w, false, MIR_DIV, -84, 7, -12, 0);
        run_binary(w, false, MIR_MOD, -85, 7, -1, 0);
        run_binary(w, true, MIR_DIV, -1, 2, (int64_t)((UINT64_C(1) << (w - 1)) - 1), 0);
        run_binary(w, true, MIR_MOD, -1, 10, 5, 0);
        run_binary(w, false, MIR_DIV, 1, 0, 0, CVM_TRAP_DIVISION_BY_ZERO);
        run_binary(w, true, MIR_DIV, 1, 0, 0, CVM_TRAP_DIVISION_BY_ZERO);
        run_binary(w, false, MIR_MOD, 1, 0, 0, CVM_TRAP_REMAINDER_BY_ZERO);
        run_binary(w, true, MIR_MOD, 1, 0, 0, CVM_TRAP_REMAINDER_BY_ZERO);
        run_binary(w, false, MIR_SHL, 1, 0, 1, 0);
        run_binary(w, false, MIR_SHL, 1, (int64_t)w - 1, signed_min, 0);
        run_binary(w, false, MIR_SHL, 1, -1, 0, CVM_TRAP_INVALID_SHIFT);
        run_binary(w, false, MIR_SHL, 1, w, 0, CVM_TRAP_INVALID_SHIFT);
        run_binary(w, false, MIR_SHL, 1, w + 1, 0, CVM_TRAP_INVALID_SHIFT);
        run_binary(w, false, MIR_SHL, 1, 1000, 0, CVM_TRAP_INVALID_SHIFT);
        run_binary(w, false, MIR_DIV, signed_min, -1, 0, CVM_TRAP_INTEGER_OVERFLOW);
        run_binary(w, false, MIR_MOD, signed_min, -1, 0, CVM_TRAP_INTEGER_OVERFLOW);
    }
    test_call_propagation();
    test_optimizer_preserves_traps();
    test_signed_remainder_strength_reduction();
    test_consteval_integer_semantics();
    printf("CVM integer arithmetic: %d assertions, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
