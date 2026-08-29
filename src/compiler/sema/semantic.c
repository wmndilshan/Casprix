#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include "compiler/sema/semantic.h"
#include "compiler/sema/escape_analysis.h"
#include "compiler/sema/drop_planner.h"
#include "compiler/sema/ownership_check.h"
#include "compiler/sema/linear_view.h"
#include "compiler/middle/closure.h"
#include "support/error.h"

static void analyze_expr(SemanticAnalyzer* analyzer, Expr* expr);
static void analyze_stmt(SemanticAnalyzer* analyzer, Stmt* stmt);
static void analyze_static_access_expr(SemanticAnalyzer* analyzer, Expr* expr);
static void analyze_lambda_expr(SemanticAnalyzer* analyzer, Expr* expr);
static void analyze_await_expr(SemanticAnalyzer* analyzer, Expr* expr);

/* ─── Global memory-model analysis contexts ─── */
static EscapeAnalyzer   g_escape_ctx;
static DropPlanner      g_drop_ctx;
static OwnershipChecker g_ownership_ctx;
/* Per-function log of StringView bindings; reset at every function entry
 * so escape promotion (which runs after all local scopes have already been
 * torn down) can still recover the view ↔ parent map. */
static LinearViewLog    g_linear_view_log;
static bool             g_memory_model_inited = false;

void init_semantic_analyzer(SemanticAnalyzer* analyzer) {
    analyzer->symbols = ALLOCATE(SymbolTable, 1);
    init_symbol_table(analyzer->symbols);
    analyzer->scope_depth = 0;
    analyzer->current_function_return_type = TYPE_VOID;
    analyzer->current_lambda_return_type = NULL;
    analyzer->in_function = false;
    analyzer->in_async_function = false;
    analyzer->current_class = NULL;
    analyzer->current_method = NULL;
    analyzer->loop_depth = 0;  // Initialize loop depth tracking
    analyzer->alloc_scope_depth = 0;
    init_closure_analyzer();

    /* Initialize memory-model analysis subsystems */
    if (!g_memory_model_inited) {
        escape_analyzer_init(&g_escape_ctx);
        drop_planner_init(&g_drop_ctx);
        ownership_checker_init(&g_ownership_ctx, analyzer);
        g_memory_model_inited = true;
    }
}

void free_semantic_analyzer(SemanticAnalyzer* analyzer) {
    if (analyzer->symbols) {
        free_symbol_table(analyzer->symbols);
        free(analyzer->symbols);
    }
    g_memory_model_inited = false;
}

static const char* type_to_string(DataType type) {
    return datatype_to_string(type);
}

static bool types_compatible(DataType t1, DataType t2) {
    if (t1 == TYPE_ERROR || t2 == TYPE_ERROR) return true;
    if (t1 == t2) return true;
    if (t1 == TYPE_DYN || t2 == TYPE_DYN) return false;

    // Allow implicit conversions between numeric types
    // Integer widening: i8 -> i16 -> i32 -> i64 -> i128
    // Unsigned widening: u8 -> u16 -> u32 -> u64 -> u128
    // Int -> Float widening: any integer -> f32/f64
    // Float widening: f16 -> f32 -> f64
    if (type_is_numeric(t1) && type_is_numeric(t2)) {
        return true;  // Allow all numeric conversions (semantic check warns on narrowing)
    }

    // char is compatible with integer types
    if ((t1 == TYPE_CHAR && type_is_integer(t2)) ||
        (type_is_integer(t1) && t2 == TYPE_CHAR)) {
        return true;
    }

    // string and strbuf are compatible (string -> strbuf conversion)
    if ((t1 == TYPE_STRING && t2 == TYPE_STRBUF) ||
        (t1 == TYPE_STRBUF && t2 == TYPE_STRING)) {
        return true;
    }

    // class/struct and ref are compatible (implicit reference taking)
    if ((t1 == TYPE_CLASS || t1 == TYPE_STRUCT) && t2 == TYPE_REF) {
        return true;
    }

    // Allow integer literal 0 (null) to be assigned to pointer types (C-FFI interop)
    // This enables: let p: rawptr = 0
    if (type_is_integer(t1) && (t2 == TYPE_RAWPTR || t2 == TYPE_PTR || t2 == TYPE_REF)) {
        return true;
    }
    if (type_is_integer(t2) && (t1 == TYPE_RAWPTR || t1 == TYPE_PTR || t1 == TYPE_REF)) {
        return true;
    }

    return false;
}

// Determine the result type of a binary operation between two numeric types
// Uses promotion rules: wider type wins, float > int, signed > unsigned at same width
static DataType get_result_type(DataType t1, DataType t2) {
    // String concatenation
    if (t1 == TYPE_STRING && t2 == TYPE_STRING) return TYPE_STRING;

    // If either is a float, result is the wider float
    if (type_is_float(t1) || type_is_float(t2)) {
        if (t1 == TYPE_F64 || t2 == TYPE_F64) return TYPE_F64;
        if (t1 == TYPE_F32 || t2 == TYPE_F32) return TYPE_F32;
        if (t1 == TYPE_F16 || t2 == TYPE_F16) return TYPE_F16;
        if (t1 == TYPE_BF16 || t2 == TYPE_BF16) return TYPE_BF16;
        return TYPE_F64;
    }

    // Both are integers - use the wider type
    if (type_is_integer(t1) && type_is_integer(t2)) {
        // Get the byte sizes
        int s1 = type_size_bytes(t1);
        int s2 = type_size_bytes(t2);
        if (s1 >= s2) return t1;
        return t2;
    }

    return TYPE_ERROR;
}

static bool dyn_packable_type(DataType type) {
    return type == TYPE_DYN ||
           type == TYPE_CLASS ||
           type == TYPE_STRING ||
           type == TYPE_PTR ||
           type == TYPE_RAWPTR ||
           type == TYPE_REF ||
           type == TYPE_FUNC;
}

static TypeInfo* build_function_type_info(DataType* param_types, int param_count,
                                          DataType return_type) {
    TypeInfo* info = create_type_info(TYPE_FUNC);
    info->param_count = param_count;
    if (param_count > 0) {
        info->param_types = ALLOCATE(TypeInfo*, param_count);
        for (int i = 0; i < param_count; i++) {
            info->param_types[i] = create_type_info(param_types[i]);
        }
    }
    info->return_type = create_type_info(return_type);
    return info;
}

/* Like build_function_type_info, but if `rich_return` is a structured function
 * TypeInfo (e.g. from a `-> lambda(int) -> int` return annotation), use a clone
 * of it as the return type so callers can arity-check the returned callable. */
static TypeInfo* build_function_type_info_ex(DataType* param_types, int param_count,
                                             DataType return_type,
                                             const TypeInfo* rich_return) {
    TypeInfo* info = build_function_type_info(param_types, param_count, return_type);
    if (rich_return && rich_return->base == TYPE_FUNC) {
        if (info->return_type) free_type_info(info->return_type);
        info->return_type = clone_type_info((TypeInfo*)rich_return);
    }
    return info;
}

static TypeInfo* build_function_type_info_from_params(Parameter* params, int param_count,
                                                      DataType return_type) {
    TypeInfo* info = create_type_info(TYPE_FUNC);
    info->param_count = param_count;
    if (param_count > 0) {
        info->param_types = ALLOCATE(TypeInfo*, param_count);
        for (int i = 0; i < param_count; i++) {
            info->param_types[i] = create_type_info(params[i].type);
        }
    }
    info->return_type = create_type_info(return_type);
    return info;
}

/* Copy a `lambda(P...) -> R` signature (parsed into a TypeInfo) onto a
 * function-typed variable/parameter symbol, so a call through that binding is
 * arity- and (loosely) type-checked against the declared signature rather than
 * defaulting to zero parameters. */
static void apply_fn_signature_to_symbol(Symbol* symbol, const TypeInfo* fn_info) {
    if (!symbol || !fn_info || fn_info->base != TYPE_FUNC) return;

    if (symbol->param_types) {
        free(symbol->param_types);
        symbol->param_types = NULL;
    }
    symbol->param_count = fn_info->param_count;
    if (fn_info->param_count > 0) {
        symbol->param_types = ALLOCATE(DataType, fn_info->param_count);
        for (int i = 0; i < fn_info->param_count; i++) {
            symbol->param_types[i] = fn_info->param_types && fn_info->param_types[i]
                ? fn_info->param_types[i]->base
                : TYPE_ERROR;
        }
    }
    symbol->return_type = fn_info->return_type ? fn_info->return_type->base : TYPE_VOID;
}

static void clear_symbol_closure_info(Symbol* symbol) {
    if (!symbol) return;
    symbol->is_closure_value = false;
    symbol->closure_capture_count = 0;
    symbol->closure_lambda_id = -1;
    if (symbol->closure_capture_types) {
        free(symbol->closure_capture_types);
        symbol->closure_capture_types = NULL;
    }
}

static void set_symbol_closure_info(Symbol* symbol, LambdaExpr* lambda) {
    if (!symbol) return;

    clear_symbol_closure_info(symbol);
    if (!lambda || lambda->capture_count <= 0) {
        return;
    }

    symbol->is_closure_value = true;
    symbol->closure_capture_count = lambda->capture_count;
    symbol->closure_lambda_id = lambda->closure_id;
    if (lambda->capture_count > 0) {
        symbol->closure_capture_types = ALLOCATE(DataType, lambda->capture_count);
        for (int i = 0; i < lambda->capture_count; i++) {
            symbol->closure_capture_types[i] = lambda->captured_types
                ? lambda->captured_types[i]
                : TYPE_I64;
        }
    }
}

static void copy_symbol_closure_info(Symbol* dst, Symbol* src) {
    if (!dst) return;

    clear_symbol_closure_info(dst);
    if (!src || !src->is_closure_value) {
        return;
    }

    dst->is_closure_value = true;
    dst->closure_capture_count = src->closure_capture_count;
    dst->closure_lambda_id = src->closure_lambda_id;
    if (src->closure_capture_count > 0 && src->closure_capture_types) {
        dst->closure_capture_types = ALLOCATE(DataType, src->closure_capture_count);
        memcpy(dst->closure_capture_types, src->closure_capture_types,
               sizeof(DataType) * src->closure_capture_count);
    }
}

static bool expr_is_stack_closure_value(SemanticAnalyzer* analyzer, Expr* expr) {
    Symbol* symbol;

    if (!expr) return false;

    if (expr->type == EXPR_LAMBDA) {
        return expr->as.lambda.capture_count > 0;
    }

    if (expr->type != EXPR_VARIABLE || !expr->as.variable.name) {
        return false;
    }

    symbol = lookup_symbol(analyzer->symbols, expr->as.variable.name);
    if (symbol && symbol->is_closure_value) {
        VariableExpr* var = &expr->as.variable;
        var->is_closure_value = true;
        var->closure_capture_count = symbol->closure_capture_count;
        var->closure_lambda_id = symbol->closure_lambda_id;
        if (symbol->closure_capture_types && symbol->closure_capture_count > 0 && !var->closure_capture_types) {
            var->closure_capture_types = ALLOCATE(DataType, symbol->closure_capture_count);
            memcpy(var->closure_capture_types, symbol->closure_capture_types, sizeof(DataType) * symbol->closure_capture_count);
        }
    }
    return symbol && symbol->is_closure_value;
}

// Check if access to a member is allowed based on access modifier
static bool can_access_member(SemanticAnalyzer* analyzer, ClassSymbol* target_class,
                              AccessModifier access, int line, int col, const char* member_name) {
    // Public members are always accessible
    if (access == ACCESS_PUBLIC) {
        return true;
    }

    // If not in a class context, can only access public members
    if (!analyzer->current_class) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                "Cannot access %s member '%s' from outside a class",
                access == ACCESS_PRIVATE ? "private" : "protected", member_name);
        report_semantic_error(line, col, msg);
        return false;
    }

    // Private members can only be accessed from the same class
    if (access == ACCESS_PRIVATE) {
        if (analyzer->current_class != target_class) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                    "Cannot access private member '%s' of class '%s' from class '%s'",
                    member_name, target_class->name, analyzer->current_class->name);
            report_semantic_error(line, col, msg);
            return false;
        }
        return true;
    }

    // Protected members can be accessed from same class or derived classes
    if (access == ACCESS_PROTECTED) {
        if (analyzer->current_class == target_class ||
            is_subclass_of(analyzer->current_class, target_class)) {
            return true;
        }

        char msg[256];
        snprintf(msg, sizeof(msg),
                "Cannot access protected member '%s' of class '%s' from unrelated class '%s'",
                member_name, target_class->name, analyzer->current_class->name);
        report_semantic_error(line, col, msg);
        return false;
    }

    return true;
}

static void analyze_binary_expr(SemanticAnalyzer* analyzer, Expr* expr) {
    BinaryExpr* binary = &expr->as.binary;
    
    if (!binary->left || !binary->right) {
        expr->data_type = TYPE_ERROR;
        return;
    }
    
    analyze_expr(analyzer, binary->left);
    analyze_expr(analyzer, binary->right);
    
    DataType left_type = binary->left->data_type;
    DataType right_type = binary->right->data_type;
    
    if (left_type == TYPE_ERROR || right_type == TYPE_ERROR) {
        expr->data_type = TYPE_ERROR;
        return;
    }
    
    switch (binary->operator) {
        case TOKEN_PLUS:
            // String concatenation or numeric addition
            if (left_type == TYPE_STRING || right_type == TYPE_STRING) {
                expr->data_type = TYPE_STRING;
            } else if (type_is_numeric(left_type) && type_is_numeric(right_type)) {
                expr->data_type = get_result_type(left_type, right_type);
            } else {
                report_type_error(expr->line, expr->column,
                    "Cannot add incompatible types");
                expr->data_type = TYPE_ERROR;
            }
            break;

        case TOKEN_MINUS:
        case TOKEN_STAR:
        case TOKEN_SLASH:
            if (type_is_numeric(left_type) && type_is_numeric(right_type)) {
                expr->data_type = get_result_type(left_type, right_type);
            } else {
                report_type_error(expr->line, expr->column,
                    "Arithmetic operators require numeric types");
                expr->data_type = TYPE_ERROR;
            }
            break;

        case TOKEN_PERCENT:
            if (type_is_integer(left_type) && type_is_integer(right_type)) {
                expr->data_type = get_result_type(left_type, right_type);
            } else {
                report_type_error(expr->line, expr->column,
                    "Modulo operator requires integer types");
                expr->data_type = TYPE_ERROR;
            }
            break;

        case TOKEN_LESS:
        case TOKEN_LESS_EQUAL:
        case TOKEN_GREATER:
        case TOKEN_GREATER_EQUAL:
            if (type_is_numeric(left_type) && type_is_numeric(right_type)) {
                expr->data_type = TYPE_BOOL;
            } else {
                report_type_error(expr->line, expr->column,
                    "Comparison operators require numeric types");
                expr->data_type = TYPE_ERROR;
            }
            break;
            
        case TOKEN_EQUAL:
        case TOKEN_NOT_EQUAL:
            if (types_compatible(left_type, right_type)) {
                expr->data_type = TYPE_BOOL;
            } else {
                report_type_error(expr->line, expr->column,
                    "Cannot compare incompatible types");
                expr->data_type = TYPE_ERROR;
            }
            break;
            
        case TOKEN_AND:
        case TOKEN_OR:
            if (left_type == TYPE_BOOL && right_type == TYPE_BOOL) {
                expr->data_type = TYPE_BOOL;
            } else {
                report_type_error(expr->line, expr->column,
                    "Logical operators require boolean operands");
                expr->data_type = TYPE_ERROR;
            }
            break;
            
        default:
            expr->data_type = TYPE_ERROR;
    }
}

