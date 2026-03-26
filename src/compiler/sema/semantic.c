#include "compiler/sema/semantic.h"
#include "compiler/sema/escape_analysis.h"
#include "compiler/sema/drop_planner.h"
#include "compiler/sema/ownership_check.h"
#include "compiler/middle/closure.h"
#include "support/error.h"

static void analyze_expr(SemanticAnalyzer* analyzer, Expr* expr);
static void analyze_stmt(SemanticAnalyzer* analyzer, Stmt* stmt);
static void analyze_static_access_expr(SemanticAnalyzer* analyzer, Expr* expr);
static void analyze_lambda_expr(SemanticAnalyzer* analyzer, Expr* expr);

/* ─── Global memory-model analysis contexts ─── */
static EscapeAnalyzer   g_escape_ctx;
static DropPlanner      g_drop_ctx;
static OwnershipChecker g_ownership_ctx;
static bool             g_memory_model_inited = false;

void init_semantic_analyzer(SemanticAnalyzer* analyzer) {
    analyzer->symbols = ALLOCATE(SymbolTable, 1);
    init_symbol_table(analyzer->symbols);
    analyzer->scope_depth = 0;
    analyzer->current_function_return_type = TYPE_VOID;
    analyzer->in_function = false;
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

static bool lambda_direct_call_supported(LambdaExpr* lambda) {
    return lambda && !lambda->has_mutable_capture;
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

    if (symbol->kind == SYMBOL_FUNCTION) {
        expr->data_type = TYPE_FUNC;
        if (expr->type_info) {
            free_type_info(expr->type_info);
        }
        expr->type_info = build_function_type_info(symbol->param_types,
                                                   symbol->param_count,
                                                   symbol->return_type);
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

    expr->data_type = symbol->type;

    if (symbol->type == TYPE_FUNC) {
        if (expr->type_info) {
            free_type_info(expr->type_info);
        }
        expr->type_info = build_function_type_info(symbol->param_types,
                                                   symbol->param_count,
                                                   symbol->return_type);
    }

    // If it's a class type, propagate the class name
    if (symbol->type == TYPE_CLASS && symbol->class_info) {
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
        LambdaExpr* lambda = &call->callee->as.lambda;
        if (!lambda_direct_call_supported(lambda)) {
            report_semantic_error(expr->line, expr->column,
                "Direct calls of lambdas with mutable captures are not implemented yet");
            expr->data_type = TYPE_ERROR;
            return;
        }
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

        if (expected_type == TYPE_FUNC &&
            expr_is_stack_closure_value(analyzer, call->arguments[i])) {
            report_semantic_error(call->arguments[i]->line, call->arguments[i]->column,
                "Passing capturing closure values as function arguments is not implemented yet");
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

static void analyze_new_expr(SemanticAnalyzer* analyzer, Expr* expr) {
    NewExpr* new_expr = &expr->as.new_expr;
    MethodSymbol* constructor = NULL;

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

    /* Prefer a constructor spelled with the class name, but keep legacy
     * func new(...) support for compatibility. Constructors are matched only
     * on the instantiated class itself, not inherited from parents. */
    for (int i = 0; i < class_sym->method_count; i++) {
        if (class_sym->methods[i].is_constructor &&
            strcmp(class_sym->methods[i].name, class_sym->name) == 0) {
            constructor = &class_sym->methods[i];
            break;
        }
    }
    if (!constructor) {
        for (int i = 0; i < class_sym->method_count; i++) {
            if (class_sym->methods[i].is_constructor) {
                constructor = &class_sym->methods[i];
                break;
            }
        }
    }

    if (!constructor) {
        if (new_expr->arg_count == 0) {
            expr->data_type = TYPE_CLASS;
            expr->class_name = strdup(class_sym->name);
            return;
        }

        {
            char msg[256];
            snprintf(msg, sizeof(msg), "Class '%s' has no constructor", new_expr->class_name);
            report_semantic_error(expr->line, expr->column, msg);
            expr->data_type = TYPE_ERROR;
            return;
        }
    }

    // Validate argument count
    if (new_expr->arg_count != constructor->param_count) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                "Constructor expects %d argument(s), got %d",
                constructor->param_count, new_expr->arg_count);
        report_semantic_error(expr->line, expr->column, msg);
        expr->data_type = TYPE_ERROR;
        return;
    }

    // Analyze and type-check arguments
    for (int i = 0; i < new_expr->arg_count; i++) {
        analyze_expr(analyzer, new_expr->arguments[i]);

        if (new_expr->arguments[i]->data_type != TYPE_ERROR &&
            !types_compatible(new_expr->arguments[i]->data_type, constructor->param_types[i])) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                    "Type mismatch in argument %d (expected %s, got %s)",
                    i + 1,
                    type_to_string(constructor->param_types[i]),
                    type_to_string(new_expr->arguments[i]->data_type));
            report_type_error(expr->line, expr->column, msg);
        }
    }

    expr->data_type = TYPE_CLASS;
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

static void analyze_member_access_expr(SemanticAnalyzer* analyzer, Expr* expr) {
    MemberAccessExpr* member = &expr->as.member;

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
    ClassSymbol* class_sym = NULL;
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

        lambda->capture_count = closure_meta->environment
            ? closure_meta->environment->count
            : 0;
        lambda->has_mutable_capture = false;
        if (lambda->capture_count > 0) {
            lambda->captured_vars = ALLOCATE(char*, lambda->capture_count);
            lambda->captured_types = ALLOCATE(DataType, lambda->capture_count);
            for (int i = 0; i < lambda->capture_count; i++) {
                lambda->captured_vars[i] = strdup(
                    closure_meta->environment->variables[i].var_name);
                lambda->captured_types[i] = closure_meta->environment->variables[i].type;
                if (closure_meta->environment->variables[i].is_mutable) {
                    lambda->has_mutable_capture = true;
                }
            }
        }
        lambda->closure_id = closure_meta->lambda_id;
        free_closure_meta(closure_meta);
    }

    analyzer->current_function_return_type = lambda->return_type;
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
    }
}

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

        if (decl->initializer->type == EXPR_LAMBDA) {
            LambdaExpr* lambda = &decl->initializer->as.lambda;
            if (lambda->capture_count > 0) {
                if (lambda->has_mutable_capture) {
                    report_semantic_error(stmt->line, stmt->column,
                        "Capturing lambda values with mutable captures are not implemented yet");
                    register_error_declaration(analyzer, decl, stmt);
                    return;
                }
            }
        }

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
                    if (lambda->capture_count > 0 && lambda->has_mutable_capture) {
                        report_semantic_error(stmt->line, stmt->column,
                            "Assigning capturing lambda values with mutable captures is not implemented yet");
                        return;
                    }
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
    
    if (if_stmt->then_branch) {
        analyze_stmt(analyzer, if_stmt->then_branch);
    }
    
    if (if_stmt->else_branch) {
        analyze_stmt(analyzer, if_stmt->else_branch);
    }
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
    
    // Analyze body
    if (for_stmt->body) {
        analyze_stmt(analyzer, for_stmt->body);
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
        analyze_stmt(analyzer, while_stmt->body);
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
    if (fi->body) analyze_stmt(analyzer, fi->body);
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

    for (int i = 0; i < ms->arm_count; i++) {
        if (ms->arms[i].pattern)  /* NULL == wildcard _  */
            analyze_expr(analyzer, ms->arms[i].pattern);
        if (ms->arms[i].body)
            analyze_stmt(analyzer, ms->arms[i].body);
    }
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

/* ─── trait declaration ─── */
static void analyze_trait_stmt(SemanticAnalyzer* analyzer, Stmt* stmt) {
    /* Traits are purely compile-time contracts.
     * We just validate that method signatures use known types. */
    TraitStmt* tr = &stmt->as.trait_stmt;
    (void)analyzer;
    (void)tr;
    /* TODO: register trait in a trait table for impl-check in semantic pass */
}

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
            }
        }
    }
    
    // Set function context
    DataType prev_return_type = analyzer->current_function_return_type;
    bool prev_in_function = analyzer->in_function;
    analyzer->current_function_return_type = func->return_type;
    analyzer->in_function = true;

    /* ── Memory model: enter function scope ── */
    escape_analyzer_reset(&g_escape_ctx);
    drop_planner_enter_scope(&g_drop_ctx);

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

    /* ── Memory model: exit function scope (drop planner) ── */
    drop_planner_exit_scope(&g_drop_ctx);
    
    // Restore context
    analyzer->current_function_return_type = prev_return_type;
    analyzer->in_function = prev_in_function;
    
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

        if (expr_is_stack_closure_value(analyzer, ret->value)) {
            report_semantic_error(stmt->line, stmt->column,
                "Returning capturing closure values is not implemented yet");
            return;
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

        // Set current method for static validation
        MethodSymbol* method_sym = find_method(class_sym, method->name);
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
            // Struct declarations: register as named type (similar to class)
            // For now, just validate field types are valid
            break;
        case STMT_ENUM:
            // Enum declarations: register variants
            break;
        case STMT_UNION:
            // Union declarations: validate field types
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
            // Impl blocks: analyze each method body
            for (int i = 0; i < stmt->as.impl_stmt.method_count; i++) {
                if (stmt->as.impl_stmt.methods[i]) {
                    analyze_stmt(analyzer, stmt->as.impl_stmt.methods[i]);
                }
            }
            break;
    }
}

/* Pre-register a class declaration (fields, methods, constructor) without
   analyzing method bodies.  This allows forward references between modules. */
static void preregister_class(SemanticAnalyzer* analyzer, Stmt* stmt) {
    ClassStmt* class_stmt = &stmt->as.class_stmt;

    if (!add_class(analyzer->symbols, class_stmt->name, class_stmt->parent_name, analyzer->scope_depth)) {
        /* Silently skip — will be reported in full analysis if truly a dup */
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
