/*
 * Casprix Compiler — AST → MIR Lowering Implementation
 *
 * Core lowering pass: walks the typed AST and emits pre-SSA MIR.
 * Variables are represented as alloca slots; a subsequent mem2reg pass
 * promotes them to SSA values with phi nodes.
 *
 * Key lowering patterns:
 *   let x: i32 = expr   →  %a = alloca i32; %v = <lower expr>; store %a, %v
 *   x + y               →  %l = load %ax; %r = load %ay; %res = add %l, %r
 *   if c { A } else { B } → condbr %c, bb_then, bb_else; bb_merge
 *   while c { body }    →  bb_cond → condbr → bb_body → br bb_cond; bb_exit
 *   match expr { arms }  → switch + conditional branches
 *   obj.method(args)     →  call.virtual or call @mangled_name
 *   |captures| => expr   →  struct_init env; call.indirect
 */

#include "mir_lower.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ================================================================
 * Internal: variable map (name → alloca value)
 * ================================================================ */

static void var_map_reset(MirLowerCtx* ctx) {
    ctx->var_map_count = 0;
}

static void var_map_set(MirLowerCtx* ctx, const char* name,
                        MirValueId alloca_id, MirType* type) {
    /* Overwrite if exists */
    for (int i = 0; i < ctx->var_map_count; i++) {
        if (strcmp(ctx->var_map[i].name, name) == 0) {
            ctx->var_map[i].alloca_id = alloca_id;
            ctx->var_map[i].type = type;
            return;
        }
    }
    /* Append */
    if (ctx->var_map_count >= ctx->var_map_capacity) {
        ctx->var_map_capacity = ctx->var_map_capacity ? ctx->var_map_capacity * 2 : 64;
        ctx->var_map = realloc(ctx->var_map,
                       ctx->var_map_capacity * sizeof(ctx->var_map[0]));
    }
    ctx->var_map[ctx->var_map_count].name = name;
    ctx->var_map[ctx->var_map_count].alloca_id = alloca_id;
    ctx->var_map[ctx->var_map_count].type = type;
    ctx->var_map_count++;
}

static MirValueId var_map_get(MirLowerCtx* ctx, const char* name, MirType** out_type) {
    for (int i = ctx->var_map_count - 1; i >= 0; i--) {
        if (strcmp(ctx->var_map[i].name, name) == 0) {
            if (out_type) *out_type = ctx->var_map[i].type;
            return ctx->var_map[i].alloca_id;
        }
    }
    if (out_type) *out_type = NULL;
    return MIR_VALUE_NONE;
}

/* ================================================================
 * Type mapping: AST DataType → MIR MirType
 * ================================================================ */

static MirType* lower_type(MirLowerCtx* ctx, DataType dt) {
    switch (dt) {
        case TYPE_VOID:   return mir_type_void(ctx->module);
        case TYPE_BOOL:   return mir_type_bool(ctx->module);
        case TYPE_I8:     return mir_type_i8(ctx->module);
        case TYPE_I16:    return mir_type_i16(ctx->module);
        case TYPE_I32:    return mir_type_i32(ctx->module);
        case TYPE_I64:    return mir_type_i64(ctx->module);
        case TYPE_U8:     return mir_type_u8(ctx->module);
        case TYPE_U16:    return mir_type_u16(ctx->module);
        case TYPE_U32:    return mir_type_u32(ctx->module);
        case TYPE_U64:    return mir_type_u64(ctx->module);
        case TYPE_F32:    return mir_type_f32(ctx->module);
        case TYPE_F64:    return mir_type_f64(ctx->module);
        case TYPE_STRING: return mir_type_ptr(ctx->module, mir_type_i8(ctx->module));
        case TYPE_CHAR:   return mir_type_u32(ctx->module);  /* UTF-32 codepoint */
        case TYPE_PTR:
        case TYPE_RAWPTR:
        case TYPE_REF:    return mir_type_ptr(ctx->module, mir_type_i8(ctx->module));
        case TYPE_CLASS:  return mir_type_ptr(ctx->module, mir_type_i8(ctx->module));
        case TYPE_FUNC:   return mir_type_func(ctx->module, mir_type_void(ctx->module), NULL, 0);
        case TYPE_DYN:    return mir_type_ptr(ctx->module, mir_type_i8(ctx->module));
        default:          return mir_type_i64(ctx->module); /* fallback */
    }
}

static MirType* lower_type_info(MirLowerCtx* ctx, TypeInfo* info) {
    if (!info) {
        return NULL;
    }

    if (info->base == TYPE_FUNC) {
        MirType* ret_type = info->return_type
            ? lower_type_info(ctx, info->return_type)
            : mir_type_void(ctx->module);
        MirType** params = NULL;
        if (info->param_count > 0) {
            params = (MirType**)mir_arena_alloc(ctx->module->arena,
                                                info->param_count * sizeof(MirType*));
            for (int i = 0; i < info->param_count; i++) {
                params[i] = info->param_types && info->param_types[i]
                    ? lower_type_info(ctx, info->param_types[i])
                    : mir_type_i64(ctx->module);
            }
        }
        return mir_type_func(ctx->module, ret_type, params, info->param_count);
    }

    return lower_type(ctx, info->base);
}

static MirType* lower_expr_type(MirLowerCtx* ctx, Expr* expr) {
    MirType* info_type;

    if (!expr) {
        return mir_type_void(ctx->module);
    }

    info_type = lower_type_info(ctx, expr->type_info);
    if (info_type) {
        return info_type;
    }

    return lower_type(ctx, expr->data_type);
}

static MirType* lower_symbol_closure_handle_type(MirLowerCtx* ctx, Symbol* symbol);

static bool symbol_is_global_variable(Symbol* symbol) {
    return symbol &&
           symbol->kind == SYMBOL_VARIABLE &&
           symbol->scope_depth == 0;
}

static MirType* lower_symbol_storage_type(MirLowerCtx* ctx, Symbol* symbol) {
    int global_idx;

    if (!symbol) {
        return NULL;
    }

    if (symbol->is_closure_value) {
        return lower_symbol_closure_handle_type(ctx, symbol);
    }

    global_idx = mir_module_find_global(ctx->module, symbol->name);
    if (global_idx >= 0 && ctx->module->globals[global_idx].type) {
        return ctx->module->globals[global_idx].type;
    }

    return lower_type(ctx, symbol->type);
}

static MirType* lower_declaration_storage_type(MirLowerCtx* ctx,
                                               DeclarationStmt* decl,
                                               Symbol* symbol) {
    MirType* var_type = lower_type_info(ctx, decl->type_info);

    if (!var_type && decl->initializer && decl->initializer->type_info) {
        var_type = lower_type_info(ctx, decl->initializer->type_info);
    }

    if (symbol && symbol->is_closure_value) {
        var_type = lower_symbol_closure_handle_type(ctx, symbol);
    }

    if (!var_type) {
        var_type = lower_type(ctx, decl->type);
    }

    return var_type;
}

static MirValueId lower_global_addr(MirLowerCtx* ctx, const char* name,
                                    MirType* value_type, bool is_const) {
    MirType* storage_type = value_type ? value_type : mir_type_i64(ctx->module);

    if (mir_module_find_global(ctx->module, name) < 0) {
        mir_module_add_global(ctx->module, name, storage_type, is_const);
    }

    return mir_build_global_addr(&ctx->builder, name, storage_type);
}

static void register_global_declaration(MirLowerCtx* ctx, Stmt* stmt) {
    DeclarationStmt* decl;
    Symbol* symbol = NULL;
    MirType* storage_type;

    if (!stmt || (stmt->type != STMT_DECLARATION && stmt->type != STMT_CONST_DECL)) {
        return;
    }

    decl = &stmt->as.declaration;
    if (!decl->name) {
        return;
    }

    if (ctx->symtable) {
        symbol = lookup_symbol(ctx->symtable, decl->name);
    }

    storage_type = lower_declaration_storage_type(ctx, decl, symbol);
    mir_module_add_global(ctx->module, decl->name, storage_type, decl->is_const);
}