static void analyze_unary_expr(SemanticAnalyzer* analyzer, Expr* expr) {
    UnaryExpr* unary = &expr->as.unary;
    
    if (!unary->operand) {
        expr->data_type = TYPE_ERROR;
        return;
    }
    
    analyze_expr(analyzer, unary->operand);
    
    DataType operand_type = unary->operand->data_type;
    
    if (operand_type == TYPE_ERROR) {
        expr->data_type = TYPE_ERROR;
        return;
    }
    
    switch (unary->operator) {
        case TOKEN_MINUS:
            if (type_is_numeric(operand_type)) {
                expr->data_type = operand_type;
            } else {
                report_type_error(expr->line, expr->column,
                    "Unary minus requires numeric type");
                expr->data_type = TYPE_ERROR;
            }
            break;
            
        case TOKEN_NOT:
            if (operand_type == TYPE_BOOL) {
                expr->data_type = TYPE_BOOL;
            } else {
                report_type_error(expr->line, expr->column,
                    "Logical NOT requires boolean type");
                expr->data_type = TYPE_ERROR;
            }
            break;
            
        default:
            expr->data_type = TYPE_ERROR;
    }
}

static void analyze_variable_expr(SemanticAnalyzer* analyzer, Expr* expr) {
    VariableExpr* var = &expr->as.variable;
    
    if (!var->name) {
        expr->data_type = TYPE_ERROR;
        return;
    }
    
    Symbol* symbol = lookup_symbol(analyzer->symbols, var->name);
    if (!symbol) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Undefined variable '%s'", var->name);
        report_semantic_error(expr->line, expr->column, msg);
        expr->data_type = TYPE_ERROR;
        return;
    }

    /* Verify ownership/borrowing validity */
    if (!check_ownership_valid(&g_ownership_ctx, var->name, expr->line)) {
        // Error reported by check_ownership_valid
        expr->data_type = TYPE_ERROR;
        return;
    }

    if (symbol->kind == SYMBOL_FUNCTION) {
        expr->data_type = TYPE_FUNC;
        if (expr->type_info) {
            free_type_info(expr->type_info);
        }
        expr->type_info = build_function_type_info_ex(symbol->param_types,
                                                     symbol->param_count,
                                                     symbol->return_type,
                                                     symbol->return_type_info);
        return;
    }

    if (symbol->kind != SYMBOL_VARIABLE && symbol->kind != SYMBOL_PARAMETER) {
        char msg[256];
        snprintf(msg, sizeof(msg), "'%s' is not a variable", var->name);
        report_semantic_error(expr->line, expr->column, msg);
        expr->data_type = TYPE_ERROR;
        return;
    }

    // Parameters are always initialized (passed by caller)
    // Only check initialization for variables
    // NOTE: We're being lenient here to avoid false positives from complex control flow
    // A full data flow analysis would be needed for perfect tracking
    if (symbol->kind == SYMBOL_VARIABLE && !symbol->is_initialized) {
        // For now, we only warn, not error, to allow complex initialization patterns
        // This is acceptable since uninitialized variables will cause runtime errors anyway
        // A production compiler would do full dataflow analysis
    }

    if (symbol->is_closure_value) {
        var->is_closure_value = true;
        var->closure_capture_count = symbol->closure_capture_count;
        var->closure_lambda_id = symbol->closure_lambda_id;
        if (symbol->closure_capture_types && symbol->closure_capture_count > 0) {
            var->closure_capture_types = ALLOCATE(DataType, symbol->closure_capture_count);
            memcpy(var->closure_capture_types, symbol->closure_capture_types, sizeof(DataType) * symbol->closure_capture_count);
        } else {
            var->closure_capture_types = NULL;
        }
    } else {
        var->is_closure_value = false;
        var->closure_capture_count = 0;
        var->closure_lambda_id = -1;
        var->closure_capture_types = NULL;
    }

    expr->data_type = symbol->type;

    if (symbol->type == TYPE_FUNC) {
        if (expr->type_info) {
            free_type_info(expr->type_info);
        }
        expr->type_info = build_function_type_info_ex(symbol->param_types,
                                                     symbol->param_count,
                                                     symbol->return_type,
                                                     symbol->return_type_info);
    }

    // If it's a class type, propagate the class name
    if (symbol->type == TYPE_CLASS && symbol->class_info) {
        /* This expr may be visited more than once (loop-body fixpoint) — free
         * any name from a previous visit before overwriting. */
        if (expr->class_name) free(expr->class_name);
        expr->class_name = strdup(symbol->class_info->name);
    }
}

static void analyze_call_expr(SemanticAnalyzer* analyzer, Expr* expr) {
    CallExpr* call = &expr->as.call;
    Symbol* callee_symbol = NULL;
    bool supported_callable = false;

    if (!call->callee) {
        expr->data_type = TYPE_ERROR;
        return;
    }

    if (call->callee->type == EXPR_VARIABLE &&
        call->callee->as.variable.name &&
        strcmp(call->callee->as.variable.name, "dyn") == 0) {
        if (call->arg_count != 1) {
            report_semantic_error(expr->line, expr->column,
                "dyn(...) expects exactly one argument");
            expr->data_type = TYPE_ERROR;
            return;
        }

        analyze_expr(analyzer, call->arguments[0]);
        if (call->arguments[0]->data_type == TYPE_ERROR) {
            expr->data_type = TYPE_ERROR;
            return;
        }

        if (!dyn_packable_type(call->arguments[0]->data_type)) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "dyn(...) currently supports only reference-like values (got %s)",
                     type_to_string(call->arguments[0]->data_type));
            report_type_error(expr->line, expr->column, msg);
            expr->data_type = TYPE_ERROR;
            return;
        }

        expr->data_type = TYPE_DYN;
        expr->class_name = NULL;
        return;
    }

    analyze_expr(analyzer, call->callee);
    if (call->callee->data_type == TYPE_ERROR) {
        expr->data_type = TYPE_ERROR;
        return;
    }

    if (call->callee->data_type != TYPE_FUNC) {
        report_semantic_error(expr->line, expr->column, "Call target is not callable");
        expr->data_type = TYPE_ERROR;
        return;
    }

    if (call->callee->type == EXPR_VARIABLE &&
        call->callee->as.variable.name) {
        callee_symbol = lookup_symbol(analyzer->symbols, call->callee->as.variable.name);
        if (callee_symbol &&
            (callee_symbol->kind == SYMBOL_FUNCTION ||
             ((callee_symbol->kind == SYMBOL_VARIABLE ||
               callee_symbol->kind == SYMBOL_PARAMETER) &&
              callee_symbol->type == TYPE_FUNC))) {
            supported_callable = true;
        }
    }

    if (!supported_callable && call->callee->type == EXPR_LAMBDA) {
        /* Mutable captures are supported: mutated captures are bound by
         * reference in codegen (asmgen.c / mir_lower.c) so the mutation is
         * visible to the enclosing scope. UNCHECKED SAFETY (v1): the borrow
         * checker does not yet reason about a closure aliasing the variables
         * it captures by reference, nor about two such closures aliasing the
         * same variable. Returning such a closure is still rejected below
         * (see the "Returning capturing closure values" gate). */
        supported_callable = true;
    }

    if (!supported_callable && call->name) {
        Symbol* direct_symbol = lookup_symbol(analyzer->symbols, call->name);
        if (direct_symbol && direct_symbol->kind == SYMBOL_FUNCTION) {
            supported_callable = true;
        }
    }

    if (!supported_callable) {
        report_semantic_error(expr->line, expr->column,
            "Callable expressions other than named functions and function-valued bindings are not implemented yet");
        expr->data_type = TYPE_ERROR;
        return;
    }

    TypeInfo* signature = call->callee->type_info;
    if (!signature || signature->base != TYPE_FUNC || !signature->return_type) {
        report_semantic_error(expr->line, expr->column,
            "Callable expression is missing function signature information");
        expr->data_type = TYPE_ERROR;
        return;
    }

    if (call->arg_count != signature->param_count) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Callable expression expects %d argument(s), got %d",
                 signature->param_count, call->arg_count);
        report_semantic_error(expr->line, expr->column, msg);
        expr->data_type = TYPE_ERROR;
        return;
    }

    bool has_error = false;
    for (int i = 0; i < call->arg_count; i++) {
        if (!call->arguments[i]) {
            has_error = true;
            continue;
        }
        
        analyze_expr(analyzer, call->arguments[i]);
        
        if (call->arguments[i]->data_type == TYPE_ERROR) {
            has_error = true;
            continue;
        }

        DataType expected_type = TYPE_ERROR;
        if (signature->param_types && signature->param_types[i]) {
            expected_type = signature->param_types[i]->base;
        }

        if (expected_type != TYPE_ERROR &&
            !types_compatible(call->arguments[i]->data_type, expected_type)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Argument %d type mismatch (expected %s, got %s)",
                     i + 1,
                     type_to_string(expected_type),
                     type_to_string(call->arguments[i]->data_type));
            report_type_error(call->arguments[i]->line, call->arguments[i]->column, msg);
            has_error = true;
        }

        /* Plain function references and non-capturing lambdas may be passed as
         * function-typed arguments (the ABI is a bare code pointer). A
         * *capturing* closure value cannot yet: the AST backend does not
         * marshal the closure handle (code ptr + captured environment) across a
         * call boundary, so the callee would invoke a bare pointer and crash.
         * Reject it here with a clear message rather than miscompiling.
         * (Direct calls of capturing closures, and binding/reassigning them,
         * are supported — see the mutable-capture work.) */
        if (expected_type == TYPE_FUNC &&
            expr_is_stack_closure_value(analyzer, call->arguments[i])) {
            report_semantic_error(call->arguments[i]->line, call->arguments[i]->column,
                "Passing a capturing closure value as a function argument is not "
                "supported yet (pass a plain function or a non-capturing lambda)");
            has_error = true;
        }
    }

    if (has_error) {
        expr->data_type = TYPE_ERROR;
    } else {
        expr->data_type = signature->return_type->base;
        if (signature->return_type->type_name &&
            (expr->data_type == TYPE_CLASS || expr->data_type == TYPE_GENERIC)) {
            expr->class_name = strdup(signature->return_type->type_name);
        }
        /* NOTE: a `-> lambda(...) -> R` return signature is deliberately NOT
         * propagated onto the call result here. The AST backend cannot yet
         * round-trip a function *value* out of a function and call it, so
         * allowing the call through semantically would compile to a crash.
         * Calling the result of such a call therefore still hits the generic
         * "callable expression" arity path (a pre-existing limitation), which
         * is safe. Only lambda-typed *parameters* are fully supported. */
    }
}

static void analyze_this_expr(SemanticAnalyzer* analyzer, Expr* expr) {
    if (!analyzer->current_class) {
        report_semantic_error(expr->line, expr->column,
            "'this' can only be used inside a class method");
        expr->data_type = TYPE_ERROR;
        return;
    }

    // Check if we're in a static method
    if (analyzer->current_method && analyzer->current_method->is_static) {
        report_semantic_error(expr->line, expr->column,
            "'this' cannot be used in static methods");
        expr->data_type = TYPE_ERROR;
        return;
    }

    // Set the class name in the this expression
    expr->as.this_expr.class_name = strdup(analyzer->current_class->name);
    expr->data_type = TYPE_CLASS;
}

static void analyze_super_expr(SemanticAnalyzer* analyzer, Expr* expr) {
    SuperExpr* super_expr = &expr->as.super_expr;
    
    // Check if Super is used inside a class
    if (!analyzer->current_class) {
        report_semantic_error(expr->line, expr->column,
            "'Super' can only be used inside a class method");
        expr->data_type = TYPE_ERROR;
        return;
    }
    
    // Check if we're in a static method
    if (analyzer->current_method && analyzer->current_method->is_static) {
        report_semantic_error(expr->line, expr->column,
            "'Super' cannot be used in static methods");
        expr->data_type = TYPE_ERROR;
        return;
    }
    
    // Check if current class has a parent
    if (!analyzer->current_class->parent_class || 
        strlen(analyzer->current_class->parent_class) == 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Class '%s' has no parent class",
                analyzer->current_class->name);
        report_semantic_error(expr->line, expr->column, msg);
        expr->data_type = TYPE_ERROR;
        return;
    }
    
    // Look up parent class
    ClassSymbol* parent_class = lookup_class(analyzer->symbols,
                                            analyzer->current_class->parent_class);
    if (!parent_class) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Parent class '%s' not found",
                analyzer->current_class->parent_class);
        report_semantic_error(expr->line, expr->column, msg);
        expr->data_type = TYPE_ERROR;
        return;
    }
    
    // Resolve member in parent class
    if (super_expr->is_method_call) {
        // Look for method in parent class
        MethodSymbol* method = find_method(parent_class, super_expr->member_name);
        if (!method) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Method '%s' not found in parent class '%s'",
                    super_expr->member_name, parent_class->name);
            report_semantic_error(expr->line, expr->column, msg);
            expr->data_type = TYPE_ERROR;
            return;
        }
        
        // Type check arguments
        if (super_expr->arg_count != method->param_count) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Method '%s' expects %d arguments, got %d",
                    super_expr->member_name, method->param_count, super_expr->arg_count);
            report_semantic_error(expr->line, expr->column, msg);
            expr->data_type = TYPE_ERROR;
            return;
        }
        
        // Analyze arguments
        for (int i = 0; i < super_expr->arg_count; i++) {
            analyze_expr(analyzer, super_expr->arguments[i]);
        }
        
        expr->data_type = method->return_type;
        expr->class_name = method->return_class_name ? strdup(method->return_class_name) : NULL;
    } else {
        // Look for field in parent class
        FieldSymbol* field = find_field(parent_class, super_expr->member_name);
        if (!field) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Field '%s' not found in parent class '%s'",
                    super_expr->member_name, parent_class->name);
            report_semantic_error(expr->line, expr->column, msg);
            expr->data_type = TYPE_ERROR;
            return;
        }
        
        expr->data_type = field->type;
        expr->class_name = field->class_name ? strdup(field->class_name) : NULL;
    }
}

/* Score one candidate constructor against an already-analyzed argument list.
 *   -1  : not a match (wrong arity, or an incompatible argument type)
 *    0  : matches only via numeric/implicit conversions
 *  >0  : number of arguments whose type is an exact match (higher == better)
 * The exact-match count is the tiebreak between same-arity overloads; because
 * types_compatible() treats every numeric type as interchangeable, that is the
 * only signal available to disambiguate e.g. new Foo(1) between func Foo(x:int)
 * and func Foo(x:f64). */
