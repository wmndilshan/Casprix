#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "compiler/ir/mir.h"
#include "compiler/ir/mir_mem2reg.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name) do { \
    if (cond) { \
        printf("  [PASS] %s\n", name); \
        g_pass++; \
    } else { \
        printf("  [FAIL] %s\n", name); \
        g_fail++; \
    } \
} while (0)

static MirInst* append_raw_inst(MirFunction* func, MirBlock* block,
                                MirOpcode opcode, MirType* type, MirValueId result) {
    MirInst* inst = (MirInst*)mir_arena_alloc(func->parent->arena, sizeof(MirInst));
    memset(inst, 0, sizeof(*inst));
    inst->opcode = opcode;
    inst->type = type;
    inst->result = result;
    mir_block_append(block, inst);
    return inst;
}

static void test_function_params_initialized(void) {
    MirModule* module = mir_module_create("params");
    MirType* i64 = mir_type_i64(module);
    MirParam params[1] = { { "x", i64, MIR_VALUE_NONE } };
    MirFunction* func = mir_module_add_function(module, "with_param", i64, params, 1);

    printf("\n--- function parameter value types ---\n");
    CHECK(func->param_count == 1, "function keeps parameter count");
    CHECK(func->params[0].value_id != MIR_VALUE_NONE, "parameter gets SSA value id");
    CHECK(mir_function_value_type(func, func->params[0].value_id) == i64,
          "parameter value type table initialized before SSA ids");

    mir_module_destroy(module);
}

static void test_valid_if_else_phi(void) {
    MirModule* module = mir_module_create("if_else");
    MirType* i64 = mir_type_i64(module);
    MirFunction* func = mir_module_add_function(module, "phi_ok", i64, NULL, 0);
    MirBuilder builder;
    MirBlock* entry = mir_function_add_block(func, "entry");
    MirBlock* then_bb = mir_function_add_block(func, "then");
    MirBlock* else_bb = mir_function_add_block(func, "else");
    MirBlock* merge_bb = mir_function_add_block(func, "merge");
    MirInst* phi_inst;
    MirValueId cond;
    MirValueId one;
    MirValueId two;
    MirValueId phi_value;

    printf("\n--- valid if/else phi ---\n");
    mir_builder_init(&builder, module, func);

    mir_builder_set_block(&builder, entry);
    cond = mir_build_const_bool(&builder, true);
    mir_build_condbr(&builder, cond, then_bb, else_bb);

    mir_builder_set_block(&builder, then_bb);
    one = mir_build_const_int(&builder, 1, i64);
    mir_build_br(&builder, merge_bb);

    mir_builder_set_block(&builder, else_bb);
    two = mir_build_const_int(&builder, 2, i64);
    mir_build_br(&builder, merge_bb);

    mir_builder_set_block(&builder, merge_bb);
    phi_value = mir_build_phi(&builder, i64);
    phi_inst = merge_bb->last;
    mir_phi_add_edge(phi_inst, then_bb, one);
    mir_phi_add_edge(phi_inst, else_bb, two);
    mir_build_ret(&builder, phi_value);

    CHECK(mir_validate_function(func), "if/else phi validates");
    mir_module_destroy(module);
}

static void test_valid_loop_phi(void) {
    MirModule* module = mir_module_create("loop");
    MirType* i64 = mir_type_i64(module);
    MirFunction* func = mir_module_add_function(module, "loop_phi_ok", i64, NULL, 0);
    MirBuilder builder;
    MirBlock* entry = mir_function_add_block(func, "entry");
    MirBlock* loop_bb = mir_function_add_block(func, "loop");
    MirBlock* body_bb = mir_function_add_block(func, "body");
    MirBlock* exit_bb = mir_function_add_block(func, "exit");
    MirInst* phi_inst;
    MirValueId zero;
    MirValueId one;
    MirValueId cond;
    MirValueId phi_value;
    MirValueId next_value;

    printf("\n--- valid loop-carried phi ---\n");
    mir_builder_init(&builder, module, func);

    mir_builder_set_block(&builder, entry);
    zero = mir_build_const_int(&builder, 0, i64);
    mir_build_br(&builder, loop_bb);

    mir_builder_set_block(&builder, loop_bb);
    phi_value = mir_build_phi(&builder, i64);
    phi_inst = loop_bb->last;
    mir_phi_add_edge(phi_inst, entry, zero);
    cond = mir_build_const_bool(&builder, true);
    mir_build_condbr(&builder, cond, body_bb, exit_bb);

    mir_builder_set_block(&builder, body_bb);
    one = mir_build_const_int(&builder, 1, i64);
    next_value = mir_build_add(&builder, phi_value, one);
    mir_build_br(&builder, loop_bb);
    mir_phi_add_edge(phi_inst, body_bb, next_value);

    mir_builder_set_block(&builder, exit_bb);
    mir_build_ret(&builder, phi_value);

    CHECK(mir_validate_function(func), "loop-carried phi validates");
    mir_module_destroy(module);
}