static MirType* lower_symbol_function_type(MirLowerCtx* ctx, Symbol* symbol) {
    MirType** params = NULL;
    MirType* ret_type;

    if (!symbol) {
        return mir_type_func(ctx->module, mir_type_void(ctx->module), NULL, 0);
    }

    ret_type = lower_type(ctx, symbol->return_type);
    if (symbol->param_count > 0) {
        params = (MirType**)mir_arena_alloc(ctx->module->arena,
                                            symbol->param_count * sizeof(MirType*));
        for (int i = 0; i < symbol->param_count; i++) {
            params[i] = lower_type(ctx, symbol->param_types[i]);
        }
    }

    return mir_type_func(ctx->module, ret_type, params, symbol->param_count);
}

static MirType* lower_capture_param_type(MirLowerCtx* ctx, DataType type) {
    return mir_type_ptr(ctx->module, lower_type(ctx, type));
}

static MirType* lower_lambda_invoke_type(MirLowerCtx* ctx, LambdaExpr* lam) {
    MirType** params = NULL;
    MirType* ret_type = lower_type(ctx, lam ? lam->return_type : TYPE_VOID);
    int total_params = lam ? (lam->param_count + lam->capture_count) : 0;

    if (total_params > 0) {
        params = (MirType**)mir_arena_alloc(ctx->module->arena,
                                            total_params * sizeof(MirType*));
        for (int i = 0; i < total_params; i++) {
            if (lam && i < lam->capture_count) {
                DataType capture_type = lam->captured_types
                    ? lam->captured_types[i]
                    : TYPE_I64;
                params[i] = lower_capture_param_type(ctx, capture_type);
            } else if (lam) {
                params[i] = lower_type(ctx, lam->parameters[i - lam->capture_count].type);
            }
        }
    }

    return mir_type_func(ctx->module, ret_type, params, total_params);
}

static MirType* lower_symbol_closure_invoke_type(MirLowerCtx* ctx, Symbol* symbol) {
    MirType** params = NULL;
    MirType* ret_type;
    int total_params;

    if (!symbol) {
        return mir_type_func(ctx->module, mir_type_void(ctx->module), NULL, 0);
    }

    ret_type = lower_type(ctx, symbol->return_type);
    total_params = symbol->param_count + symbol->closure_capture_count;
    if (total_params > 0) {
        params = (MirType**)mir_arena_alloc(ctx->module->arena,
                                            total_params * sizeof(MirType*));
        for (int i = 0; i < total_params; i++) {
            if (i < symbol->closure_capture_count) {
                DataType capture_type = symbol->closure_capture_types
                    ? symbol->closure_capture_types[i]
                    : TYPE_I64;
                params[i] = lower_capture_param_type(ctx, capture_type);
            } else {
                params[i] = lower_type(ctx, symbol->param_types[i - symbol->closure_capture_count]);
            }
        }
    }

    return mir_type_func(ctx->module, ret_type, params, total_params);
}

static MirType* lower_lambda_closure_handle_type(MirLowerCtx* ctx, LambdaExpr* lam) {
    MirType** fields;
    MirType* record_type;
    char closure_name[128];
    int field_count;

    if (!lam || lam->capture_count <= 0) {
        return mir_type_ptr(ctx->module, mir_type_i8(ctx->module));
    }

    field_count = lam->capture_count + 1;
    fields = (MirType**)mir_arena_alloc(ctx->module->arena,
                                        field_count * sizeof(MirType*));
    fields[0] = lower_lambda_invoke_type(ctx, lam);
    for (int i = 0; i < lam->capture_count; i++) {
        DataType capture_type = lam->captured_types ? lam->captured_types[i] : TYPE_I64;
        fields[i + 1] = lower_capture_param_type(ctx, capture_type);
    }

    snprintf(closure_name, sizeof(closure_name), "__closure_%d", lam->closure_id);
    record_type = mir_type_struct(ctx->module, closure_name, fields, field_count);
    return mir_type_ptr(ctx->module, record_type);
}

static MirType* lower_symbol_closure_handle_type(MirLowerCtx* ctx, Symbol* symbol) {
    MirType** fields;
    MirType* record_type;
    char closure_name[128];
    int field_count;

    if (!symbol || !symbol->is_closure_value) {
        return mir_type_ptr(ctx->module, mir_type_i8(ctx->module));
    }

    field_count = symbol->closure_capture_count + 1;
    fields = (MirType**)mir_arena_alloc(ctx->module->arena,
                                        field_count * sizeof(MirType*));
    fields[0] = lower_symbol_closure_invoke_type(ctx, symbol);
    for (int i = 0; i < symbol->closure_capture_count; i++) {
        DataType capture_type = symbol->closure_capture_types
            ? symbol->closure_capture_types[i]
            : TYPE_I64;
        fields[i + 1] = lower_capture_param_type(ctx, capture_type);
    }

    snprintf(closure_name, sizeof(closure_name), "__closure_%d", symbol->closure_lambda_id);
    record_type = mir_type_struct(ctx->module, closure_name, fields, field_count);
    return mir_type_ptr(ctx->module, record_type);
}

/* ================================================================
 * Expression lowering — recursive descent
 * ================================================================ */

static MirValueId lower_expr(MirLowerCtx* ctx, Expr* expr);
static void lower_stmt(MirLowerCtx* ctx, Stmt* stmt);
static MirValueId lower_lambda(MirLowerCtx* ctx, Expr* expr);

static MirValueId lower_literal(MirLowerCtx* ctx, Expr* expr) {
    LiteralExpr* lit = &expr->as.literal;
    switch (lit->type) {
        case TYPE_I32:
        case TYPE_I64:
        case TYPE_I8:
        case TYPE_I16:
            return mir_build_const_int(&ctx->builder, lit->value.int_value,
                                       lower_type(ctx, lit->type));
        case TYPE_U8:
        case TYPE_U16:
        case TYPE_U32:
        case TYPE_U64:
            return mir_build_const_int(&ctx->builder, lit->value.int_value,
                                       lower_type(ctx, lit->type));
        case TYPE_F32:
        case TYPE_F64:
            return mir_build_const_float(&ctx->builder, lit->value.float_value,
                                         lower_type(ctx, lit->type));
        case TYPE_BOOL:
            return mir_build_const_bool(&ctx->builder, lit->value.bool_value);
        case TYPE_STRING:
            return mir_build_const_string(&ctx->builder,
                     lit->value.string_value ? lit->value.string_value : "");
        default:
            return mir_build_const_int(&ctx->builder, lit->value.int_value,
                                       mir_type_i64(ctx->module));
    }
}

static MirValueId lower_variable(MirLowerCtx* ctx, Expr* expr) {
    VariableExpr* var = &expr->as.variable;
    MirType* var_type = NULL;
    MirValueId alloca_id = var_map_get(ctx, var->name, &var_type);
    Symbol* symbol = NULL;

    if (alloca_id == MIR_VALUE_NONE) {
        if (ctx->symtable) {
            symbol = lookup_symbol(ctx->symtable, var->name);
            if (symbol && symbol->kind == SYMBOL_FUNCTION) {
                return mir_build_const_func(&ctx->builder, var->name,
                                            lower_symbol_function_type(ctx, symbol));
            }
            if (symbol_is_global_variable(symbol)) {
                MirType* global_type = lower_symbol_storage_type(ctx, symbol);
                MirValueId global_ptr = lower_global_addr(ctx, var->name,
                                                          global_type,
                                                          symbol->is_const);
                return mir_build_load(&ctx->builder, global_ptr,
                                      global_type ? global_type : mir_type_i64(ctx->module));
            }
        }

        /* Global or external — emit as call to getter or raw symbol reference */
        return mir_build_const_int(&ctx->builder, 0, mir_type_i64(ctx->module));
    }

    if (var->is_move) {
        /* move semantics — load then invalidate */
        MirValueId loaded = mir_build_load(&ctx->builder, alloca_id,
                                            var_type ? var_type : mir_type_i64(ctx->module));
        return mir_build_move(&ctx->builder, loaded);
    }

    return mir_build_load(&ctx->builder, alloca_id,
                          var_type ? var_type : mir_type_i64(ctx->module));
}