static int score_constructor(MethodSymbol* ctor, Expr** args, int arg_count) {
    if (ctor->param_count != arg_count) return -1;
    int exact = 0;
    for (int i = 0; i < arg_count; i++) {
        DataType at = args[i]->data_type;
        DataType pt = ctor->param_types[i];
        if (at == TYPE_ERROR) { exact++; continue; }  /* don't let a bad arg veto */
        if (!types_compatible(at, pt)) return -1;
        if (at == pt) exact++;
    }
    return exact;
}

static void analyze_new_expr(SemanticAnalyzer* analyzer, Expr* expr) {
    NewExpr* new_expr = &expr->as.new_expr;

    // Look up the class
    ClassSymbol* class_sym = lookup_class(analyzer->symbols, new_expr->class_name);
    if (!class_sym) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Undefined class '%s'", new_expr->class_name);
        report_semantic_error(expr->line, expr->column, msg);
        expr->data_type = TYPE_ERROR;
        return;
    }

    // Check if class is abstract
    if (class_sym->is_abstract) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Cannot instantiate abstract class '%s'", new_expr->class_name);
        report_semantic_error(expr->line, expr->column, msg);
        expr->data_type = TYPE_ERROR;
        return;
    }

    /* Analyze arguments first — overload resolution needs their types. */
    for (int i = 0; i < new_expr->arg_count; i++) {
        analyze_expr(analyzer, new_expr->arguments[i]);
    }

    /* Collect this class's constructor overloads, in declaration order.
     * Constructors are matched only on the instantiated class itself, not
     * inherited from parents. Both spellings — the class-name form and the
     * legacy `func new` form — count as constructors. */
    int ctor_positions[16];
    int ctor_n = 0;
    for (int i = 0; i < class_sym->method_count && ctor_n < 16; i++) {
        if (class_sym->methods[i].is_constructor) {
            ctor_positions[ctor_n++] = i;
        }
    }

    if (ctor_n == 0) {
        if (new_expr->arg_count == 0) {
            new_expr->ctor_index = -1;
            expr->data_type = TYPE_CLASS;
            if (expr->class_name) free(expr->class_name);
            expr->class_name = strdup(class_sym->name);
            return;
        }
        char msg[256];
        snprintf(msg, sizeof(msg), "Class '%s' has no constructor", new_expr->class_name);
        report_semantic_error(expr->line, expr->column, msg);
        expr->data_type = TYPE_ERROR;
        return;
    }

    /* Score every candidate; keep the best, and track ties at the best score. */
    int best_score = -1;
    int best_pos = -1;      /* method_count index */
    int best_ctor_ord = -1; /* ordinal among constructors (0-based) */
    int best_ties = 0;
    for (int c = 0; c < ctor_n; c++) {
        MethodSymbol* cand = &class_sym->methods[ctor_positions[c]];
        int s = score_constructor(cand, new_expr->arguments, new_expr->arg_count);
        if (s < 0) continue;
        if (s > best_score) {
            best_score = s;
            best_pos = ctor_positions[c];
            best_ctor_ord = c;
            best_ties = 1;
        } else if (s == best_score) {
            best_ties++;
        }
    }

    if (best_pos < 0) {
        /* No overload has a compatible signature. */
        char msg[320];
        if (ctor_n == 1) {
            MethodSymbol* only = &class_sym->methods[ctor_positions[0]];
            if (only->param_count != new_expr->arg_count) {
                snprintf(msg, sizeof(msg),
                        "Constructor expects %d argument(s), got %d",
                        only->param_count, new_expr->arg_count);
            } else {
                snprintf(msg, sizeof(msg),
                        "No matching constructor for '%s': argument types do not match",
                        new_expr->class_name);
            }
        } else {
            snprintf(msg, sizeof(msg),
                    "No matching constructor for '%s' with %d argument(s)",
                    new_expr->class_name, new_expr->arg_count);
        }
        report_semantic_error(expr->line, expr->column, msg);
        expr->data_type = TYPE_ERROR;
        return;
    }

    if (best_ties > 1) {
        char msg[320];
        snprintf(msg, sizeof(msg),
                "Ambiguous constructor call for '%s' with %d argument(s): "
                "%d overloads match equally well",
                new_expr->class_name, new_expr->arg_count, best_ties);
        report_semantic_error(expr->line, expr->column, msg);
        expr->data_type = TYPE_ERROR;
        return;
    }

    MethodSymbol* constructor = &class_sym->methods[best_pos];

    /* Record the selected overload for codegen. -1 when the class has a single
     * constructor so codegen keeps emitting the legacy `Class_new` label. */
    new_expr->ctor_index = (ctor_n > 1) ? best_ctor_ord : -1;

    /* Narrowing / lossy-conversion diagnostics on the chosen signature. */
    for (int i = 0; i < new_expr->arg_count; i++) {
        DataType at = new_expr->arguments[i]->data_type;
        if (at != TYPE_ERROR &&
            !types_compatible(at, constructor->param_types[i])) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                    "Type mismatch in argument %d (expected %s, got %s)",
                    i + 1,
                    type_to_string(constructor->param_types[i]),
                    type_to_string(at));
            report_type_error(expr->line, expr->column, msg);
        }
    }

    expr->data_type = TYPE_CLASS;
    /* May be re-visited by the loop-body fixpoint — free any prior name. */
    if (expr->class_name) free(expr->class_name);
    expr->class_name = strdup(class_sym->name);
}

static void analyze_index_expr(SemanticAnalyzer* analyzer, Expr* expr) {
    IndexExpr* index_expr = &expr->as.index;

    // Analyze array expression
    analyze_expr(analyzer, index_expr->array);

    // Analyze index expression
    analyze_expr(analyzer, index_expr->index);

    // Index must be an integer
    if (index_expr->index->data_type != TYPE_INT && index_expr->index->data_type != TYPE_ERROR) {
        report_type_error(expr->line, expr->column,
                        "Array index must be an integer");
        expr->data_type = TYPE_ERROR;
        return;
    }

    // For now, we'll assume arrays contain Int (simplified)
    // In a full implementation, we'd track the element type
    expr->data_type = TYPE_INT;
}

static void analyze_array_literal_expr(SemanticAnalyzer* analyzer, Expr* expr) {
    ArrayLiteralExpr* array_literal = &expr->as.array_literal;
    DataType element_type = TYPE_I32;  // Default element type
    for (int i = 0; i < array_literal->element_count; i++) {
        analyze_expr(analyzer, array_literal->elements[i]);
        if (i == 0) {
            element_type = array_literal->elements[i]->data_type;
        } else if (array_literal->elements[i]->data_type != TYPE_ERROR &&
                   element_type != TYPE_ERROR &&
                   !types_compatible(array_literal->elements[i]->data_type, element_type)) {
            report_type_error(array_literal->elements[i]->line, array_literal->elements[i]->column,
                              "Array elements must have compatible types");
        }
    }
    expr->data_type = TYPE_CLASS;
    expr->class_name = strdup("Array");
}

static void analyze_member_access_expr(SemanticAnalyzer* analyzer, Expr* expr) {
    MemberAccessExpr* member = &expr->as.member;
    ClassSymbol* class_sym = NULL;
    
    if (!member->object) {
        expr->data_type = TYPE_ERROR;
        return;
    }

    /* Check for static access BEFORE analyzing the object
       This prevents error messages about undefined variables when the "variable" is actually a class name */

    // Check for static access BEFORE analyzing the object
    // This prevents error messages about undefined variables when the "variable" is actually a class name
    if (member->object->type == EXPR_VARIABLE) {
        VariableExpr* var = &member->object->as.variable;
        ClassSymbol* potential_class = lookup_class(analyzer->symbols, var->name);

        if (potential_class) {
            // This is static member access: ClassName.staticMember
            // Convert to static access and analyze it
            expr->type = EXPR_STATIC_ACCESS;
            expr->as.static_access.class_name = strdup(var->name);
            expr->as.static_access.member_name = member->member_name;
            expr->as.static_access.is_method_call = member->is_method_call;
            expr->as.static_access.arguments = member->arguments;
            expr->as.static_access.arg_count = member->arg_count;

            // Analyze as static access
            analyze_static_access_expr(analyzer, expr);
            return;
        }
    }

    // Analyze the object expression
    analyze_expr(analyzer, member->object);

    if (member->object->data_type == TYPE_ERROR) {
        expr->data_type = TYPE_ERROR;
        return;
    }

    // Object must be a class type
    if (member->object->data_type != TYPE_CLASS) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Cannot access member of non-class type %s",
                type_to_string(member->object->data_type));
        report_type_error(expr->line, expr->column, msg);
        expr->data_type = TYPE_ERROR;
        return;
    }

    // Get the class symbol - for 'this', use current class
    if (member->object->type == EXPR_THIS) {
        class_sym = analyzer->current_class;
    } else if (member->object->type == EXPR_MEMBER_ACCESS) {
        // Nested member access (e.g., this.field.method() or obj.field.method())
        // The object expression should already have its class_name set from a previous analyze
        if (member->object->class_name) {
            class_sym = lookup_class(analyzer->symbols, member->object->class_name);
        }

        if (!class_sym) {
            report_semantic_error(expr->line, expr->column,
                "Cannot determine class type for member access");
            expr->data_type = TYPE_ERROR;
            return;
        }
    } else if (member->object->type == EXPR_VARIABLE) {
        // Variable access - already analyzed above, just look up the symbol
        VariableExpr* var = &member->object->as.variable;
        Symbol* var_symbol = lookup_symbol(analyzer->symbols, var->name);

        if (!var_symbol) {
            char msg[256];
            snprintf(msg, sizeof(msg), "'%s' is not a variable", var->name);
            report_semantic_error(expr->line, expr->column, msg);
            expr->data_type = TYPE_ERROR;
            return;
        }

        if (var_symbol->type != TYPE_CLASS) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Cannot access member '%s' on non-class type", member->member_name);
            report_semantic_error(expr->line, expr->column, msg);
            expr->data_type = TYPE_ERROR;
            return;
        }

        // Get the class info from the symbol
        class_sym = var_symbol->class_info;

        if (!class_sym) {
            // Class info not set - this can happen if the variable wasn't initialized with New
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Cannot determine class type for variable '%s'", var->name);
            report_semantic_error(expr->line, expr->column, msg);
            expr->data_type = TYPE_ERROR;
            return;
        }
    }

    if (!class_sym) {
        report_semantic_error(expr->line, expr->column,
            "Cannot determine class type for member access");
        expr->data_type = TYPE_ERROR;
        return;
    }

    // Check if it's a method call
    if (member->is_method_call) {
        // Look up method
        MethodSymbol* method = find_method(class_sym, member->member_name);
        if (!method) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Method '%s' not found in class '%s'",
                    member->member_name, class_sym->name);
            report_semantic_error(expr->line, expr->column, msg);
            expr->data_type = TYPE_ERROR;
            return;
        }

        // Check access permissions
        if (!can_access_member(analyzer, class_sym, method->access, expr->line, expr->column, member->member_name)) {
            expr->data_type = TYPE_ERROR;
            return;
        }

        // Validate argument count
        if (member->arg_count != method->param_count) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                    "Method '%s' expects %d arguments but got %d",
                    member->member_name, method->param_count, member->arg_count);
            report_semantic_error(expr->line, expr->column, msg);
            expr->data_type = TYPE_ERROR;
            return;
        }

        // Analyze arguments and check types
        for (int i = 0; i < member->arg_count; i++) {
            analyze_expr(analyzer, member->arguments[i]);
            if (!types_compatible(member->arguments[i]->data_type, method->param_types[i])) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                        "Argument %d type mismatch (expected %s, got %s)",
                        i + 1,
                        type_to_string(method->param_types[i]),
                        type_to_string(member->arguments[i]->data_type));
                report_type_error(member->arguments[i]->line, member->arguments[i]->column, msg);
                expr->data_type = TYPE_ERROR;
                return;
            }
        }

        expr->data_type = method->return_type;

        // If method returns a class type, we need to track which class
        // For now, we'll infer it from method name patterns or use the return type
        // This is a simplification - ideally methods would store return class names
        if (method->return_type == TYPE_CLASS) {
            // Check if the method name suggests it returns the same class (like getTask)
            // Or use the class_sym name as a heuristic
            // For now, just copy the class name (this will need refinement)
            expr->class_name = strdup(class_sym->name);
        }
    } else {
        // Field access
        FieldSymbol* field = find_field(class_sym, member->member_name);
        if (!field) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Field '%s' not found in class '%s'",
                    member->member_name, class_sym->name);
            report_semantic_error(expr->line, expr->column, msg);
            expr->data_type = TYPE_ERROR;
            return;
        }

        // Check access permissions
        if (!can_access_member(analyzer, class_sym, field->access, expr->line, expr->column, member->member_name)) {
            expr->data_type = TYPE_ERROR;
            return;
        }

        expr->data_type = field->type;
        // If field is a class type, propagate the class name
        if (field->type == TYPE_CLASS && field->class_name) {
            expr->class_name = strdup(field->class_name);
        }
    }
}

static void analyze_static_access_expr(SemanticAnalyzer* analyzer, Expr* expr) {
    StaticAccessExpr* static_access = &expr->as.static_access;

    // Look up the class
    ClassSymbol* class_sym = lookup_class(analyzer->symbols, static_access->class_name);
    if (!class_sym) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Unknown class '%s'", static_access->class_name);
        report_semantic_error(expr->line, expr->column, msg);
        expr->data_type = TYPE_ERROR;
        return;
    }

    // Check if it's a method call
    if (static_access->is_method_call) {
        // Look up static method
        MethodSymbol* method = find_method(class_sym, static_access->member_name);
        if (!method) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Method '%s' not found in class '%s'",
                    static_access->member_name, static_access->class_name);
            report_semantic_error(expr->line, expr->column, msg);
            expr->data_type = TYPE_ERROR;
            return;
        }

        // Verify it's a static method
        if (!method->is_static) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Cannot call instance method '%s' on class '%s' without an instance",
                    static_access->member_name, static_access->class_name);
            report_semantic_error(expr->line, expr->column, msg);
            expr->data_type = TYPE_ERROR;
            return;
        }

        // Check access permissions
        if (!can_access_member(analyzer, class_sym, method->access, expr->line, expr->column, static_access->member_name)) {
            expr->data_type = TYPE_ERROR;
            return;
        }

        // Validate argument count
        if (static_access->arg_count != method->param_count) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                    "Method '%s' expects %d arguments but got %d",
                    static_access->member_name, method->param_count, static_access->arg_count);
            report_semantic_error(expr->line, expr->column, msg);
            expr->data_type = TYPE_ERROR;
            return;
        }

        // Analyze arguments
        for (int i = 0; i < static_access->arg_count; i++) {
            analyze_expr(analyzer, static_access->arguments[i]);
        }

        expr->data_type = method->return_type;
    } else {
        // Static field access
        FieldSymbol* field = find_field(class_sym, static_access->member_name);
        if (!field) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Field '%s' not found in class '%s'",
                    static_access->member_name, static_access->class_name);
            report_semantic_error(expr->line, expr->column, msg);
            expr->data_type = TYPE_ERROR;
            return;
        }

        // Verify it's a static field
        if (!field->is_static) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Cannot access instance field '%s' on class '%s' without an instance",
                    static_access->member_name, static_access->class_name);
            report_semantic_error(expr->line, expr->column, msg);
            expr->data_type = TYPE_ERROR;
            return;
        }

        // Check access permissions
        if (!can_access_member(analyzer, class_sym, field->access, expr->line, expr->column, static_access->member_name)) {
            expr->data_type = TYPE_ERROR;
            return;
        }

        expr->data_type = field->type;
        if (field->type == TYPE_CLASS && field->class_name) {
            expr->class_name = strdup(field->class_name);
        }
    }
}