static void test_mem2reg_keeps_function_valid(void) {
    MirModule* module = mir_module_create("mem2reg");
    MirType* i64 = mir_type_i64(module);
    MirParam params[1] = { { "x", i64, MIR_VALUE_NONE } };
    MirFunction* func = mir_module_add_function(module, "mem2reg_ok", i64, params, 1);
    MirBuilder builder;
    MirValueId slot;
    MirValueId loaded;
    MirMem2RegStats stats = {0};

    printf("\n--- mem2reg validation ---\n");
    mir_builder_init(&builder, module, func);
    mir_builder_set_block(&builder, mir_function_add_block(func, "entry"));
    slot = mir_build_alloca(&builder, i64);
    mir_build_store(&builder, slot, func->params[0].value_id);
    loaded = mir_build_load(&builder, slot, i64);
    mir_build_ret(&builder, loaded);

    CHECK(mir_validate_function(func), "function valid before mem2reg");
    mir_mem2reg(func, &stats);
    CHECK(mir_validate_function(func), "function valid after mem2reg");
    CHECK(stats.allocas_promoted == 1, "mem2reg promoted single alloca");
    mir_module_destroy(module);
}

static void test_unreachable_block_is_allowed(void) {
    MirModule* module = mir_module_create("unreachable");
    MirType* i64 = mir_type_i64(module);
    MirFunction* func = mir_module_add_function(module, "unreachable_ok", i64, NULL, 0);
    MirBuilder builder;
    MirBlock* entry = mir_function_add_block(func, "entry");
    MirBlock* dead = mir_function_add_block(func, "dead");

    printf("\n--- unreachable block ---\n");
    mir_builder_init(&builder, module, func);
    mir_builder_set_block(&builder, entry);
    mir_build_ret(&builder, mir_build_const_int(&builder, 0, i64));
    mir_builder_set_block(&builder, dead);
    mir_build_ret(&builder, mir_build_const_int(&builder, 1, i64));

    CHECK(mir_validate_function(func), "terminated unreachable block is tolerated");
    mir_module_destroy(module);
}

static void test_malformed_predecessor_list_fails(void) {
    MirModule* module = mir_module_create("bad_preds");
    MirType* i64 = mir_type_i64(module);
    MirFunction* func = mir_module_add_function(module, "bad_preds", i64, NULL, 0);
    MirBuilder builder;
    MirBlock* entry = mir_function_add_block(func, "entry");
    MirBlock* then_bb = mir_function_add_block(func, "then");
    MirBlock* else_bb = mir_function_add_block(func, "else");
    MirBlock* merge_bb = mir_function_add_block(func, "merge");
    MirInst* phi_inst;
    MirValueId cond;
    MirValueId one;
    MirValueId two;

    printf("\n--- malformed predecessor list ---\n");
    mir_builder_init(&builder, module, func);

    mir_builder_set_block(&builder, entry);
    cond = mir_build_const_bool(&builder, true);
    mir_build_condbr(&builder, cond, then_bb, else_bb);

    mir_builder_set_block(&builder, then_bb);
    one = mir_build_const_int(&builder, 1, i64);
    mir_build_br(&builder, merge_bb);

    mir_builder_set_block(&builder, else_bb);
    two = mir_build_const_int(&builder, 2, i64);
    mir_build_br(&builder, merge_bb);

    mir_builder_set_block(&builder, merge_bb);
    (void)mir_build_phi(&builder, i64);
    phi_inst = merge_bb->last;
    mir_phi_add_edge(phi_inst, then_bb, one);
    mir_phi_add_edge(phi_inst, else_bb, two);
    mir_build_ret(&builder, phi_inst->result);

    merge_bb->pred_count = 1;
    CHECK(!mir_validate_function(func), "phi/pred mismatch is rejected");
    mir_module_destroy(module);
}