static MirValueId lower_binary(MirLowerCtx* ctx, Expr* expr) {
    BinaryExpr* bin = &expr->as.binary;
    MirValueId lhs = lower_expr(ctx, bin->left);
    MirValueId rhs = lower_expr(ctx, bin->right);

    bool is_float = (bin->left->data_type == TYPE_F32 || bin->left->data_type == TYPE_F64);

    switch (bin->operator) {
        case TOKEN_PLUS:  return is_float ? mir_build_fadd(&ctx->builder, lhs, rhs)
                                          : mir_build_add(&ctx->builder, lhs, rhs);
        case TOKEN_MINUS: return is_float ? mir_build_fsub(&ctx->builder, lhs, rhs)
                                          : mir_build_sub(&ctx->builder, lhs, rhs);
        case TOKEN_STAR:  return is_float ? mir_build_fmul(&ctx->builder, lhs, rhs)
                                          : mir_build_mul(&ctx->builder, lhs, rhs);
        case TOKEN_SLASH: return is_float ? mir_build_fdiv(&ctx->builder, lhs, rhs)
                                          : mir_build_div(&ctx->builder, lhs, rhs);
        case TOKEN_PERCENT: return mir_build_mod(&ctx->builder, lhs, rhs);

        case TOKEN_EQUAL:       return mir_build_cmp_eq(&ctx->builder, lhs, rhs);
        case TOKEN_NOT_EQUAL:  return mir_build_cmp_ne(&ctx->builder, lhs, rhs);
        case TOKEN_LESS:        return mir_build_cmp_lt(&ctx->builder, lhs, rhs);
        case TOKEN_LESS_EQUAL:  return mir_build_cmp_le(&ctx->builder, lhs, rhs);
        case TOKEN_GREATER:     return mir_build_cmp_gt(&ctx->builder, lhs, rhs);
        case TOKEN_GREATER_EQUAL: return mir_build_cmp_ge(&ctx->builder, lhs, rhs);

        case TOKEN_BITAND:    return mir_build_and(&ctx->builder, lhs, rhs);
        case TOKEN_PIPE:      return mir_build_or(&ctx->builder, lhs, rhs);
        case TOKEN_BITXOR:    return mir_build_xor(&ctx->builder, lhs, rhs);

        default:
            return mir_build_add(&ctx->builder, lhs, rhs); /* fallback */
    }
}

static MirValueId lower_unary(MirLowerCtx* ctx, Expr* expr) {
    UnaryExpr* un = &expr->as.unary;
    MirValueId operand = lower_expr(ctx, un->operand);

    switch (un->operator) {
        case TOKEN_MINUS: {
            bool is_float = (un->operand->data_type == TYPE_F32 ||
                            un->operand->data_type == TYPE_F64);
            return is_float ? mir_build_fneg(&ctx->builder, operand)
                            : mir_build_neg(&ctx->builder, operand);
        }
        case TOKEN_NOT:
            return mir_build_not(&ctx->builder, operand);
        case TOKEN_BITAND:
            return mir_build_borrow(&ctx->builder, operand);
        default:
            return operand;
    }
}

static const char* lambda_symbol_name(MirLowerCtx* ctx, LambdaExpr* lam) {
    char lambda_name[128];
    int lambda_id = lam ? lam->closure_id : ctx->lambda_counter++;
    snprintf(lambda_name, sizeof(lambda_name), "__lambda_%d", lambda_id);
    return mir_arena_strdup(ctx->module->arena, lambda_name);
}

static MirValueId lower_captured_addr(MirLowerCtx* ctx, const char* name, DataType type) {
    MirType* var_type = NULL;
    MirValueId alloca_id = var_map_get(ctx, name, &var_type);

    (void)var_type;

    if (alloca_id == MIR_VALUE_NONE) {
        return mir_build_const_null(&ctx->builder, lower_capture_param_type(ctx, type));
    }

    return alloca_id;
}

static MirValueId build_lambda_closure_handle(MirLowerCtx* ctx, LambdaExpr* lam,
                                              const char* lambda_name) {
    MirType* closure_handle_type;
    MirType* record_type;
    MirValueId closure_record;
    MirValueId fn_ptr;

    if (!lam || lam->capture_count <= 0) {
        return mir_build_const_null(&ctx->builder,
                                    mir_type_ptr(ctx->module, mir_type_i8(ctx->module)));
    }

    closure_handle_type = lower_lambda_closure_handle_type(ctx, lam);
    record_type = (closure_handle_type && closure_handle_type->kind == MIR_TYPE_PTR)
        ? closure_handle_type->as.ptr.pointee
        : NULL;
    closure_record = mir_build_alloca(&ctx->builder,
                                      record_type ? record_type : mir_type_i64(ctx->module));
    fn_ptr = mir_build_const_func(&ctx->builder, lambda_name, lower_lambda_invoke_type(ctx, lam));

    {
        MirValueId slot_ptr = mir_build_get_field_ptr(&ctx->builder, closure_record, 0);
        mir_build_store(&ctx->builder, slot_ptr, fn_ptr);
    }

    for (int i = 0; i < lam->capture_count; i++) {
        DataType capture_type = lam->captured_types ? lam->captured_types[i] : TYPE_I64;
        MirValueId capture_addr = lower_captured_addr(ctx, lam->captured_vars[i], capture_type);
        MirValueId slot_ptr = mir_build_get_field_ptr(&ctx->builder, closure_record, i + 1);
        mir_build_store(&ctx->builder, slot_ptr, capture_addr);
    }

    return closure_record;
}

static MirValueId lower_closure_value_call(MirLowerCtx* ctx, Expr* expr, Symbol* callee_symbol) {
    CallExpr* call = &expr->as.call;
    MirValueId closure_handle;
    MirValueId fn_slot;
    MirValueId fn_ptr;
    MirValueId* args = NULL;
    MirType* ret_type = lower_expr_type(ctx, expr);
    MirType* invoke_type = lower_symbol_closure_invoke_type(ctx, callee_symbol);
    int total_args;

    if (!callee_symbol || !call->callee) {
        return MIR_VALUE_NONE;
    }

    closure_handle = lower_expr(ctx, call->callee);
    fn_slot = mir_build_get_field_ptr(&ctx->builder, closure_handle, 0);
    fn_ptr = mir_build_load(&ctx->builder, fn_slot, invoke_type);

    total_args = call->arg_count + callee_symbol->closure_capture_count;
    if (total_args > 0) {
        args = (MirValueId*)mir_arena_alloc(ctx->module->arena,
                                            total_args * sizeof(MirValueId));
    }

    for (int i = 0; i < callee_symbol->closure_capture_count; i++) {
        DataType capture_type = callee_symbol->closure_capture_types
            ? callee_symbol->closure_capture_types[i]
            : TYPE_I64;
        MirType* capture_ptr_type = lower_capture_param_type(ctx, capture_type);
        MirValueId capture_slot = mir_build_get_field_ptr(&ctx->builder, closure_handle, i + 1);
        args[i] = mir_build_load(&ctx->builder, capture_slot, capture_ptr_type);
    }

    for (int i = 0; i < call->arg_count; i++) {
        args[callee_symbol->closure_capture_count + i] = lower_expr(ctx, call->arguments[i]);
    }

    return mir_build_call_indirect(&ctx->builder, fn_ptr, args, total_args, ret_type);
}

