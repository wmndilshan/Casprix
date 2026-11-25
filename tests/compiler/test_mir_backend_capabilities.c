#include "compiler/ir/mir.h"
#include "compiler/ir/mir_backend_caps.h"
#include "vm/cvm_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } else { \
        passes++; \
    } \
} while (0)

static int passes;
static int failures;

static bool capability_rejects(MirModule* module, MirCapabilityProfile profile) {
    FILE* sink = tmpfile();
    bool accepted = mir_backend_caps_validate(module, profile, sink);
    if (sink) fclose(sink);
    return !accepted;
}

static MirModule* make_vector_module(MirFunction** main_out) {
    MirModule* module = mir_module_create("vector_capability_probe");
    MirType* i64 = mir_type_i64(module);
    MirFunction* main_func = mir_module_add_function(module, "main", i64, NULL, 0);
    MirBlock* entry = mir_function_add_block(main_func, "entry");
    MirBuilder builder;
    mir_builder_init(&builder, module, main_func);
    mir_builder_set_block(&builder, entry);
    MirValueId scalar = mir_build_const_int(&builder, 7, i64);
    MirValueId vector = mir_build_vec_broadcast(&builder, scalar, i64, 4);
    MirValueId sum = mir_build_vec_reduce_sum(&builder, vector, i64, 4);
    mir_build_ret(&builder, sum);
    *main_out = main_func;
    return module;
}

static void test_vector_rejection(void) {
    MirFunction* main_func = NULL;
    MirModule* module = make_vector_module(&main_func);
    CHECK(mir_verify_module(module, NULL) == 0);
    CHECK(capability_rejects(module, MIR_CAP_CVM));

    CvmState* vm = cvm_state_create(module, NULL);
    CHECK(vm != NULL);
    if (vm) {
        /* This direct embedding call intentionally bypasses the capability API. */
        CHECK(cvm_call_function(vm, main_func, NULL, 0) == 0);
        CHECK(vm->trap_code == CVM_TRAP_UNSUPPORTED_MIR);
        vm->trap_code = 0;
        CHECK(cvm_run(vm, "main", NULL, 0) == 0);
        CHECK(vm->trap_code == CVM_TRAP_UNSUPPORTED_MIR);
        cvm_state_destroy(vm);
    }
    mir_module_destroy(module);
}

static void test_supported_scalar(void) {
    MirModule* module = mir_module_create("scalar_capability_probe");
    MirType* i64 = mir_type_i64(module);
    MirFunction* main_func = mir_module_add_function(module, "main", i64, NULL, 0);
    MirBlock* entry = mir_function_add_block(main_func, "entry");
    MirBuilder builder;
    mir_builder_init(&builder, module, main_func);
    mir_builder_set_block(&builder, entry);
    MirValueId value = mir_build_const_int(&builder, 42, i64);
    mir_build_ret(&builder, value);

    CHECK(mir_backend_caps_validate(module, MIR_CAP_CVM, NULL));
    CvmState* vm = cvm_state_create(module, NULL);
    CHECK(vm != NULL);
    if (vm) {
        CHECK(cvm_run(vm, "main", NULL, 0) == 42);
        CHECK(vm->trap_code == 0);
        cvm_state_destroy(vm);
    }
    mir_module_destroy(module);
}

static void test_live_undef_rejected(void) {
    MirModule* module = mir_module_create("live_undef_probe");
    MirType* i64 = mir_type_i64(module);
    MirFunction* f = mir_module_add_function(module, "main", i64, NULL, 0);
    MirBlock* entry = mir_function_add_block(f, "entry");
    MirInst* undef = (MirInst*)mir_arena_alloc(module->arena, sizeof(MirInst));
    memset(undef, 0, sizeof(*undef));
    undef->opcode = MIR_UNDEF;
    undef->type = i64;
    undef->result = mir_function_new_value(f, i64);
    entry->first = entry->last = undef;
    entry->inst_count = 1;
    MirBuilder b;
    mir_builder_init(&b, module, f);
    mir_builder_set_block(&b, entry);
    mir_build_ret(&b, undef->result);
    CHECK(capability_rejects(module, MIR_CAP_CVM));
    CvmState* vm = cvm_state_create(module, NULL);
    CHECK(vm != NULL);
    if (vm) {
        (void)cvm_run(vm, "main", NULL, 0);
        CHECK(vm->trap_code == CVM_TRAP_UNSUPPORTED_MIR);
        cvm_state_destroy(vm);
    }
    mir_module_destroy(module);
}