static void test_use_before_definition_fails(void) {
    MirModule* module = mir_module_create("use_before_def");
    MirType* i64 = mir_type_i64(module);
    MirFunction* func = mir_module_add_function(module, "use_before_def", i64, NULL, 0);
    MirBlock* entry = mir_function_add_block(func, "entry");
    MirValueId late_value = mir_function_new_value(func, i64);
    MirValueId result_value = mir_function_new_value(func, i64);
    MirInst* add_inst;
    MirInst* const_inst;
    MirInst* ret_inst;

    printf("\n--- use before definition ---\n");
    add_inst = append_raw_inst(func, entry, MIR_ADD, i64, result_value);
    add_inst->as.binary.lhs = late_value;
    add_inst->as.binary.rhs = late_value;

    const_inst = append_raw_inst(func, entry, MIR_CONST_INT, i64, late_value);
    const_inst->as.imm_i64 = 7;

    ret_inst = append_raw_inst(func, entry, MIR_RET, mir_type_void(module), MIR_VALUE_NONE);
    ret_inst->as.ret.value = result_value;

    CHECK(!mir_validate_function(func), "same-block use-before-def is rejected");
    mir_module_destroy(module);
}

static void test_non_dominating_definition_fails(void) {
    MirModule* module = mir_module_create("non_dom");
    MirType* i64 = mir_type_i64(module);
    MirFunction* func = mir_module_add_function(module, "non_dom", i64, NULL, 0);
    MirBuilder builder;
    MirBlock* entry = mir_function_add_block(func, "entry");
    MirBlock* then_bb = mir_function_add_block(func, "then");
    MirBlock* else_bb = mir_function_add_block(func, "else");
    MirBlock* merge_bb = mir_function_add_block(func, "merge");
    MirValueId cond;
    MirValueId only_then;

    printf("\n--- non-dominating definition ---\n");
    mir_builder_init(&builder, module, func);

    mir_builder_set_block(&builder, entry);
    cond = mir_build_const_bool(&builder, true);
    mir_build_condbr(&builder, cond, then_bb, else_bb);

    mir_builder_set_block(&builder, then_bb);
    only_then = mir_build_const_int(&builder, 42, i64);
    mir_build_br(&builder, merge_bb);

    mir_builder_set_block(&builder, else_bb);
    mir_build_br(&builder, merge_bb);

    mir_builder_set_block(&builder, merge_bb);
    mir_build_ret(&builder, only_then);

    CHECK(!mir_validate_function(func), "merge use without dominating def is rejected");
    mir_module_destroy(module);
}

static void test_duplicate_ssa_definition_fails(void) {
    MirModule* module = mir_module_create("dup_def");
    MirType* i64 = mir_type_i64(module);
    MirFunction* func = mir_module_add_function(module, "dup_def", i64, NULL, 0);
    MirBlock* entry = mir_function_add_block(func, "entry");
    MirValueId reused = mir_function_new_value(func, i64);
    MirInst* a;
    MirInst* b;
    MirInst* ret_inst;

    printf("\n--- duplicate SSA definition ---\n");
    a = append_raw_inst(func, entry, MIR_CONST_INT, i64, reused);
    a->as.imm_i64 = 1;
    b = append_raw_inst(func, entry, MIR_CONST_INT, i64, reused);
    b->as.imm_i64 = 2;
    ret_inst = append_raw_inst(func, entry, MIR_RET, mir_type_void(module), MIR_VALUE_NONE);
    ret_inst->as.ret.value = reused;

    CHECK(!mir_validate_function(func), "duplicate SSA definition is rejected");
    mir_module_destroy(module);
}

static void test_instruction_after_terminator_fails(void) {
    MirModule* module = mir_module_create("after_term");
    MirType* i64 = mir_type_i64(module);
    MirFunction* func = mir_module_add_function(module, "after_term", i64, NULL, 0);
    MirBlock* entry = mir_function_add_block(func, "entry");
    MirInst* ret_inst;
    MirInst* extra_inst;

    printf("\n--- instruction after terminator ---\n");
    ret_inst = append_raw_inst(func, entry, MIR_RET, mir_type_void(module), MIR_VALUE_NONE);
    ret_inst->as.ret.value = MIR_VALUE_NONE;
    extra_inst = append_raw_inst(func, entry, MIR_CONST_INT, i64, mir_function_new_value(func, i64));
    extra_inst->as.imm_i64 = 9;

    CHECK(!mir_validate_function(func), "instruction after terminator is rejected");
    mir_module_destroy(module);
}

int main(void) {
    printf("=== MIR Validation Regression Tests ===\n");
    test_function_params_initialized();
    test_valid_if_else_phi();
    test_valid_loop_phi();
    test_mem2reg_keeps_function_valid();
    test_unreachable_block_is_allowed();
    test_malformed_predecessor_list_fails();
    test_use_before_definition_fails();
    test_non_dominating_definition_fails();
    test_duplicate_ssa_definition_fails();
    test_instruction_after_terminator_fails();

    printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