static MirValueId lower_call(MirLowerCtx* ctx, Expr* expr) {
    CallExpr* call = &expr->as.call;
    Symbol* direct_symbol = NULL;

    if (call->callee &&
        call->callee->type == EXPR_VARIABLE &&
        call->callee->as.variable.name &&
        strcmp(call->callee->as.variable.name, "dyn") == 0) {
        if (call->arg_count == 1) {
            DataType arg_type = call->arguments[0] ? call->arguments[0]->data_type : TYPE_ERROR;
            if (arg_type == TYPE_DYN ||
                arg_type == TYPE_CLASS ||
                arg_type == TYPE_STRING ||
                arg_type == TYPE_PTR ||
                arg_type == TYPE_RAWPTR ||
                arg_type == TYPE_REF ||
                arg_type == TYPE_FUNC) {
                return lower_expr(ctx, call->arguments[0]);
            }
        }
        return mir_build_const_null(&ctx->builder,
                                    mir_type_ptr(ctx->module, mir_type_i8(ctx->module)));
    }

    if (call->callee &&
        call->callee->type == EXPR_LAMBDA &&
        call->callee->as.lambda.capture_count > 0) {
        LambdaExpr* lam = &call->callee->as.lambda;
        int total_args = lam->capture_count + call->arg_count;
        MirValueId* args = NULL;
        MirType* ret_type = lower_expr_type(ctx, expr);
        const char* lambda_name;

        lower_lambda(ctx, call->callee);
        lambda_name = lambda_symbol_name(ctx, lam);

        if (total_args > 0) {
            args = (MirValueId*)mir_arena_alloc(ctx->module->arena,
                    total_args * sizeof(MirValueId));
        }

        for (int i = 0; i < lam->capture_count; i++) {
            DataType capture_type = lam->captured_types
                ? lam->captured_types[i]
                : TYPE_I64;
            args[i] = lower_captured_addr(ctx, lam->captured_vars[i], capture_type);
        }
        for (int i = 0; i < call->arg_count; i++) {
            args[lam->capture_count + i] = lower_expr(ctx, call->arguments[i]);
        }

        return mir_build_call(&ctx->builder, lambda_name, args, total_args, ret_type);
    }

    MirValueId* args = NULL;
    if (call->arg_count > 0) {
        args = (MirValueId*)mir_arena_alloc(ctx->module->arena,
                call->arg_count * sizeof(MirValueId));
        for (int i = 0; i < call->arg_count; i++) {
            args[i] = lower_expr(ctx, call->arguments[i]);
        }
    }

    MirType* ret_type = lower_expr_type(ctx, expr);
    if (call->name && ctx->symtable) {
        direct_symbol = lookup_symbol(ctx->symtable, call->name);
        if (direct_symbol && direct_symbol->kind == SYMBOL_FUNCTION) {
            return mir_build_call(&ctx->builder, call->name, args, call->arg_count, ret_type);
        }
    }

    if (call->callee &&
        call->callee->type == EXPR_VARIABLE &&
        call->callee->as.variable.name &&
        ctx->symtable) {
        Symbol* callee_symbol = lookup_symbol(ctx->symtable, call->callee->as.variable.name);
        if (callee_symbol && callee_symbol->is_closure_value) {
            return lower_closure_value_call(ctx, expr, callee_symbol);
        }
    }

    if (!call->callee) {
        return MIR_VALUE_NONE;
    }

    MirValueId callee = lower_expr(ctx, call->callee);
    return mir_build_call_indirect(&ctx->builder, callee, args, call->arg_count, ret_type);
}

static const char* lower_object_class_name(Expr* object) {
    if (!object) return NULL;
    if (object->class_name) return object->class_name;

    switch (object->type) {
        case EXPR_THIS:
            return object->as.this_expr.class_name;
        case EXPR_NEW:
            return object->as.new_expr.class_name;
        default:
            return NULL;
    }
}

static int lower_field_index(MirLowerCtx* ctx, Expr* object, const char* member_name) {
    const char* class_name = lower_object_class_name(object);
    ClassSymbol* class_sym;
    FieldSymbol* field;

    if (!ctx->symtable || !class_name || !member_name) {
        return 0;
    }

    class_sym = lookup_class(ctx->symtable, class_name);
    if (!class_sym) {
        return 0;
    }

    field = find_field(class_sym, member_name);
    if (!field || field->offset < 0) {
        return 0;
    }

    return field->offset / 8;
}

static MirValueId lower_field_ptr(MirLowerCtx* ctx, Expr* object, const char* member_name) {
    MirValueId obj = lower_expr(ctx, object);
    int field_index = lower_field_index(ctx, object, member_name);
    return mir_build_get_field_ptr(&ctx->builder, obj, field_index);
}

static MirValueId lower_member_access(MirLowerCtx* ctx, Expr* expr) {
    MemberAccessExpr* ma = &expr->as.member;

    if (ma->is_method_call) {
        MirValueId obj = lower_expr(ctx, ma->object);
        /* Method call — use virtual dispatch for class instances */
        MirValueId* args = NULL;
        int total_args = ma->arg_count + 1; /* self + args */
        args = (MirValueId*)mir_arena_alloc(ctx->module->arena,
                total_args * sizeof(MirValueId));
        args[0] = obj;
        for (int i = 0; i < ma->arg_count; i++) {
            args[i + 1] = lower_expr(ctx, ma->arguments[i]);
        }

        /* Generate a mangled name: ClassName_methodName */
        char mangled[256];
        const char* class_name = ma->object->class_name ? ma->object->class_name : "Object";
        snprintf(mangled, sizeof(mangled), "%s_%s", class_name, ma->member_name);

        MirType* ret_type = lower_type(ctx, expr->data_type);
        return mir_build_call(&ctx->builder, mangled, args, total_args, ret_type);
    } else {
        /* Field access in expression position yields the field value. */
        MirValueId field_ptr = lower_field_ptr(ctx, ma->object, ma->member_name);
        MirType* field_type = lower_type(ctx, expr->data_type);
        return mir_build_load(&ctx->builder, field_ptr, field_type);
    }
}

static MirValueId lower_index(MirLowerCtx* ctx, Expr* expr) {
    IndexExpr* idx = &expr->as.index;
    MirValueId base = lower_expr(ctx, idx->array);
    MirValueId index = lower_expr(ctx, idx->index);
    MirValueId elem_ptr = mir_build_get_elem_ptr(&ctx->builder, base, index);
    MirType* elem_type = lower_type(ctx, expr->data_type);
    return mir_build_load(&ctx->builder, elem_ptr, elem_type);
}

static const char* find_constructor_method_name(MirLowerCtx* ctx, const char* class_name) {
    ClassSymbol* class_sym;

    if (!ctx || !ctx->symtable || !class_name) return NULL;

    class_sym = lookup_class(ctx->symtable, class_name);
    if (!class_sym) return NULL;

    for (int i = 0; i < class_sym->method_count; i++) {
        if (class_sym->methods[i].is_constructor &&
            strcmp(class_sym->methods[i].name, class_sym->name) == 0) {
            return class_sym->methods[i].name;
        }
    }

    for (int i = 0; i < class_sym->method_count; i++) {
        if (class_sym->methods[i].is_constructor) {
            return class_sym->methods[i].name;
        }
    }

    return NULL;
}