static void analyze_lambda_expr(SemanticAnalyzer* analyzer, Expr* expr) {
    LambdaExpr* lambda = &expr->as.lambda;
    ClosureMeta* closure_meta = analyze_closure(lambda, analyzer->symbols, analyzer->symbols);

    DataType prev_return_type = analyzer->current_function_return_type;
    DataType* prev_lambda_return_type = analyzer->current_lambda_return_type;
    bool prev_in_function = analyzer->in_function;

    if (closure_meta) {
        if (lambda->captured_vars) {
            for (int i = 0; i < lambda->capture_count; i++) {
                free(lambda->captured_vars[i]);
            }
            free(lambda->captured_vars);
            lambda->captured_vars = NULL;
        }
        if (lambda->captured_types) {
            free(lambda->captured_types);
            lambda->captured_types = NULL;
        }
        if (lambda->captured_is_mutable) {
            free(lambda->captured_is_mutable);
            lambda->captured_is_mutable = NULL;
        }

        lambda->capture_count = closure_meta->environment
            ? closure_meta->environment->count
            : 0;
        lambda->has_mutable_capture = false;
        if (lambda->capture_count > 0) {
            lambda->captured_vars = ALLOCATE(char*, lambda->capture_count);
            lambda->captured_types = ALLOCATE(DataType, lambda->capture_count);
            lambda->captured_is_mutable = ALLOCATE(bool, lambda->capture_count);
            for (int i = 0; i < lambda->capture_count; i++) {
                lambda->captured_vars[i] = strdup(
                    closure_meta->environment->variables[i].var_name);
                lambda->captured_types[i] = closure_meta->environment->variables[i].type;
                lambda->captured_is_mutable[i] =
                    closure_meta->environment->variables[i].is_mutable;
                if (closure_meta->environment->variables[i].is_mutable) {
                    lambda->has_mutable_capture = true;
                }
            }
        }
        lambda->closure_id = closure_meta->lambda_id;
        free_closure_meta(closure_meta);
    }

    analyzer->current_function_return_type = lambda->return_type;
    analyzer->current_lambda_return_type = &lambda->return_type;
    analyzer->in_function = true;

    enter_scope(analyzer->symbols, &analyzer->scope_depth);

    for (int i = 0; i < lambda->param_count; i++) {
        if (!lambda->parameters[i].name) continue;
        if (!add_symbol(analyzer->symbols, lambda->parameters[i].name,
                        SYMBOL_PARAMETER, lambda->parameters[i].type,
                        analyzer->scope_depth)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Duplicate lambda parameter '%s'",
                     lambda->parameters[i].name);
            report_semantic_error(expr->line, expr->column, msg);
        }
    }

    if (lambda->is_expression) {
        analyze_expr(analyzer, lambda->expr_body);
        if (lambda->return_type == TYPE_ERROR &&
            lambda->expr_body &&
            lambda->expr_body->data_type != TYPE_ERROR) {
            lambda->return_type = lambda->expr_body->data_type;
        }
    } else if (lambda->block_body) {
        analyze_stmt(analyzer, lambda->block_body);
    }

    exit_scope(analyzer->symbols, &analyzer->scope_depth);

    analyzer->current_function_return_type = prev_return_type;
    analyzer->current_lambda_return_type = prev_lambda_return_type;
    analyzer->in_function = prev_in_function;

    if (expr->type_info) {
        free_type_info(expr->type_info);
    }
    expr->type_info = build_function_type_info_from_params(lambda->parameters,
                                                           lambda->param_count,
                                                           lambda->return_type);
    expr->data_type = TYPE_FUNC;
}

static FieldSymbol* find_field_with_owner(ClassSymbol* class_sym, const char* name,
                                          ClassSymbol** owner_out) {
    if (!class_sym || !name) return NULL;

    for (int i = 0; i < class_sym->field_count; i++) {
        if (strcmp(class_sym->fields[i].name, name) == 0) {
            if (owner_out) *owner_out = class_sym;
            return &class_sym->fields[i];
        }
    }

    if (class_sym->parent) {
        return find_field_with_owner(class_sym->parent, name, owner_out);
    }

    return NULL;
}

static ClassSymbol* resolve_member_target_class(SemanticAnalyzer* analyzer, Expr* object) {
    if (!object) return NULL;

    if (object->type == EXPR_THIS) {
        return analyzer->current_class;
    }

    if (object->type == EXPR_MEMBER_ACCESS) {
        if (object->class_name) {
            return lookup_class(analyzer->symbols, object->class_name);
        }
        return NULL;
    }

    if (object->type == EXPR_VARIABLE) {
        VariableExpr* var = &object->as.variable;
        Symbol* symbol = lookup_symbol(analyzer->symbols, var->name);
        if (symbol && symbol->type == TYPE_CLASS) {
            return symbol->class_info;
        }
    }

    return NULL;
}

static bool is_constructor_field_initialization(SemanticAnalyzer* analyzer, Expr* target,
                                                ClassSymbol* owner_class) {
    if (!analyzer->current_method || !analyzer->current_method->is_constructor ||
        !analyzer->current_class || !owner_class) {
        return false;
    }

    if (target->type != EXPR_MEMBER_ACCESS) {
        return false;
    }

    MemberAccessExpr* member = &target->as.member;
    if (!member->object || member->object->type != EXPR_THIS) {
        return false;
    }

    return analyzer->current_class == owner_class ||
           is_subclass_of(analyzer->current_class, owner_class);
}

static void validate_assignment_target_mutability(SemanticAnalyzer* analyzer, Stmt* stmt) {
    AssignmentStmt* assign = &stmt->as.assignment;
    FieldSymbol* field = NULL;
    ClassSymbol* owner_class = NULL;

    if (assign->target->type == EXPR_MEMBER_ACCESS) {
        MemberAccessExpr* member = &assign->target->as.member;
        ClassSymbol* class_sym = resolve_member_target_class(analyzer, member->object);
        field = find_field_with_owner(class_sym, member->member_name, &owner_class);
    } else if (assign->target->type == EXPR_STATIC_ACCESS) {
        StaticAccessExpr* static_access = &assign->target->as.static_access;
        ClassSymbol* class_sym = lookup_class(analyzer->symbols, static_access->class_name);
        field = find_field_with_owner(class_sym, static_access->member_name, &owner_class);
    }

    if (!field) {
        return;
    }

    if (field->is_const && !is_constructor_field_initialization(analyzer, assign->target, owner_class)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Cannot assign to const field '%s'", field->name);
        report_semantic_error(stmt->line, stmt->column, msg);
        return;
    }

    if (!field->is_mutable &&
        !is_constructor_field_initialization(analyzer, assign->target, owner_class)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Cannot assign to immutable field '%s' outside its constructor",
                 field->name);
        report_semantic_error(stmt->line, stmt->column, msg);
    }
}

static void analyze_expr(SemanticAnalyzer* analyzer, Expr* expr) {
    if (!expr) {
        return;
    }

    switch (expr->type) {
        case EXPR_BINARY:
            analyze_binary_expr(analyzer, expr);
            break;
        case EXPR_UNARY:
            analyze_unary_expr(analyzer, expr);
            break;
        case EXPR_LITERAL:
            // Type already set during parsing
            break;
        case EXPR_VARIABLE:
            analyze_variable_expr(analyzer, expr);
            break;
        case EXPR_CALL:
            analyze_call_expr(analyzer, expr);
            break;
        case EXPR_MEMBER_ACCESS:
            analyze_member_access_expr(analyzer, expr);
            break;
        case EXPR_STATIC_ACCESS:
            analyze_static_access_expr(analyzer, expr);
            break;
        case EXPR_THIS:
            analyze_this_expr(analyzer, expr);
            break;
        case EXPR_SUPER:
            analyze_super_expr(analyzer, expr);
            break;
        case EXPR_NEW:
            analyze_new_expr(analyzer, expr);
            break;
        case EXPR_INDEX:
            analyze_index_expr(analyzer, expr);
            break;
        case EXPR_LAMBDA:
            analyze_lambda_expr(analyzer, expr);
            break;
        case EXPR_GENERIC_INST:
            // Generic instantiation is handled during parsing/monomorphization
            // Expression type is already set
            break;
        case EXPR_AWAIT:
            analyze_await_expr(analyzer, expr);
            break;
        case EXPR_ARRAY_LITERAL:
            analyze_array_literal_expr(analyzer, expr);
            break;
    }
}

/* Retained for future gates; the mutable-capture let-binding gate that used
 * this was removed when by-reference captures landed. */
static void register_error_declaration(SemanticAnalyzer* analyzer,
                                       DeclarationStmt* decl,
                                       Stmt* stmt) __attribute__((unused));
static void register_error_declaration(SemanticAnalyzer* analyzer,
                                       DeclarationStmt* decl,
                                       Stmt* stmt) {
    if (!decl || !decl->name) {
        return;
    }

    if (!add_symbol(analyzer->symbols, decl->name, SYMBOL_VARIABLE,
                    TYPE_ERROR, analyzer->scope_depth)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Variable '%s' already declared in this scope", decl->name);
        report_semantic_error(stmt->line, stmt->column, msg);
        return;
    }

    Symbol* symbol = lookup_symbol(analyzer->symbols, decl->name);
    if (symbol) {
        symbol->is_initialized = true;
        symbol->is_mutable = decl->is_mutable;
        symbol->is_const = decl->is_const;
    }
}

static void analyze_await_expr(SemanticAnalyzer* analyzer, Expr* expr) {
    if (!analyzer->in_async_function) {
        report_semantic_error(expr->line, expr->column, "'await' expression outside of async function");
        expr->data_type = TYPE_ERROR;
        return;
    }

    analyze_expr(analyzer, expr->as.await_expr.expression);
    
    // Check if the expression returns a Future
    // For now, we'll be lenient and allow awaiting anything, 
    // but ideally we check if expression->data_type == TYPE_FUTURE.
    
    // If it's a future, the result of await is the element type of the future.
    if (expr->as.await_expr.expression->data_type == TYPE_FUTURE) {
        if (expr->as.await_expr.expression->type_info && expr->as.await_expr.expression->type_info->element_type) {
            expr->data_type = expr->as.await_expr.expression->type_info->element_type->base;
            expr->type_info = expr->as.await_expr.expression->type_info->element_type; // Copy type info for nested types
        } else {
            expr->data_type = TYPE_VOID; // Default for Future<Void>
        }
    } else {
        // If not a future, just pass through (auto-wrap synchronous values?)
        // Many languages allow 'await 5' -> 5.
        expr->data_type = expr->as.await_expr.expression->data_type;
        expr->type_info = expr->as.await_expr.expression->type_info;
    }
}

static void analyze_declaration_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    DeclarationStmt* decl = &stmt->as.declaration;

    if (!decl->name) {
        report_semantic_error(stmt->line, stmt->column,
            "Invalid variable declaration");
        return;
    }

    // Analyze initializer first
    if (decl->initializer) {
        analyze_expr(analyzer, decl->initializer);

        // If this is a move expression, mark the source variable as moved
        if (is_move_expr(decl->initializer)) {
            mark_moved(&g_ownership_ctx, decl->initializer->as.variable.name, stmt->line);
        }

        /* A capturing lambda with mutable captures may be bound to a let/mut:
         * mutated captures are bound by reference in codegen so writes inside
         * the lambda reach the original variable. UNCHECKED SAFETY (v1): the
         * borrow checker does not verify that the captured variables outlive
         * the binding or that no conflicting aliases exist. Returning such a
         * closure from a function is still rejected (analyze_return_stmt). */

        // Type inference: if type is TYPE_ERROR (from := syntax), infer from initializer
        if (decl->type == TYPE_ERROR && decl->initializer->data_type != TYPE_ERROR) {
            decl->type = decl->initializer->data_type;
            // Also copy class name for class types
            if (decl->type == TYPE_CLASS && decl->initializer->class_name) {
                decl->class_name = strdup(decl->initializer->class_name);
            }
        }

        if (decl->initializer->type_info &&
            decl->type == decl->initializer->data_type) {
            if (decl->type_info) {
                free_type_info(decl->type_info);
            }
            decl->type_info = clone_type_info(decl->initializer->type_info);
        }

        if (decl->initializer->data_type != TYPE_ERROR &&
            decl->type != TYPE_ERROR &&
            !types_compatible(decl->initializer->data_type, decl->type)) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Type mismatch in variable initialization (expected %s, got %s)",
                     type_to_string(decl->type),
                     type_to_string(decl->initializer->data_type));
            report_type_error(stmt->line, stmt->column, msg);
        }
    }

    // Error if using := without initializer
    if (decl->type == TYPE_ERROR && !decl->initializer) {
        report_semantic_error(stmt->line, stmt->column,
            "Type inference (:=) requires an initializer expression");
        return;
    }

    // Add to symbol table
    if (!add_symbol(analyzer->symbols, decl->name, SYMBOL_VARIABLE,
                    decl->type, analyzer->scope_depth)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Variable '%s' already declared in this scope", decl->name);
        report_semantic_error(stmt->line, stmt->column, msg);
        return;
    }

    // Mark as initialized if there's an initializer
    // For class types, also set the class_info
    if (decl->initializer && decl->initializer->data_type != TYPE_ERROR) {
        Symbol* symbol = lookup_symbol(analyzer->symbols, decl->name);
        if (symbol) {
            symbol->is_initialized = true;
            symbol->is_mutable = decl->is_mutable;
            symbol->is_const = decl->is_const;

            if (decl->type == TYPE_FUNC &&
                ((decl->initializer && decl->initializer->type_info) || decl->type_info)) {
                TypeInfo* fn_info = decl->type_info ? decl->type_info : decl->initializer->type_info;
                symbol->param_count = fn_info->param_count;
                if (symbol->param_types) {
                    free(symbol->param_types);
                    symbol->param_types = NULL;
                }
                if (fn_info->param_count > 0) {
                    symbol->param_types = ALLOCATE(DataType, fn_info->param_count);
                    for (int i = 0; i < fn_info->param_count; i++) {
                        symbol->param_types[i] = fn_info->param_types[i]
                            ? fn_info->param_types[i]->base
                            : TYPE_ERROR;
                    }
                }
                symbol->return_type = fn_info->return_type
                    ? fn_info->return_type->base
                    : TYPE_VOID;

                if (decl->initializer && decl->initializer->type == EXPR_LAMBDA) {
                    set_symbol_closure_info(symbol, &decl->initializer->as.lambda);
                } else if (decl->initializer &&
                           decl->initializer->type == EXPR_VARIABLE &&
                           decl->initializer->as.variable.name) {
                    Symbol* init_symbol = lookup_symbol(analyzer->symbols,
                                                       decl->initializer->as.variable.name);
                    copy_symbol_closure_info(symbol, init_symbol);
                } else {
                    clear_symbol_closure_info(symbol);
                }
            }

            /* ── Linear Type System: register String / StringView ────────
             * This is the single point at which the AST→MIR boundary
             * captures the parent ↔ view relationship.  We register:
             *   - every owning String binding with the drop planner so the
             *     surviving-view check has something to anchor against;
             *   - every StringView binding with both the ownership checker
             *     (linear-consume tracking) and the drop planner (parent
             *     dependency edge).                                         */
            if (linear_view_decl_is_string(decl)) {
                drop_planner_register(&g_drop_ctx, decl->name,
                                       /*stack_offset*/ 0, DROP_DTOR,
                                       /*dtor_name*/ "string_release",
                                       /*is_param*/ false);
            } else if (linear_view_decl_is_view(decl)) {
                const char* parent =
                    linear_view_infer_parent(analyzer, decl->initializer);
                register_linear_view(&g_ownership_ctx, decl->name,
                                     parent, stmt->line);
                drop_planner_register_linear_view(&g_drop_ctx, decl->name,
                                                  parent, stmt->line);
                /* Also copy into the function-level log so escape promotion
                 * can still see this view after the declaring scope exits
                 * and the Symbol (with its ownership_data) is freed. */
                linear_view_log_add(&g_linear_view_log, decl->name,
                                    parent, stmt->line);
            }

            // If this is a class type, set class_info from various sources
            if (decl->type == TYPE_CLASS) {
                ClassSymbol* class_sym = NULL;

                // Try to get class from declaration type annotation
                if (decl->class_name) {
                    class_sym = lookup_class(analyzer->symbols, decl->class_name);
                }
                // Or from initializer expression class_name
                else if (decl->initializer->class_name) {
                    class_sym = lookup_class(analyzer->symbols, decl->initializer->class_name);
                }
                // Or if initializer is a New expression
                else if (decl->initializer->type == EXPR_NEW) {
                    NewExpr* new_expr = &decl->initializer->as.new_expr;
                    class_sym = lookup_class(analyzer->symbols, new_expr->class_name);
                }

                if (class_sym) {
                    symbol->class_info = class_sym;
                }
            }
        }
    } else {
        Symbol* symbol = lookup_symbol(analyzer->symbols, decl->name);
        if (symbol) {
            symbol->is_mutable = decl->is_mutable;
            symbol->is_const = decl->is_const;
        }
    }
}