static void test_bytecode_branch_and_call_rejection(void) {
    MirModule* branch_module = mir_module_create("branch_bytecode_probe");
    MirType* i64 = mir_type_i64(branch_module);
    MirFunction* f = mir_module_add_function(branch_module, "main", i64, NULL, 0);
    MirBlock* entry = mir_function_add_block(f, "entry");
    MirBlock* exit_block = mir_function_add_block(f, "exit");
    MirBuilder b;
    mir_builder_init(&b, branch_module, f);
    mir_builder_set_block(&b, entry);
    mir_build_br(&b, exit_block);
    mir_builder_set_block(&b, exit_block);
    MirValueId one = mir_build_const_int(&b, 1, i64);
    mir_build_ret(&b, one);
    CHECK(capability_rejects(branch_module, MIR_CAP_VM_BYTECODE));
    mir_module_destroy(branch_module);

    MirModule* call_module = mir_module_create("call_bytecode_probe");
    i64 = mir_type_i64(call_module);
    MirFunction* callee = mir_module_add_function(call_module, "callee", i64, NULL, 0);
    MirBlock* callee_entry = mir_function_add_block(callee, "entry");
    mir_builder_init(&b, call_module, callee);
    mir_builder_set_block(&b, callee_entry);
    one = mir_build_const_int(&b, 1, i64);
    mir_build_ret(&b, one);
    f = mir_module_add_function(call_module, "main", i64, NULL, 0);
    entry = mir_function_add_block(f, "entry");
    mir_builder_init(&b, call_module, f);
    mir_builder_set_block(&b, entry);
    MirValueId call = mir_build_call(&b, "callee", NULL, 0, i64);
    mir_build_ret(&b, call);
    CHECK(capability_rejects(call_module, MIR_CAP_VM_BYTECODE));
    mir_module_destroy(call_module);
}

static void test_unknown_opcode_fails_closed(void) {
    MirModule* module = mir_module_create("unknown_opcode_probe");
    MirType* i64 = mir_type_i64(module);
    MirFunction* f = mir_module_add_function(module, "main", i64, NULL, 0);
    MirBlock* entry = mir_function_add_block(f, "entry");
    MirBuilder b;
    mir_builder_init(&b, module, f);
    mir_builder_set_block(&b, entry);
    MirValueId value = mir_build_const_int(&b, 0, i64);
    mir_build_ret(&b, value);
    entry->first->opcode = (MirOpcode)999;

    CHECK(mir_verify_module(module, NULL) != 0);
    CHECK(capability_rejects(module, MIR_CAP_CVM));
    CvmState* vm = cvm_state_create(module, NULL);
    CHECK(vm != NULL);
    if (vm) {
        (void)cvm_run(vm, "main", NULL, 0);
        CHECK(vm->trap_code == CVM_TRAP_UNSUPPORTED_MIR);
        cvm_state_destroy(vm);
    }
    mir_module_destroy(module);
}

int main(void) {
    CHECK(strcmp(mir_opcode_name(MIR_UNDEF), "undef") == 0);
    CHECK(strcmp(mir_opcode_name(MIR_CALL), "call") == 0);
    test_vector_rejection();
    test_supported_scalar();
    test_live_undef_rejected();
    test_bytecode_branch_and_call_rejection();
    test_unknown_opcode_fails_closed();
    printf("MIR backend capability checks: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