static MirValueId lower_new(MirLowerCtx* ctx, Expr* expr) {
    NewExpr* ne = &expr->as.new_expr;
    MirValueId obj = mir_build_obj_alloc(&ctx->builder, ne->class_name, 64);
    const char* ctor_method_name = find_constructor_method_name(ctx, ne->class_name);

    /* Call constructor if args present */
    if (ctor_method_name) {
        MirValueId* args = (MirValueId*)mir_arena_alloc(ctx->module->arena,
                            (ne->arg_count + 1) * sizeof(MirValueId));
        args[0] = obj;
        for (int i = 0; i < ne->arg_count; i++) {
            args[i + 1] = lower_expr(ctx, ne->arguments[i]);
        }
        char ctor_name[256];
        snprintf(ctor_name, sizeof(ctor_name), "%s_%s", ne->class_name, ctor_method_name);
        mir_build_call(&ctx->builder, ctor_name, args, ne->arg_count + 1,
                       mir_type_void(ctx->module));
    }

    return obj;
}

static MirValueId lower_lambda(MirLowerCtx* ctx, Expr* expr) {
    LambdaExpr* lam = &expr->as.lambda;
    const char* lambda_name = lambda_symbol_name(ctx, lam);

    /* Create MIR function for the lambda body */
    MirType* ret_type = lower_type(ctx, lam->return_type);
    int total_params = lam->param_count + lam->capture_count;

    MirParam* params = (MirParam*)mir_arena_alloc(ctx->module->arena,
                        total_params * sizeof(MirParam));

    int p = 0;
    for (int i = 0; i < lam->capture_count; i++) {
        params[p].name = lam->captured_vars ? lam->captured_vars[i] : "__capture";
        params[p].type = lower_capture_param_type(ctx,
                                    lam->captured_types ? lam->captured_types[i] : TYPE_I64);
        params[p].value_id = MIR_VALUE_NONE;
        p++;
    }
    for (int i = 0; i < lam->param_count; i++) {
        params[p].name = lam->parameters[i].name;
        params[p].type = lower_type(ctx, lam->parameters[i].type);
        params[p].value_id = MIR_VALUE_NONE;
        p++;
    }

    MirFunction* existing = mir_module_find_function(ctx->module, lambda_name);
    MirFunction* lambda_func = existing ? existing : mir_module_add_function(ctx->module, lambda_name,
                                                        ret_type, params, total_params);
    if (existing) {
        if (lam->capture_count > 0) {
            return build_lambda_closure_handle(ctx, lam, lambda_name);
        }
        return mir_build_const_func(&ctx->builder, lambda_name,
                                    lower_expr_type(ctx, expr));
    }

    /* Lower the lambda body in a separate context */
    MirBuilder saved_builder = ctx->builder;
    int saved_var_count = ctx->var_map_count;
    int saved_loop_depth = ctx->loop_depth;

    MirBlock* entry = mir_function_add_block(lambda_func, "entry");
    mir_builder_init(&ctx->builder, ctx->module, lambda_func);
    mir_builder_set_block(&ctx->builder, entry);

    /* Bind params to var_map */
    for (int i = 0; i < lambda_func->param_count; i++) {
        if (i < lam->capture_count) {
            DataType capture_type = lam->captured_types ? lam->captured_types[i] : TYPE_I64;
            MirType* capture_value_type = lower_type(ctx, capture_type);
            MirValueId capture_alloca = mir_build_alloca(&ctx->builder, capture_value_type);
            MirValueId capture_loaded = mir_build_load(&ctx->builder,
                                                       lambda_func->params[i].value_id,
                                                       capture_value_type);
            mir_build_store(&ctx->builder, capture_alloca, capture_loaded);
            var_map_set(ctx, lambda_func->params[i].name, capture_alloca, capture_value_type);
        } else {
            MirValueId param_alloca = mir_build_alloca(&ctx->builder, lambda_func->params[i].type);
            mir_build_store(&ctx->builder, param_alloca, lambda_func->params[i].value_id);
            var_map_set(ctx, lambda_func->params[i].name, param_alloca, lambda_func->params[i].type);
        }
    }

    /* Lower body */
    if (lam->is_expression && lam->expr_body) {
        MirValueId result = lower_expr(ctx, lam->expr_body);
        mir_build_ret(&ctx->builder, result);
    } else if (lam->block_body) {
        lower_stmt(ctx, lam->block_body);
    }

    if (!mir_block_is_terminated(ctx->builder.current_block)) {
        if (ret_type->kind == MIR_TYPE_VOID) {
            mir_build_ret_void(&ctx->builder);
        } else {
            MirValueId zero = mir_build_const_int(&ctx->builder, 0, ret_type);
            mir_build_ret(&ctx->builder, zero);
        }
    }

    /* Restore context */
    ctx->builder = saved_builder;
    ctx->var_map_count = saved_var_count;
    ctx->loop_depth = saved_loop_depth;

    if (lam->capture_count > 0) {
        return build_lambda_closure_handle(ctx, lam, lambda_name);
    }

    return mir_build_const_func(&ctx->builder, lambda_name,
                                lower_expr_type(ctx, expr));
}

static MirValueId lower_expr(MirLowerCtx* ctx, Expr* expr) {
    if (!expr) return MIR_VALUE_NONE;

    switch (expr->type) {
        case EXPR_LITERAL:       return lower_literal(ctx, expr);
        case EXPR_VARIABLE:      return lower_variable(ctx, expr);
        case EXPR_BINARY:        return lower_binary(ctx, expr);
        case EXPR_UNARY:         return lower_unary(ctx, expr);
        case EXPR_CALL:          return lower_call(ctx, expr);
        case EXPR_MEMBER_ACCESS: return lower_member_access(ctx, expr);
        case EXPR_INDEX:         return lower_index(ctx, expr);
        case EXPR_NEW:           return lower_new(ctx, expr);
        case EXPR_LAMBDA:        return lower_lambda(ctx, expr);
        case EXPR_THIS: {
            MirType* this_type = NULL;
            MirValueId this_alloca = var_map_get(ctx, "this", &this_type);
            if (this_alloca == MIR_VALUE_NONE) {
                return MIR_VALUE_NONE;
            }
            return mir_build_load(&ctx->builder, this_alloca,
                                  this_type ? this_type : mir_type_ptr(ctx->module, mir_type_i8(ctx->module)));
        }
        case EXPR_STATIC_ACCESS: {
            StaticAccessExpr* sa = &expr->as.static_access;
            if (sa->is_method_call) {
                MirValueId* args = NULL;
                if (sa->arg_count > 0) {
                    args = (MirValueId*)mir_arena_alloc(ctx->module->arena,
                            sa->arg_count * sizeof(MirValueId));
                    for (int i = 0; i < sa->arg_count; i++) {
                        args[i] = lower_expr(ctx, sa->arguments[i]);
                    }
                }
                char mangled[256];
                snprintf(mangled, sizeof(mangled), "%s_%s", sa->class_name, sa->member_name);
                return mir_build_call(&ctx->builder, mangled, args, sa->arg_count,
                                     lower_type(ctx, expr->data_type));
            }
            return mir_build_const_int(&ctx->builder, 0, mir_type_i64(ctx->module));
        }
        case EXPR_AWAIT: {
            MirValueId future = lower_expr(ctx, expr->as.await_expr.expression);
            MirBlock* resume_bb = mir_function_add_block(ctx->builder.func, "await.resume");
            MirValueId result = mir_build_suspend(&ctx->builder, resume_bb, future);
            mir_builder_set_block(&ctx->builder, resume_bb);
            return result;
        }
        case EXPR_SUPER:
        case EXPR_GENERIC_INST:
        default:
            return mir_build_const_int(&ctx->builder, 0, mir_type_i64(ctx->module));
    }
}

/* ================================================================
 * Statement lowering
 * ================================================================ */

static void lower_stmt(MirLowerCtx* ctx, Stmt* stmt);