static void analyze_assignment_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    AssignmentStmt* assign = &stmt->as.assignment;

    if (!assign->target) {
        report_semantic_error(stmt->line, stmt->column,
            "Invalid assignment");
        return;
    }

    if (!assign->value) {
        report_semantic_error(stmt->line, stmt->column,
            "Missing assignment value");
        return;
    }

    // Analyze the target expression (l-value)
    analyze_expr(analyzer, assign->target);

    // Analyze the value expression (r-value)
    analyze_expr(analyzer, assign->value);

    // If this is a move expression, mark the source variable as moved
    if (is_move_expr(assign->value)) {
        mark_moved(&g_ownership_ctx, assign->value->as.variable.name, stmt->line);
    }

    // Type checking
    if (assign->target->data_type != TYPE_ERROR &&
        assign->value->data_type != TYPE_ERROR &&
        !types_compatible(assign->value->data_type, assign->target->data_type)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Type mismatch in assignment (expected %s, got %s)",
                 type_to_string(assign->target->data_type),
                 type_to_string(assign->value->data_type));
        report_type_error(stmt->line, stmt->column, msg);
    }

    // Mark variable as initialized if the target is a simple variable
    if (assign->target->type == EXPR_VARIABLE) {
        Symbol* symbol = lookup_symbol(analyzer->symbols, assign->target->as.variable.name);
        if (symbol) {
            if (symbol->is_const) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Cannot assign to constant '%s'",
                         assign->target->as.variable.name);
                report_semantic_error(stmt->line, stmt->column, msg);
                return;
            }
            if (!symbol->is_mutable) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Cannot assign to immutable binding '%s'",
                         assign->target->as.variable.name);
                report_semantic_error(stmt->line, stmt->column, msg);
                return;
            }
            symbol->is_initialized = true;

            if (symbol->type == TYPE_FUNC) {
                if (assign->value->type == EXPR_LAMBDA) {
                    LambdaExpr* lambda = &assign->value->as.lambda;
                    /* Reassigning a func-typed binding to a capturing lambda
                     * with mutable captures is supported (by-reference capture
                     * codegen). UNCHECKED SAFETY (v1): no alias/lifetime
                     * verification for the captured variables. */
                    set_symbol_closure_info(symbol, lambda);
                } else if (assign->value->type == EXPR_VARIABLE &&
                           assign->value->as.variable.name) {
                    Symbol* value_symbol = lookup_symbol(analyzer->symbols,
                                                         assign->value->as.variable.name);
                    copy_symbol_closure_info(symbol, value_symbol);
                } else {
                    clear_symbol_closure_info(symbol);
                }
            }
        }
    }

    validate_assignment_target_mutability(analyzer, stmt);
}

static void analyze_print_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    PrintStmt* print = &stmt->as.print;
    
    if (!print->expression) {
        report_semantic_error(stmt->line, stmt->column,
            "Print statement requires an expression");
        return;
    }
    
    analyze_expr(analyzer, print->expression);
}

/* Conservative "does control flow definitely leave this statement without
 * falling through to the following statement?" — used so a branch that always
 * returns/throws/breaks/continues does not dilute a move-state merge at a join
 * point it cannot actually reach. Recurses through blocks (last stmt), if/else
 * (both sides), and match (all arms) so nested divergence is detected. */
static bool branch_diverges(Stmt* s) {
    if (!s) return false;
    switch (s->type) {
        case STMT_RETURN:
        case STMT_THROW:
        case STMT_BREAK:
        case STMT_CONTINUE:
            return true;
        case STMT_BLOCK: {
            BlockStmt* b = &s->as.block;
            for (int i = 0; i < b->stmt_count; i++) {
                if (b->statements[i] && branch_diverges(b->statements[i])) return true;
            }
            return false;
        }
        case STMT_IF: {
            IfStmt* f = &s->as.if_stmt;
            /* Diverges only if BOTH sides diverge (and there is an else). */
            return f->else_branch &&
                   branch_diverges(f->then_branch) &&
                   branch_diverges(f->else_branch);
        }
        case STMT_MATCH: {
            MatchStmt* m = &s->as.match_stmt;
            if (m->arm_count == 0) return false;
            for (int i = 0; i < m->arm_count; i++) {
                if (!branch_diverges(m->arms[i].body)) return false;
            }
            return true;
        }
        default:
            return false;
    }
}

/* True if `s` contains a `move x` expression anywhere in its subtree that
 * could move an outer variable across a loop back-edge. Cheap AST pre-scan so
 * loops with no moves skip the extra fixpoint pass entirely. */
static bool expr_has_move(Expr* e);
static bool stmt_has_move(Stmt* s);

static bool expr_has_move(Expr* e) {
    if (!e) return false;
    if (e->type == EXPR_VARIABLE && e->as.variable.is_move) return true;
    switch (e->type) {
        case EXPR_BINARY:
            return expr_has_move(e->as.binary.left) || expr_has_move(e->as.binary.right);
        case EXPR_UNARY:  return expr_has_move(e->as.unary.operand);
        case EXPR_CALL:
            if (expr_has_move(e->as.call.callee)) return true;
            for (int i = 0; i < e->as.call.arg_count; i++)
                if (expr_has_move(e->as.call.arguments[i])) return true;
            return false;
        case EXPR_MEMBER_ACCESS:
            if (expr_has_move(e->as.member.object)) return true;
            for (int i = 0; i < e->as.member.arg_count; i++)
                if (expr_has_move(e->as.member.arguments[i])) return true;
            return false;
        case EXPR_INDEX:
            return expr_has_move(e->as.index.array) || expr_has_move(e->as.index.index);
        case EXPR_AWAIT: return expr_has_move(e->as.await_expr.expression);
        case EXPR_ARRAY_LITERAL:
            for (int i = 0; i < e->as.array_literal.element_count; i++)
                if (expr_has_move(e->as.array_literal.elements[i])) return true;
            return false;
        default: return false;
    }
}

static bool stmt_has_move(Stmt* s) {
    if (!s) return false;
    switch (s->type) {
        case STMT_DECLARATION:
        case STMT_CONST_DECL:   return expr_has_move(s->as.declaration.initializer);
        case STMT_ASSIGNMENT:   return expr_has_move(s->as.assignment.target) ||
                                       expr_has_move(s->as.assignment.value);
        case STMT_PRINT:        return expr_has_move(s->as.print.expression);
        case STMT_EXPR:         return expr_has_move(s->as.expr_stmt.expression);
        case STMT_RETURN:       return expr_has_move(s->as.return_stmt.value);
        case STMT_THROW:        return expr_has_move(s->as.throw_stmt.value);
        case STMT_IF:
            return expr_has_move(s->as.if_stmt.condition) ||
                   stmt_has_move(s->as.if_stmt.then_branch) ||
                   stmt_has_move(s->as.if_stmt.else_branch);
        case STMT_WHILE:
            return expr_has_move(s->as.while_stmt.condition) ||
                   stmt_has_move(s->as.while_stmt.body);
        case STMT_FOR:
            return expr_has_move(s->as.for_stmt.initializer) ||
                   expr_has_move(s->as.for_stmt.condition) ||
                   stmt_has_move(s->as.for_stmt.increment) ||
                   stmt_has_move(s->as.for_stmt.body);
        case STMT_FOR_IN:
            return expr_has_move(s->as.for_in_stmt.iterable) ||
                   stmt_has_move(s->as.for_in_stmt.body);
        case STMT_BLOCK:
            for (int i = 0; i < s->as.block.stmt_count; i++)
                if (stmt_has_move(s->as.block.statements[i])) return true;
            return false;
        case STMT_MATCH:
            if (expr_has_move(s->as.match_stmt.subject)) return true;
            for (int i = 0; i < s->as.match_stmt.arm_count; i++)
                if (stmt_has_move(s->as.match_stmt.arms[i].body)) return true;
            return false;
        default: return false;
    }
}

/* Analyse a loop body with a fixpoint so cross-iteration moves are caught.
 * The move-set lattice is monotone (a variable never becomes un-moved within
 * the analysis), so one priming pass captures every body-carried move into the
 * loop-head state; a second (final) pass from that widened head then reports a
 * cross-iteration reuse exactly once, alongside any genuine single-iteration
 * error. Loops whose body contains no `move` skip straight to a single
 * ordinary pass.
 *
 * The loop may execute zero times, so the exit state is union(entry, body). */
static void analyze_loop_body_fixpoint(SemanticAnalyzer* analyzer, Stmt* body) {
    if (!body) return;

    bool prev_suppress = g_ownership_ctx.suppress_diagnostics;

    if (!stmt_has_move(body)) {
        analyze_stmt(analyzer, body);
        return;
    }

    OwnStateSnapshot entry;
    own_state_snapshot(&g_ownership_ctx, &entry);

    /* Priming pass — silent; widen the loop head with body-carried moves. */
    g_ownership_ctx.suppress_diagnostics = true;
    analyze_stmt(analyzer, body);
    g_ownership_ctx.suppress_diagnostics = prev_suppress;

    OwnStateSnapshot after_prime;
    own_state_snapshot(&g_ownership_ctx, &after_prime);

    OwnStateSnapshot head;
    own_state_copy(&head, &entry);
    own_state_merge_union(&head, &after_prime);
    own_state_free(&after_prime);

    /* Final pass — diagnostics enabled, from the widened head state. */
    own_state_restore(&g_ownership_ctx, &head);
    analyze_stmt(analyzer, body);

    OwnStateSnapshot after_final;
    own_state_snapshot(&g_ownership_ctx, &after_final);

    /* Exit state: union(entry, post-body) — the body may not run at all. */
    OwnStateSnapshot exit_s;
    own_state_copy(&exit_s, &entry);
    own_state_merge_union(&exit_s, &after_final);
    own_state_restore(&g_ownership_ctx, &exit_s);

    own_state_free(&after_final);
    own_state_free(&exit_s);
    own_state_free(&head);
    own_state_free(&entry);
}

static void analyze_if_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    IfStmt* if_stmt = &stmt->as.if_stmt;

    if (!if_stmt->condition) {
        report_semantic_error(stmt->line, stmt->column,
            "Missing if condition");
        return;
    }

    analyze_expr(analyzer, if_stmt->condition);

    if (if_stmt->condition->data_type != TYPE_ERROR &&
        if_stmt->condition->data_type != TYPE_BOOL) {
        report_type_error(if_stmt->condition->line, if_stmt->condition->column,
            "If condition must be boolean");
    }

    /* Path-sensitive move analysis: analyse each branch from a common
     * pre-branch snapshot, then merge so a move on one path does not leak
     * onto the other or past the join. */
    OwnStateSnapshot pre;
    own_state_snapshot(&g_ownership_ctx, &pre);

    OwnStateSnapshot then_s;
    if (if_stmt->then_branch) {
        analyze_stmt(analyzer, if_stmt->then_branch);
    }
    own_state_snapshot(&g_ownership_ctx, &then_s);

    own_state_restore(&g_ownership_ctx, &pre);
    OwnStateSnapshot else_s;
    if (if_stmt->else_branch) {
        analyze_stmt(analyzer, if_stmt->else_branch);
    }
    own_state_snapshot(&g_ownership_ctx, &else_s);

    bool then_div = branch_diverges(if_stmt->then_branch);
    bool else_div = if_stmt->else_branch ? branch_diverges(if_stmt->else_branch) : false;

    /* Merge: a variable is MOVED after the join only if every path that can
     * reach the join moved it. A branch that diverges cannot reach the join
     * and is excluded. With no else branch, the implicit fall-through path is
     * `pre` (nothing moved there). */
    if (then_div && else_div) {
        /* Join is unreachable dead code; leave state at the pre-branch value. */
        own_state_restore(&g_ownership_ctx, &pre);
    } else if (then_div) {
        own_state_restore(&g_ownership_ctx, &else_s);
    } else if (else_div) {
        own_state_restore(&g_ownership_ctx, &then_s);
    } else {
        OwnStateSnapshot merged;
        own_state_copy(&merged, &then_s);
        own_state_merge_intersect(&merged, &else_s);
        own_state_restore(&g_ownership_ctx, &merged);
        own_state_free(&merged);
    }

    own_state_free(&pre);
    own_state_free(&then_s);
    own_state_free(&else_s);
}