static void lower_declaration(MirLowerCtx* ctx, Stmt* stmt) {
    DeclarationStmt* decl = &stmt->as.declaration;
    Symbol* symbol = NULL;
    MirType* var_type;

    if (ctx->symtable) {
        symbol = lookup_symbol(ctx->symtable, decl->name);
    }

    var_type = lower_declaration_storage_type(ctx, decl, symbol);

    if (ctx->lowering_toplevel) {
        MirValueId global_ptr = lower_global_addr(ctx, decl->name, var_type, decl->is_const);
        if (decl->initializer) {
            MirValueId init_val = lower_expr(ctx, decl->initializer);
            mir_build_store(&ctx->builder, global_ptr, init_val);
        }
        return;
    }

    MirValueId alloca_id = mir_build_alloca(&ctx->builder, var_type);
    var_map_set(ctx, decl->name, alloca_id, var_type);

    if (decl->initializer) {
        MirValueId init_val = lower_expr(ctx, decl->initializer);
        mir_build_store(&ctx->builder, alloca_id, init_val);
    }
}

static void lower_assignment(MirLowerCtx* ctx, Stmt* stmt) {
    AssignmentStmt* assign = &stmt->as.assignment;

    if (assign->target->type == EXPR_VARIABLE) {
        MirType* var_type = NULL;
        MirValueId alloca_id = var_map_get(ctx, assign->target->as.variable.name, &var_type);
        MirValueId value = lower_expr(ctx, assign->value);
        if (alloca_id != MIR_VALUE_NONE) {
            mir_build_store(&ctx->builder, alloca_id, value);
        } else if (ctx->symtable) {
            Symbol* symbol = lookup_symbol(ctx->symtable, assign->target->as.variable.name);
            if (symbol_is_global_variable(symbol)) {
                MirType* global_type = lower_symbol_storage_type(ctx, symbol);
                MirValueId global_ptr = lower_global_addr(ctx,
                                                          assign->target->as.variable.name,
                                                          global_type,
                                                          symbol->is_const);
                mir_build_store(&ctx->builder, global_ptr, value);
            }
        }
    } else if (assign->target->type == EXPR_MEMBER_ACCESS) {
        /* Field assignment: obj.field = value */
        MirValueId field_ptr = lower_field_ptr(ctx,
                                               assign->target->as.member.object,
                                               assign->target->as.member.member_name);
        MirValueId value = lower_expr(ctx, assign->value);
        mir_build_store(&ctx->builder, field_ptr, value);
    } else if (assign->target->type == EXPR_INDEX) {
        /* Array element assignment: arr[i] = value */
        MirValueId base = lower_expr(ctx, assign->target->as.index.array);
        MirValueId idx = lower_expr(ctx, assign->target->as.index.index);
        MirValueId elem_ptr = mir_build_get_elem_ptr(&ctx->builder, base, idx);
        MirValueId value = lower_expr(ctx, assign->value);
        mir_build_store(&ctx->builder, elem_ptr, value);
    }
}

static void lower_if(MirLowerCtx* ctx, Stmt* stmt) {
    IfStmt* if_s = &stmt->as.if_stmt;

    MirBlock* then_bb = mir_function_add_block(ctx->builder.func, "if.then");
    MirBlock* else_bb = if_s->else_branch
                        ? mir_function_add_block(ctx->builder.func, "if.else")
                        : NULL;
    MirBlock* merge_bb = mir_function_add_block(ctx->builder.func, "if.merge");

    MirValueId cond = lower_expr(ctx, if_s->condition);
    mir_build_condbr(&ctx->builder, cond, then_bb, else_bb ? else_bb : merge_bb);

    /* Then branch */
    mir_builder_set_block(&ctx->builder, then_bb);
    lower_stmt(ctx, if_s->then_branch);
    if (!mir_block_is_terminated(ctx->builder.current_block)) {
        mir_build_br(&ctx->builder, merge_bb);
    }

    /* Else branch */
    if (else_bb) {
        mir_builder_set_block(&ctx->builder, else_bb);
        lower_stmt(ctx, if_s->else_branch);
        if (!mir_block_is_terminated(ctx->builder.current_block)) {
            mir_build_br(&ctx->builder, merge_bb);
        }
    }

    mir_builder_set_block(&ctx->builder, merge_bb);
}

static void lower_while(MirLowerCtx* ctx, Stmt* stmt) {
    WhileStmt* w = &stmt->as.while_stmt;

    MirBlock* cond_bb = mir_function_add_block(ctx->builder.func, "while.cond");
    MirBlock* body_bb = mir_function_add_block(ctx->builder.func, "while.body");
    MirBlock* exit_bb = mir_function_add_block(ctx->builder.func, "while.exit");

    /* Push loop targets */
    assert(ctx->loop_depth < 32);
    ctx->loop_stack[ctx->loop_depth].break_bb = exit_bb;
    ctx->loop_stack[ctx->loop_depth].continue_bb = cond_bb;
    ctx->loop_depth++;

    mir_build_br(&ctx->builder, cond_bb);

    /* Condition */
    mir_builder_set_block(&ctx->builder, cond_bb);
    MirValueId cond = lower_expr(ctx, w->condition);
    mir_build_condbr(&ctx->builder, cond, body_bb, exit_bb);

    /* Body */
    mir_builder_set_block(&ctx->builder, body_bb);
    lower_stmt(ctx, w->body);
    if (!mir_block_is_terminated(ctx->builder.current_block)) {
        mir_build_br(&ctx->builder, cond_bb);
    }

    ctx->loop_depth--;
    mir_builder_set_block(&ctx->builder, exit_bb);
}

static void lower_for(MirLowerCtx* ctx, Stmt* stmt) {
    ForStmt* f = &stmt->as.for_stmt;

    /* Initializer */
    if (f->variable) {
        MirType* var_type = lower_type(ctx, f->var_type);
        MirValueId alloca_id = mir_build_alloca(&ctx->builder, var_type);
        var_map_set(ctx, f->variable, alloca_id, var_type);
        if (f->initializer) {
            MirValueId init = lower_expr(ctx, f->initializer);
            mir_build_store(&ctx->builder, alloca_id, init);
        }
    }

    MirBlock* cond_bb = mir_function_add_block(ctx->builder.func, "for.cond");
    MirBlock* body_bb = mir_function_add_block(ctx->builder.func, "for.body");
    MirBlock* incr_bb = mir_function_add_block(ctx->builder.func, "for.incr");
    MirBlock* exit_bb = mir_function_add_block(ctx->builder.func, "for.exit");

    ctx->loop_stack[ctx->loop_depth].break_bb = exit_bb;
    ctx->loop_stack[ctx->loop_depth].continue_bb = incr_bb;
    ctx->loop_depth++;

    mir_build_br(&ctx->builder, cond_bb);

    /* Condition */
    mir_builder_set_block(&ctx->builder, cond_bb);
    if (f->condition) {
        MirValueId cond = lower_expr(ctx, f->condition);
        mir_build_condbr(&ctx->builder, cond, body_bb, exit_bb);
    } else {
        mir_build_br(&ctx->builder, body_bb); /* infinite loop */
    }

    /* Body */
    mir_builder_set_block(&ctx->builder, body_bb);
    lower_stmt(ctx, f->body);
    if (!mir_block_is_terminated(ctx->builder.current_block)) {
        mir_build_br(&ctx->builder, incr_bb);
    }

    /* Increment */
    mir_builder_set_block(&ctx->builder, incr_bb);
    if (f->increment) {
        lower_stmt(ctx, f->increment);
    }
    if (!mir_block_is_terminated(ctx->builder.current_block)) {
        mir_build_br(&ctx->builder, cond_bb);
    }

    ctx->loop_depth--;
    mir_builder_set_block(&ctx->builder, exit_bb);
}

static void lower_match(MirLowerCtx* ctx, Stmt* stmt) {
    MatchStmt* m = &stmt->as.match_stmt;
    MirValueId subject = lower_expr(ctx, m->subject);
    MirBlock* merge_bb = mir_function_add_block(ctx->builder.func, "match.merge");

    /* Lower each arm as a conditional chain */
    MirBlock* next_test = NULL;
    for (int i = 0; i < m->arm_count; i++) {
        MatchArm* arm = &m->arms[i];
        MirBlock* arm_bb = mir_function_add_block(ctx->builder.func, "match.arm");

        if (arm->pattern == NULL) {
            /* Wildcard — always taken */
            mir_build_br(&ctx->builder, arm_bb);
        } else {
            next_test = (i + 1 < m->arm_count)
                        ? mir_function_add_block(ctx->builder.func, "match.test")
                        : merge_bb;
            MirValueId pat_val = lower_expr(ctx, arm->pattern);
            MirValueId cond = mir_build_cmp_eq(&ctx->builder, subject, pat_val);
            mir_build_condbr(&ctx->builder, cond, arm_bb, next_test);
        }

        mir_builder_set_block(&ctx->builder, arm_bb);
        if (arm->body) lower_stmt(ctx, arm->body);
        if (!mir_block_is_terminated(ctx->builder.current_block)) {
            mir_build_br(&ctx->builder, merge_bb);
        }

        if (next_test && next_test != merge_bb) {
            mir_builder_set_block(&ctx->builder, next_test);
        }
    }

    mir_builder_set_block(&ctx->builder, merge_bb);
}

static void lower_return(MirLowerCtx* ctx, Stmt* stmt) {
    ReturnStmt* ret = &stmt->as.return_stmt;
    if (ret->value) {
        MirValueId val = lower_expr(ctx, ret->value);
        mir_build_ret(&ctx->builder, val);
    } else {
        mir_build_ret_void(&ctx->builder);
    }
}

static void lower_print(MirLowerCtx* ctx, Stmt* stmt) {
    PrintStmt* p = &stmt->as.print;
    MirValueId val = lower_expr(ctx, p->expression);
    MirValueId args[1] = { val };
    const char* print_fn = "nuwan_print_str";
    if (p->expression->data_type == TYPE_INT || p->expression->data_type == TYPE_I32 || p->expression->data_type == TYPE_I64) {
        print_fn = "nuwan_print_int";
    } else if (p->expression->data_type == TYPE_FLOAT || p->expression->data_type == TYPE_F32 || p->expression->data_type == TYPE_F64) {
        print_fn = "nuwan_print_float";
    } else if (p->expression->data_type == TYPE_BOOL) {
        print_fn = "nuwan_print_bool";
    }
    mir_build_call(&ctx->builder, print_fn, args, 1, mir_type_void(ctx->module));
}

static void lower_block(MirLowerCtx* ctx, Stmt* stmt) {
    BlockStmt* block = &stmt->as.block;
    for (int i = 0; i < block->stmt_count; i++) {
        lower_stmt(ctx, block->statements[i]);
        /* Stop if block was terminated (return/break/continue) */
        if (mir_block_is_terminated(ctx->builder.current_block)) break;
    }
}

static void lower_stmt(MirLowerCtx* ctx, Stmt* stmt) {
    if (!stmt) return;

    switch (stmt->type) {
        case STMT_DECLARATION:
        case STMT_CONST_DECL:
            lower_declaration(ctx, stmt);
            break;
        case STMT_ASSIGNMENT:
            lower_assignment(ctx, stmt);
            break;
        case STMT_IF:
            lower_if(ctx, stmt);
            break;
        case STMT_WHILE:
            lower_while(ctx, stmt);
            break;
        case STMT_FOR:
            lower_for(ctx, stmt);
            break;
        case STMT_MATCH:
            lower_match(ctx, stmt);
            break;
        case STMT_RETURN:
            lower_return(ctx, stmt);
            break;
        case STMT_BREAK:
            if (ctx->loop_depth > 0) {
                mir_build_br(&ctx->builder, ctx->loop_stack[ctx->loop_depth - 1].break_bb);
            }
            break;
        case STMT_CONTINUE:
            if (ctx->loop_depth > 0) {
                mir_build_br(&ctx->builder, ctx->loop_stack[ctx->loop_depth - 1].continue_bb);
            }
            break;
        case STMT_PRINT:
            lower_print(ctx, stmt);
            break;
        case STMT_EXPR:
            lower_expr(ctx, stmt->as.expr_stmt.expression);
            break;
        case STMT_BLOCK:
            lower_block(ctx, stmt);
            break;
        case STMT_FUNCTION:
            /* Nested function — lower as separate MIR function */
            mir_lower_function(ctx, stmt);
            break;
        case STMT_FOR_IN:
        case STMT_TRY:
        case STMT_THROW:
        case STMT_INCLUDE:
        case STMT_CLASS:
        case STMT_EXTERN:
        case STMT_STRUCT:
        case STMT_ENUM:
        case STMT_UNION:
        case STMT_TRAIT:
        case STMT_IMPL:
            /* Complex constructs — handled individually or deferred */
            break;
    }
}

/* ================================================================
 * Function lowering
 * ================================================================ */

MirFunction* mir_lower_function(MirLowerCtx* ctx, Stmt* func_stmt) {
    FunctionStmt* f = &func_stmt->as.function;

    MirType* ret_type = lower_type(ctx, f->return_type);

    MirParam* params = NULL;
    if (f->param_count > 0) {
        params = (MirParam*)mir_arena_alloc(ctx->module->arena,
                  f->param_count * sizeof(MirParam));
        for (int i = 0; i < f->param_count; i++) {
            params[i].name = f->parameters[i].name;
            params[i].type = lower_type(ctx, f->parameters[i].type);
            params[i].value_id = MIR_VALUE_NONE;
        }
    }

    MirFunction* mir_func = mir_module_add_function(ctx->module, f->name,
                                                     ret_type, params, f->param_count);

    /* Save and set up context */
    MirBuilder saved_builder = ctx->builder;
    int saved_var_count = ctx->var_map_count;
    int saved_loop_depth = ctx->loop_depth;
    ctx->loop_depth = 0;

    MirBlock* entry = mir_function_add_block(mir_func, "entry");
    mir_builder_init(&ctx->builder, ctx->module, mir_func);
    mir_func->is_async = f->is_async;
    mir_builder_set_block(&ctx->builder, entry);

    /* Bind parameters to alloca slots */
    for (int i = 0; i < mir_func->param_count; i++) {
        MirValueId param_alloca = mir_build_alloca(&ctx->builder, mir_func->params[i].type);
        mir_build_store(&ctx->builder, param_alloca, mir_func->params[i].value_id);
        var_map_set(ctx, mir_func->params[i].name, param_alloca, mir_func->params[i].type);
    }

    /* Lower body */
    if (f->body) {
        lower_stmt(ctx, f->body);
    }

    /* Ensure function is terminated */
    if (!mir_block_is_terminated(ctx->builder.current_block)) {
        if (ret_type->kind == MIR_TYPE_VOID) {
            mir_build_ret_void(&ctx->builder);
        } else {
            /* Return zero as default — semantic analysis should catch this */
            MirValueId zero = mir_build_const_int(&ctx->builder, 0, ret_type);
            mir_build_ret(&ctx->builder, zero);
        }
    }

    /* Restore context */
    ctx->builder = saved_builder;
    ctx->var_map_count = saved_var_count;
    ctx->loop_depth = saved_loop_depth;

    return mir_func;
}

/* ================================================================
 * Top-level: lower entire program
 * ================================================================ */