static void analyze_for_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    ForStmt* for_stmt = &stmt->as.for_stmt;
    
    enter_scope(analyzer->symbols, &analyzer->scope_depth);
    
    if (!for_stmt->variable) {
        report_semantic_error(stmt->line, stmt->column,
            "For loop requires a variable");
        exit_scope(analyzer->symbols, &analyzer->scope_depth);
        return;
    }
    
    // Add loop variable
    if (!add_symbol(analyzer->symbols, for_stmt->variable, SYMBOL_VARIABLE,
                    for_stmt->var_type, analyzer->scope_depth)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Loop variable '%s' already declared", for_stmt->variable);
        report_semantic_error(stmt->line, stmt->column, msg);
    }
    
    // Analyze initializer
    if (for_stmt->initializer) {
        analyze_expr(analyzer, for_stmt->initializer);
        if (for_stmt->initializer->data_type != TYPE_ERROR &&
            !types_compatible(for_stmt->initializer->data_type, for_stmt->var_type)) {
            report_type_error(stmt->line, stmt->column,
                "Loop initializer type mismatch");
        }
    }
    
    Symbol* loop_var = lookup_symbol(analyzer->symbols, for_stmt->variable);
    if (loop_var) {
        loop_var->is_initialized = true;
    }
    
    // Analyze condition
    if (for_stmt->condition) {
        analyze_expr(analyzer, for_stmt->condition);
        if (for_stmt->condition->data_type != TYPE_ERROR &&
            for_stmt->condition->data_type != TYPE_BOOL) {
            report_type_error(for_stmt->condition->line, for_stmt->condition->column,
                "Loop condition must be boolean");
        }
    }
    
    // Analyze increment
    if (for_stmt->increment) {
        analyze_stmt(analyzer, for_stmt->increment);
    }
    
    // Enter loop - increment loop_depth for break/continue validation
    analyzer->loop_depth++;
    
    // Analyze body (fixpoint for cross-iteration move detection)
    if (for_stmt->body) {
        analyze_loop_body_fixpoint(analyzer, for_stmt->body);
    }

    // Exit loop
    analyzer->loop_depth--;

    exit_scope(analyzer->symbols, &analyzer->scope_depth);
}

static void analyze_while_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    WhileStmt* while_stmt = &stmt->as.while_stmt;
    
    if (!while_stmt->condition) {
        report_semantic_error(stmt->line, stmt->column,
            "Missing while condition");
        return;
    }
    
    analyze_expr(analyzer, while_stmt->condition);
    
    if (while_stmt->condition->data_type != TYPE_ERROR &&
        while_stmt->condition->data_type != TYPE_BOOL) {
        report_type_error(while_stmt->condition->line, while_stmt->condition->column,
            "While condition must be boolean");
    }
    
    // Enter loop - increment loop_depth for break/continue validation
    analyzer->loop_depth++;
    
    if (while_stmt->body) {
        analyze_loop_body_fixpoint(analyzer, while_stmt->body);
    }

    // Exit loop
    analyzer->loop_depth--;
}

/* ─── for-in loop ─── */
static void analyze_for_in_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    ForInStmt* fi = &stmt->as.for_in_stmt;

    if (!fi->iterable) {
        report_semantic_error(stmt->line, stmt->column, "for-in: missing iterable expression");
        return;
    }
    analyze_expr(analyzer, fi->iterable);

    /* Declare loop variable in a new scope */
    int scope = 0;
    enter_scope(analyzer->symbols, &scope);
    add_symbol(analyzer->symbols, fi->var_name, SYMBOL_VARIABLE, TYPE_I64, scope);

    analyzer->loop_depth++;
    if (fi->body) analyze_loop_body_fixpoint(analyzer, fi->body);
    analyzer->loop_depth--;

    exit_scope(analyzer->symbols, &scope);
}

/* ─── match expression ─── */
static void analyze_match_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    MatchStmt* ms = &stmt->as.match_stmt;
    if (!ms->subject) {
        report_semantic_error(stmt->line, stmt->column, "match: missing subject expression");
        return;
    }
    analyze_expr(analyzer, ms->subject);

    /* Path-sensitive move merge across arms: each arm is analysed from the
     * same pre-match snapshot; a variable is MOVED after the match only if
     * every non-diverging arm moved it. We cannot cheaply prove exhaustiveness,
     * so the implicit "no arm matched" fall-through (pre-match state) is also
     * folded in — a move confined to one arm never escapes the match. */
    OwnStateSnapshot pre;
    own_state_snapshot(&g_ownership_ctx, &pre);

    OwnStateSnapshot acc;       /* accumulated intersection of reachable arms */
    own_state_copy(&acc, &pre); /* start from fall-through (nothing extra moved) */

    for (int i = 0; i < ms->arm_count; i++) {
        if (ms->arms[i].pattern)  /* NULL == wildcard _  */
            analyze_expr(analyzer, ms->arms[i].pattern);

        own_state_restore(&g_ownership_ctx, &pre);
        if (ms->arms[i].body)
            analyze_stmt(analyzer, ms->arms[i].body);

        if (!branch_diverges(ms->arms[i].body)) {
            OwnStateSnapshot arm_s;
            own_state_snapshot(&g_ownership_ctx, &arm_s);
            own_state_merge_intersect(&acc, &arm_s);
            own_state_free(&arm_s);
        }
    }

    own_state_restore(&g_ownership_ctx, &acc);
    own_state_free(&acc);
    own_state_free(&pre);
}

/* ─── throw statement ─── */
static void analyze_throw_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    ThrowStmt* th = &stmt->as.throw_stmt;
    if (!th->value) {
        report_semantic_error(stmt->line, stmt->column, "throw: missing value expression");
        return;
    }
    analyze_expr(analyzer, th->value);
    if (th->value->data_type == TYPE_VOID) {
        report_type_error(stmt->line, stmt->column, "thrown value cannot be void");
    }
}

/* ─── try / catch / finally ─── */
static void analyze_try_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    TryStmt* t = &stmt->as.try_stmt;

    if (t->try_body) analyze_stmt(analyzer, t->try_body);

    for (int i = 0; i < t->catch_count; i++) {
        int scope = 0;
        enter_scope(analyzer->symbols, &scope);
        /* Bind exception variable if named  */
        if (t->catches[i].exception_var) {
            add_symbol(analyzer->symbols,
                       t->catches[i].exception_var,
                       SYMBOL_VARIABLE, TYPE_I64, scope);
        }
        if (t->catches[i].body) analyze_stmt(analyzer, t->catches[i].body);
        exit_scope(analyzer->symbols, &scope);
    }

    if (t->finally_body) analyze_stmt(analyzer, t->finally_body);
}

/* trait declaration analysis lives further down, near analyze_impl_stmt, since
 * it shares the trait-table registration helper. */

static void analyze_function_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    FunctionStmt* func = &stmt->as.function;
    
    if (!func->name) {
        report_semantic_error(stmt->line, stmt->column,
            "Function requires a name");
        return;
    }
    
    // Collect parameter types
    DataType* param_types = NULL;
    if (func->param_count > 0) {
        param_types = ALLOCATE(DataType, func->param_count);
        for (int i = 0; i < func->param_count; i++) {
            param_types[i] = func->parameters[i].type;
        }
    }
    
    // Add function to symbol table (may already be pre-registered from pass 1)
    Symbol* existing = lookup_symbol(analyzer->symbols, func->name);
    if (existing && existing->kind == SYMBOL_FUNCTION) {
        // Already pre-registered — skip re-registration
        if (param_types) free(param_types);
    } else if (!add_function(analyzer->symbols, func->name, func->return_type,
                      param_types, func->param_count, analyzer->scope_depth)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Function '%s' already declared", func->name);
        report_semantic_error(stmt->line, stmt->column, msg);
        if (param_types) free(param_types);
        return;
    } else {
        if (param_types) free(param_types);
    }

    /* Carry a `-> lambda(...) -> R` return signature onto the symbol so a call
     * of this function yields a result whose arity can be checked. */
    {
        Symbol* fn_sym = lookup_symbol(analyzer->symbols, func->name);
        if (fn_sym && fn_sym->kind == SYMBOL_FUNCTION && func->return_type_info) {
            fn_sym->return_type_info = func->return_type_info;
        }
    }
    
    // Analyze function body in new scope
    enter_scope(analyzer->symbols, &analyzer->scope_depth);
    
    // Add parameters to symbol table
    for (int i = 0; i < func->param_count; i++) {
        if (!func->parameters[i].name) continue;

        if (!add_symbol(analyzer->symbols, func->parameters[i].name,
                        SYMBOL_PARAMETER, func->parameters[i].type,
                        analyzer->scope_depth)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Duplicate parameter name '%s'", func->parameters[i].name);
            report_semantic_error(stmt->line, stmt->column, msg);
        } else {
            Symbol* param = lookup_symbol(analyzer->symbols, func->parameters[i].name);
            if (param) {
                param->is_initialized = true;

                // Set class_info for class-type parameters
                if (func->parameters[i].type == TYPE_CLASS && func->parameters[i].class_name) {
                    ClassSymbol* class_sym = lookup_class(analyzer->symbols, func->parameters[i].class_name);
                    if (class_sym) {
                        param->class_info = class_sym;
                    }
                }

                // Retain a `lambda(...) -> R` parameter's inner signature so
                // calls through the parameter are arity-checked correctly.
                if (func->parameters[i].type == TYPE_FUNC &&
                    func->parameters[i].type_info) {
                    apply_fn_signature_to_symbol(param, func->parameters[i].type_info);
                }
            }
        }
    }
    
    // Set function context
    DataType prev_return_type = analyzer->current_function_return_type;
    bool prev_in_function = analyzer->in_function;
    bool prev_in_async = analyzer->in_async_function;

    analyzer->current_function_return_type = func->return_type;
    analyzer->in_function = true;
    analyzer->in_async_function = func->is_async;

    /* ── Memory model: enter function scope ── */
    escape_analyzer_reset(&g_escape_ctx);
    drop_planner_enter_scope(&g_drop_ctx);
    linear_view_log_reset(&g_linear_view_log);
    ownership_reset_function(&g_ownership_ctx);

    /* Register parameters for escape analysis and drop planning */
    for (int i = 0; i < func->param_count; i++) {
        if (!func->parameters[i].name) continue;
        escape_register_var(&g_escape_ctx, func->parameters[i].name, true);

        /* Parameters with class type get ARC drop tracking */
        DropKind dk = DROP_NONE;
        if (func->parameters[i].type == TYPE_CLASS ||
            func->parameters[i].type == TYPE_STRING ||
            func->parameters[i].type == TYPE_STRBUF) {
            dk = DROP_ARC;
        }
        drop_planner_register(&g_drop_ctx, func->parameters[i].name,
                               0, dk, NULL, true);
    }

    if (func->body) {
        analyze_stmt(analyzer, func->body);
    }

    /* ── Memory model: run escape analysis on function body ── */
    escape_analyze_function(&g_escape_ctx, stmt);

    /* Linear Type System: promote `StringView` entries in the escape table
     * from the function-scoped view log (the ownership-checker state is
     * already gone by now — per-scope Symbol entries were freed on scope
     * exit), then run the parent ↔ view fixpoint.  Any view that escapes
     * farther than its parent is reported here, at the function-declaration
     * line — the last sema hook before MIR lowering kicks in. */
    linear_view_promote_from_log(&g_escape_ctx, &g_linear_view_log);
    escape_propagate_view_links(&g_escape_ctx, stmt->line);

    /* Enforce parent-survives-view at the function boundary too: any owning
     * String declared in the function body that is about to be dropped must
     * not have a live outer-scope view.  (Block-scope exits already run
     * this check from `analyze_block_stmt`.) */
    drop_planner_check_string_drop_invariants(&g_drop_ctx, stmt->line);

    /* ── Memory model: exit function scope (drop planner) ── */
    validate_scope_end(&g_ownership_ctx, analyzer->scope_depth);
    drop_planner_exit_scope(&g_drop_ctx);
    
    // Restore context
    analyzer->current_function_return_type = prev_return_type;
    analyzer->in_function = prev_in_function;
    analyzer->in_async_function = prev_in_async;
    
    exit_scope(analyzer->symbols, &analyzer->scope_depth);
}

static void analyze_return_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    ReturnStmt* ret = &stmt->as.return_stmt;
    
    if (!analyzer->in_function) {
        report_semantic_error(stmt->line, stmt->column,
            "Return statement outside function");
        return;
    }
    
    if (ret->value) {
        analyze_expr(analyzer, ret->value);

        if (analyzer->current_lambda_return_type && *analyzer->current_lambda_return_type == TYPE_ERROR) {
            *analyzer->current_lambda_return_type = ret->value->data_type;
            analyzer->current_function_return_type = ret->value->data_type;
        }

        if (expr_is_stack_closure_value(analyzer, ret->value)) {
            report_semantic_error(stmt->line, stmt->column,
                "Returning capturing closure values is not implemented yet");
            return;
        }

        /* ── Memory model safety: prevent returning references to local stack-allocated variables ── */
        if (ret->value->type == EXPR_VARIABLE) {
            const char* var_name = ret->value->as.variable.name;
            Symbol* sym = lookup_symbol(analyzer->symbols, var_name);
            DataType ret_type = analyzer->current_function_return_type;
            
            if (sym && (ret_type == TYPE_REF || ret_type == TYPE_PTR || ret_type == TYPE_RAWPTR)) {
                bool is_stack = false;
                if (sym->type == TYPE_STRUCT || type_is_primitive(sym->type)) {
                    is_stack = true;
                } else if (escape_can_stack_alloc(&g_escape_ctx, var_name)) {
                    is_stack = true;
                }

                if (is_stack) {
                     char msg[256];
                     snprintf(msg, sizeof(msg), "Cannot return reference to local stack-allocated variable '%s'", var_name);
                     report_semantic_error(stmt->line, stmt->column, msg);
                }
            }
        }
        
        if (ret->value->data_type != TYPE_ERROR &&
            !types_compatible(ret->value->data_type, analyzer->current_function_return_type)) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Return type mismatch (expected %s, got %s)",
                     type_to_string(analyzer->current_function_return_type),
                     type_to_string(ret->value->data_type));
            report_type_error(stmt->line, stmt->column, msg);
        }
    } else {
        if (analyzer->current_lambda_return_type && *analyzer->current_lambda_return_type == TYPE_ERROR) {
            *analyzer->current_lambda_return_type = TYPE_VOID;
            analyzer->current_function_return_type = TYPE_VOID;
        }
        if (analyzer->current_function_return_type != TYPE_VOID) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Function must return a value of type %s",
                     type_to_string(analyzer->current_function_return_type));
            report_type_error(stmt->line, stmt->column, msg);
        }
    }
}