static void lower_class(MirLowerCtx* ctx, Stmt* stmt) {
    ClassStmt* cls = &stmt->as.class_stmt;

    /* Lower each method as a standalone function with mangled name */
    for (int i = 0; i < cls->method_count; i++) {
        MethodDecl* m = &cls->methods[i];
        if (!m->body && !m->is_constructor) continue;

        char mangled[256];
        snprintf(mangled, sizeof(mangled), "%s_%s", cls->name, m->name);

        /* Build param list: self + method params */
        int total_params = m->param_count + 1;
        MirParam* params = (MirParam*)mir_arena_alloc(ctx->module->arena,
                            total_params * sizeof(MirParam));
        params[0].name = "this";
        params[0].type = mir_type_ptr(ctx->module, mir_type_i8(ctx->module));
        params[0].value_id = MIR_VALUE_NONE;
        for (int j = 0; j < m->param_count; j++) {
            params[j + 1].name = m->parameters[j].name;
            params[j + 1].type = lower_type(ctx, m->parameters[j].type);
            params[j + 1].value_id = MIR_VALUE_NONE;
        }

        MirType* ret_type = lower_type(ctx, m->return_type);
        MirFunction* mir_func = mir_module_add_function(ctx->module, mangled,
                                                         ret_type, params, total_params);

        MirBuilder saved = ctx->builder;
        int saved_vars = ctx->var_map_count;
        ctx->loop_depth = 0;

        MirBlock* entry = mir_function_add_block(mir_func, "entry");
        mir_builder_init(&ctx->builder, ctx->module, mir_func);
        mir_builder_set_block(&ctx->builder, entry);

        for (int j = 0; j < mir_func->param_count; j++) {
            MirValueId pa = mir_build_alloca(&ctx->builder, mir_func->params[j].type);
            mir_build_store(&ctx->builder, pa, mir_func->params[j].value_id);
            var_map_set(ctx, mir_func->params[j].name, pa, mir_func->params[j].type);
        }

        if (m->body) lower_stmt(ctx, m->body);
        if (!mir_block_is_terminated(ctx->builder.current_block)) {
            mir_build_ret_void(&ctx->builder);
        }

        ctx->builder = saved;
        ctx->var_map_count = saved_vars;
    }
}

static void lower_impl(MirLowerCtx* ctx, Stmt* stmt) {
    ImplStmt* impl = &stmt->as.impl_stmt;

    for (int i = 0; i < impl->method_count; i++) {
        if (impl->methods[i] && impl->methods[i]->type == STMT_FUNCTION) {
            FunctionStmt* m = &impl->methods[i]->as.function;

            char mangled[256];
            snprintf(mangled, sizeof(mangled), "%s_%s", impl->target_name, m->name);

            /* Temporarily rename the function for lowering */
            char* original_name = m->name;
            m->name = mangled;
            mir_lower_function(ctx, impl->methods[i]);
            m->name = original_name;
        }
    }
}

static void lower_extern(MirLowerCtx* ctx, Stmt* stmt) {
    ExternStmt* ext = &stmt->as.extern_stmt;
    MirType* ret_type = lower_type(ctx, ext->return_type);

    MirParam* params = NULL;
    if (ext->param_count > 0) {
        params = (MirParam*)mir_arena_alloc(ctx->module->arena,
                  ext->param_count * sizeof(MirParam));
        for (int i = 0; i < ext->param_count; i++) {
            params[i].name = ext->parameters[i].name;
            params[i].type = lower_type(ctx, ext->parameters[i].type);
            params[i].value_id = MIR_VALUE_NONE;
        }
    }

    MirFunction* func = mir_module_add_function(ctx->module, ext->name,
                                                 ret_type, params, ext->param_count);
    func->is_extern = true;
}

MirModule* mir_lower_program(Stmt** statements, int stmt_count,
                              SymbolTable* symtable, const char* module_name) {
    MirModule* module = mir_module_create(module_name);

    MirLowerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.module = module;
    ctx.symtable = symtable;
    ctx.var_map = NULL;
    ctx.var_map_count = 0;
    ctx.var_map_capacity = 0;
    ctx.loop_depth = 0;
    ctx.lambda_counter = 0;
    ctx.lowering_toplevel = false;
    ctx.error_count = 0;

    /* First pass: collect externs and type declarations */
    for (int i = 0; i < stmt_count; i++) {
        if (!statements[i]) continue;
        switch (statements[i]->type) {
            case STMT_EXTERN:
                lower_extern(&ctx, statements[i]);
                break;
            case STMT_STRUCT: {
                /* Register struct as MIR type */
                StructStmt* s = &statements[i]->as.struct_stmt;
                MirType** fields = NULL;
                if (s->field_count > 0) {
                    fields = (MirType**)mir_arena_alloc(module->arena,
                              s->field_count * sizeof(MirType*));
                    for (int j = 0; j < s->field_count; j++) {
                        fields[j] = lower_type(&ctx, s->fields[j].type);
                    }
                }
                mir_type_struct(module, s->name, fields, s->field_count);
                break;
            }
            default:
                break;
        }
    }

    /* Second pass: register top-level globals so functions can reference them. */
    for (int i = 0; i < stmt_count; i++) {
        if (!statements[i]) continue;
        switch (statements[i]->type) {
            case STMT_DECLARATION:
            case STMT_CONST_DECL:
                register_global_declaration(&ctx, statements[i]);
                break;
            default:
                break;
        }
    }

    /* Third pass: lower functions, classes, impls */
    for (int i = 0; i < stmt_count; i++) {
        if (!statements[i]) continue;
        switch (statements[i]->type) {
            case STMT_FUNCTION:
                var_map_reset(&ctx);
                mir_lower_function(&ctx, statements[i]);
                break;
            case STMT_CLASS:
                lower_class(&ctx, statements[i]);
                break;
            case STMT_IMPL:
                lower_impl(&ctx, statements[i]);
                break;
            default:
                break; /* Handled below as top-level code */
        }
    }

    /* Fourth pass: lower top-level statements into a synthetic entry function.
     * This captures let-declarations, print, expression statements, etc.
     * that appear at file scope outside any fn/class/impl.
     * The function is named "__casprix_entry" and has void→void signature.
     * The legacy AST codegen already emits these as _start / main, so this
     * mirrors that behaviour on the MIR path. */
    {
        bool has_toplevel = false;
        for (int i = 0; i < stmt_count; i++) {
            if (!statements[i]) continue;
            switch (statements[i]->type) {
                case STMT_FUNCTION: case STMT_CLASS: case STMT_IMPL:
                case STMT_EXTERN: case STMT_STRUCT: case STMT_ENUM:
                case STMT_UNION: case STMT_TRAIT: case STMT_INCLUDE:
                    break;
                default:
                    has_toplevel = true;
                    break;
            }
            if (has_toplevel) break;
        }

        if (has_toplevel) {
            var_map_reset(&ctx);

            MirType* void_type = mir_type_void(module);
            MirFunction* entry_func = mir_module_add_function(
                module, "__casprix_entry", void_type, NULL, 0);

            MirBlock* entry_bb = mir_function_add_block(entry_func, "entry");
            mir_builder_init(&ctx.builder, module, entry_func);
            mir_builder_set_block(&ctx.builder, entry_bb);
            ctx.lowering_toplevel = true;

            for (int i = 0; i < stmt_count; i++) {
                if (!statements[i]) continue;
                switch (statements[i]->type) {
                    case STMT_FUNCTION: case STMT_CLASS: case STMT_IMPL:
                    case STMT_EXTERN: case STMT_STRUCT: case STMT_ENUM:
                    case STMT_UNION: case STMT_TRAIT: case STMT_INCLUDE:
                        break;   /* already handled */
                    default:
                        lower_stmt(&ctx, statements[i]);
                        break;
                }
            }

            /* Ensure the entry function is terminated */
            if (!mir_block_is_terminated(ctx.builder.current_block)) {
                mir_build_ret_void(&ctx.builder);
            }

            ctx.lowering_toplevel = false;
        }
    }

    free(ctx.var_map);
    return module;
}