static void analyze_block_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    BlockStmt* block = &stmt->as.block;
    bool is_alloc_scope = block->is_alloc_scope;

    enter_scope(analyzer->symbols, &analyzer->scope_depth);
    drop_planner_enter_scope(&g_drop_ctx);
    if (is_alloc_scope) {
        analyzer->alloc_scope_depth++;
    }

    for (int i = 0; i < block->stmt_count; i++) {
        if (block->statements[i]) {
            analyze_stmt(analyzer, block->statements[i]);
        }
    }

    /* Validate ownership at scope exit and plan drops */
    validate_scope_end(&g_ownership_ctx, analyzer->scope_depth);
    if (is_alloc_scope) {
        analyzer->alloc_scope_depth--;
    }

    /* AST → MIR boundary: enforce the linear-view invariant before the
     * planner discards the entries of this scope.  Any owning String about
     * to be dropped that still has a live StringView in an outer scope is
     * reported here, with the diagnostic pinned at the closing brace. */
    drop_planner_check_string_drop_invariants(&g_drop_ctx, stmt->line);

    drop_planner_exit_scope(&g_drop_ctx);
    exit_scope(analyzer->symbols, &analyzer->scope_depth);
}

static void analyze_class_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    ClassStmt* class_stmt = &stmt->as.class_stmt;

    // Class was already registered in pre-pass; just look it up
    ClassSymbol* class_sym = lookup_class(analyzer->symbols, class_stmt->name);
    if (!class_sym) {
        // Not pre-registered — register now (shouldn't happen with two-pass, but be safe)
        if (!add_class(analyzer->symbols, class_stmt->name, class_stmt->parent_name, analyzer->scope_depth)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Class '%s' already declared", class_stmt->name);
            report_type_error(stmt->line, stmt->column, msg);
            return;
        }
        class_sym = lookup_class(analyzer->symbols, class_stmt->name);
        if (!class_sym) return;
    }

    // Analyze default values for fields
    for (int i = 0; i < class_stmt->field_count; i++) {
        FieldDecl* field = &class_stmt->fields[i];
        if (field->is_static && !field->is_mutable && !field->default_value) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Immutable static field '%s' must have an initializer",
                     field->name);
            report_semantic_error(stmt->line, stmt->column, msg);
        }

        if (field->default_value) {
            analyze_expr(analyzer, field->default_value);
            if (!types_compatible(field->default_value->data_type, field->type)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                        "Field '%s' default value type %s does not match declared type %s",
                        field->name,
                        type_to_string(field->default_value->data_type),
                        type_to_string(field->type));
                report_type_error(stmt->line, stmt->column, msg);
            }
        }
    }

    // Analyze method bodies (methods were already registered in pre-pass)
    for (int i = 0; i < class_stmt->method_count; i++) {
        MethodDecl* method = &class_stmt->methods[i];

        // Analyze method body with class context
        ClassSymbol* prev_class = analyzer->current_class;
        analyzer->current_class = class_sym;

        // Set current method for static validation. find_method() returns the
        // first symbol by name, which is wrong for an overloaded constructor
        // set — pick the MethodSymbol whose arity/param types match this AST
        // declaration so `current_method` reflects the body being analyzed.
        MethodSymbol* method_sym = NULL;
        for (int mi = 0; mi < class_sym->method_count; mi++) {
            MethodSymbol* cand = &class_sym->methods[mi];
            if (strcmp(cand->name, method->name) != 0) continue;
            if (cand->param_count != method->param_count) continue;
            bool same = true;
            for (int pj = 0; pj < cand->param_count; pj++) {
                if (cand->param_types[pj] != method->parameters[pj].type) { same = false; break; }
            }
            if (same) { method_sym = cand; break; }
        }
        if (!method_sym) method_sym = find_method(class_sym, method->name);
        MethodSymbol* prev_method = analyzer->current_method;
        analyzer->current_method = method_sym;

        bool prev_in_function = analyzer->in_function;
        DataType prev_return_type = analyzer->current_function_return_type;

        analyzer->in_function = true;
        analyzer->current_function_return_type = method->return_type;

        // Enter scope for method parameters
        enter_scope(analyzer->symbols, &analyzer->scope_depth);

        // Add parameters to scope
        for (int j = 0; j < method->param_count; j++) {
            if (!add_symbol(analyzer->symbols, method->parameters[j].name,
                           SYMBOL_PARAMETER, method->parameters[j].type, analyzer->scope_depth)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Duplicate parameter '%s' in method '%s'",
                        method->parameters[j].name, method->name);
                report_type_error(stmt->line, stmt->column, msg);
            } else {
                // Set class_info for class-type parameters
                Symbol* param = lookup_symbol(analyzer->symbols, method->parameters[j].name);
                if (param) {
                    param->is_initialized = true;

                    if (method->parameters[j].type == TYPE_CLASS && method->parameters[j].class_name) {
                        ClassSymbol* param_class = lookup_class(analyzer->symbols, method->parameters[j].class_name);
                        if (param_class) {
                            param->class_info = param_class;
                        }
                    }

                    if (method->parameters[j].type == TYPE_FUNC &&
                        method->parameters[j].type_info) {
                        apply_fn_signature_to_symbol(param, method->parameters[j].type_info);
                    }
                }
            }
        }

        // Analyze method body
        if (method->body) {
            analyze_stmt(analyzer, method->body);
        }

        exit_scope(analyzer->symbols, &analyzer->scope_depth);

        // Restore context
        analyzer->in_function = prev_in_function;
        analyzer->current_function_return_type = prev_return_type;
        analyzer->current_class = prev_class;
        analyzer->current_method = prev_method;
    }

    /* ----------------------------------------------------------------
     * Abstract method enforcement:
     * If this class is NOT abstract, every abstract method in the parent
     * hierarchy must be overridden by a concrete method in this class.
     * ---------------------------------------------------------------- */
    if (!class_sym->is_abstract && class_sym->parent) {
        ClassSymbol* ancestor = class_sym->parent;
        while (ancestor) {
            for (int i = 0; i < ancestor->method_count; i++) {
                MethodSymbol* am = &ancestor->methods[i];
                if (!am->is_abstract) continue;

                MethodSymbol* override = find_method(class_sym, am->name);
                if (!override || override->is_abstract) {
                    char msg[512];
                    snprintf(msg, sizeof(msg),
                        "Class '%s' must implement abstract method '%s' inherited from '%s'",
                        class_sym->name, am->name, ancestor->name);
                    report_type_error(stmt->line, stmt->column, msg);
                }
            }
            ancestor = ancestor->parent;
        }
    }
}

static void analyze_extern_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    ExternStmt* ext = &stmt->as.extern_stmt;

    if (!ext->name) {
        report_semantic_error(stmt->line, stmt->column, "Extern function requires a name");
        return;
    }

    // Collect parameter types
    DataType* param_types = NULL;
    if (ext->param_count > 0) {
        param_types = ALLOCATE(DataType, ext->param_count);
        for (int i = 0; i < ext->param_count; i++) {
            param_types[i] = ext->parameters[i].type;
        }
    }

    // Add function to symbol table
    if (!add_extern_function(analyzer->symbols, ext->name, ext->return_type,
                             param_types, ext->param_count, analyzer->scope_depth)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Function '%s' already declared", ext->name);
        report_semantic_error(stmt->line, stmt->column, msg);
    } else {
        // Mark as external function (we might need a flag in Symbol for this if we want to distinguish)
        // For now, just adding it to the symbol table is enough for it to be callable
    }

    if (param_types) free(param_types);
}

/* Pre-register a struct as a named type (pass 1), mirroring preregister_class
 * minus methods/inheritance/traits/generics. Structs share the ClassSymbol
 * namespace via add_class so the rest of the compiler can resolve a struct
 * name as a type (params, `new`, member access) through lookup_class. */
static void preregister_struct(SemanticAnalyzer* analyzer, Stmt* stmt) {
    StructStmt* struct_stmt = &stmt->as.struct_stmt;

    if (!add_class(analyzer->symbols, struct_stmt->name, NULL, analyzer->scope_depth)) {
        /* The name is already claimed by another type (class or struct) in
         * this scope. Report here — pass 1 processes declarations in source
         * order, so the first declaration wins and this one is the conflict. */
        char msg[256];
        snprintf(msg, sizeof(msg), "Struct '%s' already declared", struct_stmt->name);
        report_type_error(stmt->line, stmt->column, msg);
        return;
    }

    ClassSymbol* struct_sym = lookup_class(analyzer->symbols, struct_stmt->name);
    if (!struct_sym) return;

    for (int i = 0; i < struct_stmt->field_count; i++) {
        StructField* field = &struct_stmt->fields[i];
        add_field_to_class(struct_sym, field->name, field->type,
                           field->class_name, /*is_static*/ false, ACCESS_PUBLIC,
                           /*is_const*/ false, /*is_mutable*/ true);
    }
}

static void analyze_struct_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    StructStmt* struct_stmt = &stmt->as.struct_stmt;

    /* Struct name conflicts (with a class or another struct) are detected and
     * reported during pre-registration; bail out here so we don't validate
     * this struct's fields against the unrelated symbol that owns the name. */
    ClassSymbol* struct_sym = lookup_class(analyzer->symbols, struct_stmt->name);
    if (!struct_sym) {
        if (lookup_symbol(analyzer->symbols, struct_stmt->name)) {
            return; /* name taken by another symbol — already reported in pass 1 */
        }
        if (!add_class(analyzer->symbols, struct_stmt->name, NULL, analyzer->scope_depth)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Struct '%s' already declared", struct_stmt->name);
            report_type_error(stmt->line, stmt->column, msg);
            return;
        }
        struct_sym = lookup_class(analyzer->symbols, struct_stmt->name);
        if (!struct_sym) return;
    }

    for (int i = 0; i < struct_stmt->field_count; i++) {
        StructField* field = &struct_stmt->fields[i];

        /* Duplicate field name within this struct */
        for (int j = 0; j < i; j++) {
            if (strcmp(struct_stmt->fields[j].name, field->name) == 0) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Duplicate field '%s' in struct '%s'",
                         field->name, struct_stmt->name);
                report_semantic_error(stmt->line, stmt->column, msg);
                break;
            }
        }

        /* Field type validation: a named (class/struct) type must resolve */
        if (field->type == TYPE_CLASS && field->class_name) {
            if (!lookup_class(analyzer->symbols, field->class_name)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Undefined type '%s' for field '%s' in struct '%s'",
                         field->class_name, field->name, struct_stmt->name);
                report_semantic_error(stmt->line, stmt->column, msg);
            }
        }
    }
}

/* Pre-register an enum as a named type (pass 1), mirroring preregister_struct.
 * Enums share the ClassSymbol namespace via add_class so a name collision with
 * a class/struct/enum is caught here regardless of declaration order. Variants
 * (and any data-carrying payloads) are validated in analyze_enum_stmt. */
static void preregister_enum(SemanticAnalyzer* analyzer, Stmt* stmt) {
    EnumStmt* enum_stmt = &stmt->as.enum_stmt;

    if (!add_class(analyzer->symbols, enum_stmt->name, NULL, analyzer->scope_depth)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Enum '%s' already declared", enum_stmt->name);
        report_type_error(stmt->line, stmt->column, msg);
        return;
    }
}

static void analyze_enum_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    EnumStmt* enum_stmt = &stmt->as.enum_stmt;

    /* Name conflicts (with a class/struct/enum) are detected and reported
     * during pre-registration; if the symbol is missing, register it now and
     * report if that fails. */
    if (!lookup_class(analyzer->symbols, enum_stmt->name)) {
        if (lookup_symbol(analyzer->symbols, enum_stmt->name)) {
            return; /* name taken by another symbol — already reported in pass 1 */
        }
        if (!add_class(analyzer->symbols, enum_stmt->name, NULL, analyzer->scope_depth)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Enum '%s' already declared", enum_stmt->name);
            report_type_error(stmt->line, stmt->column, msg);
            return;
        }
    }

    for (int i = 0; i < enum_stmt->variant_count; i++) {
        EnumVariant* variant = &enum_stmt->variants[i];

        /* Duplicate variant name within this enum */
        for (int j = 0; j < i; j++) {
            if (strcmp(enum_stmt->variants[j].name, variant->name) == 0) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Duplicate variant '%s' in enum '%s'",
                         variant->name, enum_stmt->name);
                report_semantic_error(stmt->line, stmt->column, msg);
                break;
            }
        }

        /* Payload type validation: any named (class/struct/enum) payload type
         * must resolve. The parser stores the type name in payload_names[k]
         * for TYPE_CLASS payloads (NULL for primitives). */
        for (int k = 0; k < variant->payload_count; k++) {
            if (variant->payload_types[k] == TYPE_CLASS &&
                variant->payload_names && variant->payload_names[k]) {
                if (!lookup_class(analyzer->symbols, variant->payload_names[k])) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Undefined type '%s' in payload of variant '%s' in enum '%s'",
                             variant->payload_names[k], variant->name, enum_stmt->name);
                    report_semantic_error(stmt->line, stmt->column, msg);
                }
            }
        }
    }
}

/* Pre-register a union as a named type (pass 1), mirroring preregister_enum.
 * Unions share the ClassSymbol namespace via add_class so a name collision with
 * a class/struct/enum/union is caught here regardless of declaration order.
 * Member validation happens in analyze_union_stmt. */
static void preregister_union(SemanticAnalyzer* analyzer, Stmt* stmt) {
    UnionStmt* union_stmt = &stmt->as.union_stmt;

    if (!add_class(analyzer->symbols, union_stmt->name, NULL, analyzer->scope_depth)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Union '%s' already declared", union_stmt->name);
        report_type_error(stmt->line, stmt->column, msg);
        return;
    }
}

static void analyze_union_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    UnionStmt* union_stmt = &stmt->as.union_stmt;

    /* Name conflicts (with a class/struct/enum/union) are detected and reported
     * during pre-registration; if the symbol is missing, register it now and
     * report if that fails. */
    if (!lookup_class(analyzer->symbols, union_stmt->name)) {
        if (lookup_symbol(analyzer->symbols, union_stmt->name)) {
            return; /* name taken by another symbol — already reported in pass 1 */
        }
        if (!add_class(analyzer->symbols, union_stmt->name, NULL, analyzer->scope_depth)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Union '%s' already declared", union_stmt->name);
            report_type_error(stmt->line, stmt->column, msg);
            return;
        }
    }

    for (int i = 0; i < union_stmt->field_count; i++) {
        UnionField* field = &union_stmt->fields[i];

        /* Duplicate member name within this union */
        for (int j = 0; j < i; j++) {
            if (strcmp(union_stmt->fields[j].name, field->name) == 0) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Duplicate member '%s' in union '%s'",
                         field->name, union_stmt->name);
                report_semantic_error(stmt->line, stmt->column, msg);
                break;
            }
        }

        /* Member type validation: the parser resolves member types via
         * parse_type(), which does not retain a class name, so an unresolved
         * named type surfaces as TYPE_ERROR. */
        if (field->type == TYPE_ERROR) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Invalid type for member '%s' in union '%s'",
                     field->name, union_stmt->name);
            report_semantic_error(stmt->line, stmt->column, msg);
        }
    }
}

/* Register a trait's required-method signatures into the trait table. Shared
 * with analyze_trait_stmt so both pass-1 pre-registration and (fallback)
 * pass-2 analysis populate identically. Emits a diagnostic for duplicate
 * method names within the trait. */
static void register_trait_methods(SemanticAnalyzer* analyzer, Stmt* stmt,
                                   TraitSymbol* trait_sym) {
    (void)analyzer;
    TraitStmt* tr = &stmt->as.trait_stmt;

    for (int i = 0; i < tr->method_count; i++) {
        TraitMethodDecl* m = &tr->methods[i];

        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp(tr->methods[j].name, m->name) == 0) { dup = true; break; }
        }
        if (dup) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Duplicate method '%s' in trait '%s'", m->name, tr->name);
            report_semantic_error(stmt->line, stmt->column, msg);
            continue;
        }

        DataType* pt = NULL;
        if (m->param_count > 0) {
            pt = ALLOCATE(DataType, m->param_count);
            for (int k = 0; k < m->param_count; k++) pt[k] = m->parameters[k].type;
        }
        add_method_to_trait(trait_sym, m->name, m->return_type,
                            m->return_class_name, pt, m->param_count, m->has_default);
        if (pt) free(pt);
    }
}

/* Pre-register a trait as a named type (pass 1). Traits share the same type
 * namespace as class/struct/enum/union via the add_trait name-collision scan,
 * so a conflict with any of them is caught here regardless of declaration
 * order. */
static void preregister_trait(SemanticAnalyzer* analyzer, Stmt* stmt) {
    TraitStmt* tr = &stmt->as.trait_stmt;

    if (!add_trait(analyzer->symbols, tr->name, analyzer->scope_depth)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Trait '%s' already declared", tr->name);
        report_type_error(stmt->line, stmt->column, msg);
        return;
    }

    TraitSymbol* trait_sym = lookup_trait(analyzer->symbols, tr->name);
    if (!trait_sym) return;
    register_trait_methods(analyzer, stmt, trait_sym);
}

static void analyze_trait_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    TraitStmt* tr = &stmt->as.trait_stmt;

    /* Name conflicts are detected and reported during pre-registration. If the
     * trait is missing here it is either (a) a conflict already reported in
     * pass 1 — the name belongs to another symbol — in which case stay silent,
     * or (b) pass 1 did not run for this statement, in which case register it
     * now. */
    TraitSymbol* trait_sym = lookup_trait(analyzer->symbols, tr->name);
    if (!trait_sym) {
        if (lookup_symbol(analyzer->symbols, tr->name)) {
            return; /* name taken by another symbol — already reported */
        }
        if (!add_trait(analyzer->symbols, tr->name, analyzer->scope_depth)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Trait '%s' already declared", tr->name);
            report_type_error(stmt->line, stmt->column, msg);
            return;
        }
        trait_sym = lookup_trait(analyzer->symbols, tr->name);
        if (!trait_sym) return;
        register_trait_methods(analyzer, stmt, trait_sym);
    }

    for (int i = 0; i < tr->method_count; i++) {
        TraitMethodDecl* m = &tr->methods[i];

        /* Named return type must resolve (parameters carry no class name from
         * the parser, so only the return type is checkable here). */
        if (m->return_type == TYPE_CLASS && m->return_class_name) {
            if (!lookup_class(analyzer->symbols, m->return_class_name)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Undefined type '%s' in return of trait method '%s' in trait '%s'",
                         m->return_class_name, m->name, tr->name);
                report_semantic_error(stmt->line, stmt->column, msg);
            }
        }

        /* Type-check default bodies (skipped entirely before this pass). A
         * default body is a function body, so establish function context and a
         * fresh scope for it; there is no `self`-type context for traits, so
         * this is best-effort. */
        if (m->has_default && m->default_body) {
            bool prev_in_function = analyzer->in_function;
            DataType prev_return_type = analyzer->current_function_return_type;
            analyzer->in_function = true;
            analyzer->current_function_return_type = m->return_type;

            enter_scope(analyzer->symbols, &analyzer->scope_depth);
            for (int k = 0; k < m->param_count; k++) {
                if (m->parameters[k].name) {
                    add_symbol(analyzer->symbols, m->parameters[k].name,
                               SYMBOL_PARAMETER, m->parameters[k].type,
                               analyzer->scope_depth);
                }
            }
            analyze_stmt(analyzer, m->default_body);
            exit_scope(analyzer->symbols, &analyzer->scope_depth);

            analyzer->in_function = prev_in_function;
            analyzer->current_function_return_type = prev_return_type;
        }
    }
}

static void analyze_impl_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    ImplStmt* impl = &stmt->as.impl_stmt;

    /* Target type must exist (class/struct/enum/union all live in the class
     * namespace). */
    if (impl->target_name && !lookup_class(analyzer->symbols, impl->target_name)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "impl target type '%s' is not defined", impl->target_name);
        report_semantic_error(stmt->line, stmt->column, msg);
    }

    /* Trait-impl compliance checking. */
    if (impl->trait_name) {
        TraitSymbol* trait_sym = lookup_trait(analyzer->symbols, impl->trait_name);
        if (!trait_sym) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Unknown trait '%s' in impl for '%s'",
                     impl->trait_name, impl->target_name ? impl->target_name : "?");
            report_semantic_error(stmt->line, stmt->column, msg);
        } else {
            /* Completeness: every required method must be provided unless it
             * has a default implementation. */
            for (int i = 0; i < trait_sym->method_count; i++) {
                TraitMethodSymbol* req = &trait_sym->methods[i];
                if (req->has_default) continue;

                bool found = false;
                for (int j = 0; j < impl->method_count; j++) {
                    Stmt* ms = impl->methods[j];
                    if (ms && ms->type == STMT_FUNCTION &&
                        ms->as.function.name &&
                        strcmp(ms->as.function.name, req->name) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "impl of trait '%s' for '%s' is missing method '%s'",
                             impl->trait_name,
                             impl->target_name ? impl->target_name : "?", req->name);
                    report_type_error(stmt->line, stmt->column, msg);
                }
            }

            /* Signature compliance for each impl method that matches a trait
             * method by name. Extra methods not in the trait are allowed. */
            for (int j = 0; j < impl->method_count; j++) {
                Stmt* ms = impl->methods[j];
                if (!ms || ms->type != STMT_FUNCTION || !ms->as.function.name) continue;
                FunctionStmt* fn = &ms->as.function;

                TraitMethodSymbol* req = find_trait_method(trait_sym, fn->name);
                if (!req) continue;

                if (fn->param_count != req->param_count) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "method '%s' in impl of trait '%s' for '%s': expected %d parameter(s), found %d",
                             fn->name, impl->trait_name,
                             impl->target_name ? impl->target_name : "?",
                             req->param_count, fn->param_count);
                    report_type_error(stmt->line, stmt->column, msg);
                } else {
                    for (int k = 0; k < fn->param_count; k++) {
                        DataType have = fn->parameters[k].type;
                        DataType want = req->param_types ? req->param_types[k] : TYPE_VOID;
                        if (!types_compatible(have, want)) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "method '%s' in impl of trait '%s' for '%s': parameter %d expected %s, found %s",
                                     fn->name, impl->trait_name,
                                     impl->target_name ? impl->target_name : "?",
                                     k + 1, type_to_string(want), type_to_string(have));
                            report_type_error(stmt->line, stmt->column, msg);
                        }
                    }
                }

                if (!types_compatible(fn->return_type, req->return_type)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "method '%s' in impl of trait '%s' for '%s': return type expected %s, found %s",
                             fn->name, impl->trait_name,
                             impl->target_name ? impl->target_name : "?",
                             type_to_string(req->return_type),
                             type_to_string(fn->return_type));
                    report_type_error(stmt->line, stmt->column, msg);
                }
            }
        }
    }

    /* Analyze each method body (unchanged behaviour). */
    for (int i = 0; i < impl->method_count; i++) {
        if (impl->methods[i]) {
            analyze_stmt(analyzer, impl->methods[i]);
        }
    }
}

static void analyze_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    if (!stmt) return;

    switch (stmt->type) {
        case STMT_DECLARATION:
            analyze_declaration_stmt(analyzer, stmt);
            break;
        case STMT_ASSIGNMENT:
            analyze_assignment_stmt(analyzer, stmt);
            break;
        case STMT_PRINT:
            analyze_print_stmt(analyzer, stmt);
            break;
        case STMT_IF:
            analyze_if_stmt(analyzer, stmt);
            break;
        case STMT_FOR:
            analyze_for_stmt(analyzer, stmt);
            break;
        case STMT_WHILE:
            analyze_while_stmt(analyzer, stmt);
            break;
        case STMT_FUNCTION:
            analyze_function_stmt(analyzer, stmt);
            break;
        case STMT_RETURN:
            analyze_return_stmt(analyzer, stmt);
            break;
        case STMT_EXPR:
            if (stmt->as.expr_stmt.expression) {
                analyze_expr(analyzer, stmt->as.expr_stmt.expression);
            }
            break;
        case STMT_BLOCK:
            analyze_block_stmt(analyzer, stmt);
            break;
        case STMT_INCLUDE:
            // Include statements are processed during module resolution
            break;
        case STMT_BREAK:
        case STMT_CONTINUE:
            // Validate we're in a loop context (fixes TODO)
            if (analyzer->loop_depth == 0) {
                const char* stmt_name = (stmt->type == STMT_BREAK) ? "break" : "continue";
                char msg[256];
                snprintf(msg, sizeof(msg), "'%s' statement outside of loop", stmt_name);
                report_semantic_error(stmt->line, stmt->column, msg);
            }
            break;
        case STMT_CLASS:
            analyze_class_stmt(analyzer, stmt);
            break;
        case STMT_EXTERN:
            analyze_extern_stmt(analyzer, stmt);
            break;
        case STMT_STRUCT:
            analyze_struct_stmt(analyzer, stmt);
            break;
        case STMT_ENUM:
            analyze_enum_stmt(analyzer, stmt);
            break;
        case STMT_UNION:
            analyze_union_stmt(analyzer, stmt);
            break;
        case STMT_FOR_IN:
            analyze_for_in_stmt(analyzer, stmt);
            break;
        case STMT_MATCH:
            analyze_match_stmt(analyzer, stmt);
            break;
        case STMT_THROW:
            analyze_throw_stmt(analyzer, stmt);
            break;
        case STMT_TRY:
            analyze_try_stmt(analyzer, stmt);
            break;
        case STMT_TRAIT:
            analyze_trait_stmt(analyzer, stmt);
            break;
        case STMT_CONST_DECL:
            // Const declarations use the same layout as regular declarations
            analyze_declaration_stmt(analyzer, stmt);
            break;
        case STMT_IMPL:
            analyze_impl_stmt(analyzer, stmt);
            break;
    }
}

/* Pre-register a class declaration (fields, methods, constructor) without
   analyzing method bodies.  This allows forward references between modules. */
static void preregister_class(SemanticAnalyzer* analyzer, Stmt* stmt) {
    ClassStmt* class_stmt = &stmt->as.class_stmt;

    if (!add_class(analyzer->symbols, class_stmt->name, class_stmt->parent_name, analyzer->scope_depth)) {
        /* Name already claimed by another type (class/struct/enum) in this
         * scope. Pass 1 runs in source order, so the first declaration wins
         * and this one is the conflict. (A genuine class/class redeclaration
         * is also reported here.) */
        if (lookup_class(analyzer->symbols, class_stmt->name)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Class '%s' already declared", class_stmt->name);
            report_type_error(stmt->line, stmt->column, msg);
        }
        return;
    }

    ClassSymbol* class_sym = lookup_class(analyzer->symbols, class_stmt->name);
    if (!class_sym) return;

    /* Add fields */
    for (int i = 0; i < class_stmt->field_count; i++) {
        FieldDecl* field = &class_stmt->fields[i];
        add_field_to_class(class_sym, field->name, field->type,
                           field->class_name, field->is_static, field->access,
                           field->is_const, field->is_mutable);
    }

    /* Propagate abstract class flag from AST to symbol table */
    if (class_stmt->is_abstract) {
        class_sym->is_abstract = true;
    }

    /* Add methods (signatures only, no body analysis) */
    for (int i = 0; i < class_stmt->method_count; i++) {
        MethodDecl* method = &class_stmt->methods[i];

        DataType* param_types = NULL;
        if (method->param_count > 0) {
            param_types = ALLOCATE(DataType, method->param_count);
            for (int j = 0; j < method->param_count; j++) {
                param_types[j] = method->parameters[j].type;
            }
        }

        add_method_to_class(class_sym, method->name, method->return_type,
                            param_types, method->param_count,
                            method->is_constructor, method->is_static, method->access);

        /* Propagate abstract method flag */
        if (method->is_abstract) {
            MethodSymbol* msym = find_method(class_sym, method->name);
            if (msym) {
                msym->is_abstract = true;
            }
            class_sym->is_abstract = true;  /* containing class must also be abstract */
        }

        if (param_types) free(param_types);
    }
}

bool analyze_program(SemanticAnalyzer* analyzer, Stmt** statements, int count) {
    if (!analyzer || !statements) {
        return false;
    }

    /* Pass 1: Pre-register all declarations (extern functions, classes,
       top-level functions) so that forward references resolve correctly
       across imported modules. */
    for (int i = 0; i < count; i++) {
        if (!statements[i]) continue;
        switch (statements[i]->type) {
            case STMT_EXTERN:
                analyze_extern_stmt(analyzer, statements[i]);
                break;
            case STMT_CLASS:
                preregister_class(analyzer, statements[i]);
                break;
            case STMT_STRUCT:
                preregister_struct(analyzer, statements[i]);
                break;
            case STMT_ENUM:
                preregister_enum(analyzer, statements[i]);
                break;
            case STMT_UNION:
                preregister_union(analyzer, statements[i]);
                break;
            case STMT_TRAIT:
                preregister_trait(analyzer, statements[i]);
                break;
            case STMT_FUNCTION: {
                /* Pre-register function signature (without body analysis) */
                FunctionStmt* func = &statements[i]->as.function;
                DataType* param_types = NULL;
                if (func->param_count > 0) {
                    param_types = ALLOCATE(DataType, func->param_count);
                    for (int j = 0; j < func->param_count; j++) {
                        param_types[j] = func->parameters[j].type;
                    }
                }
                add_function(analyzer->symbols, func->name, func->return_type,
                             param_types, func->param_count, analyzer->scope_depth);
                if (param_types) free(param_types);
                break;
            }
            default:
                break;
        }
    }

    /* Pass 2: Full analysis of all statements (class bodies, function bodies,
       expressions, etc.).  Externs/classes already registered above will be
       skipped or handled gracefully by the individual analyzers. */
    for (int i = 0; i < count; i++) {
        if (statements[i]) {
            /* Skip externs — already fully handled in pass 1 */
            if (statements[i]->type == STMT_EXTERN) continue;
            analyze_stmt(analyzer, statements[i]);
        }
    }

    return !had_error;
}
