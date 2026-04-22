/*
 * Casprix Compiler - Generic Monomorphization Implementation
 */

#include "compiler/middle/monomorphize.h"
#include "support/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void init_monomorphization(MonoContext* ctx) {
    ctx->instances = NULL;
    ctx->instance_count = 0;
    ctx->type_environment = malloc(sizeof(SymbolTable));
    init_symbol_table(ctx->type_environment);
}

// Helper: Deep copy type
static DataType copy_type(DataType type) {
    return type;  // For simple types, just copy
}

// Helper: Deep copy expression with type substitution
static Expr* copy_expr_with_substitution(Expr* expr, TypeParam* params, int param_count,
                                         DataType* args, int arg_count) {
    if (!expr) return NULL;
    (void)copy_type;  // Mark as used

    Expr* new_expr = malloc(sizeof(Expr));
    memcpy(new_expr, expr, sizeof(Expr));

    // Perform type substitution
    for (int i = 0; i < param_count && i < arg_count; i++) {
        if (expr->data_type == TYPE_GENERIC && expr->class_name &&
            strcmp(expr->class_name, params[i].name) == 0) {
            new_expr->data_type = args[i];
            new_expr->class_name = NULL;  // Clear generic param name
        }
    }

    // Recursively copy subexpressions
    switch (expr->type) {
        case EXPR_BINARY:
            new_expr->as.binary.left = copy_expr_with_substitution(expr->as.binary.left, params, param_count, args, arg_count);
            new_expr->as.binary.right = copy_expr_with_substitution(expr->as.binary.right, params, param_count, args, arg_count);
            break;

        case EXPR_UNARY:
            new_expr->as.unary.operand = copy_expr_with_substitution(expr->as.unary.operand, params, param_count, args, arg_count);
            break;

        case EXPR_CALL:
            new_expr->as.call.callee = copy_expr_with_substitution(
                expr->as.call.callee, params, param_count, args, arg_count);
            new_expr->as.call.name = expr->as.call.name ? strdup(expr->as.call.name) : NULL;
            new_expr->as.call.arguments = malloc(expr->as.call.arg_count * sizeof(Expr*));
            for (int i = 0; i < expr->as.call.arg_count; i++) {
                new_expr->as.call.arguments[i] = copy_expr_with_substitution(
                    expr->as.call.arguments[i], params, param_count, args, arg_count);
            }
            break;

        case EXPR_MEMBER_ACCESS:
            new_expr->as.member.object = copy_expr_with_substitution(expr->as.member.object, params, param_count, args, arg_count);
            if (expr->as.member.is_method_call && expr->as.member.arg_count > 0) {
                new_expr->as.member.arguments = malloc(expr->as.member.arg_count * sizeof(Expr*));
                for (int i = 0; i < expr->as.member.arg_count; i++) {
                    new_expr->as.member.arguments[i] = copy_expr_with_substitution(
                        expr->as.member.arguments[i], params, param_count, args, arg_count);
                }
            }
            break;

        case EXPR_INDEX:
            new_expr->as.index.array = copy_expr_with_substitution(expr->as.index.array, params, param_count, args, arg_count);
            new_expr->as.index.index = copy_expr_with_substitution(expr->as.index.index, params, param_count, args, arg_count);
            break;

        case EXPR_NEW:
            if (expr->as.new_expr.arg_count > 0) {
                new_expr->as.new_expr.arguments = malloc(expr->as.new_expr.arg_count * sizeof(Expr*));
                for (int i = 0; i < expr->as.new_expr.arg_count; i++) {
                    new_expr->as.new_expr.arguments[i] = copy_expr_with_substitution(
                        expr->as.new_expr.arguments[i], params, param_count, args, arg_count);
                }
            }
            break;

        default:
            break;
    }

    return new_expr;
}

// Helper: Deep copy statement with type substitution
static Stmt* copy_stmt_with_substitution(Stmt* stmt, TypeParam* params, int param_count,
                                         DataType* args, int arg_count) {
    if (!stmt) return NULL;

    Stmt* new_stmt = malloc(sizeof(Stmt));
    memcpy(new_stmt, stmt, sizeof(Stmt));

    switch (stmt->type) {
        case STMT_DECLARATION:
            // Substitute type in declaration
            for (int i = 0; i < param_count && i < arg_count; i++) {
                if (stmt->as.declaration.type == TYPE_GENERIC && stmt->as.declaration.class_name &&
                    strcmp(stmt->as.declaration.class_name, params[i].name) == 0) {
                    new_stmt->as.declaration.type = args[i];
                    new_stmt->as.declaration.class_name = NULL;
                }
            }
            new_stmt->as.declaration.initializer = copy_expr_with_substitution(
                stmt->as.declaration.initializer, params, param_count, args, arg_count);
            break;

        case STMT_EXPR:
            new_stmt->as.expr_stmt.expression = copy_expr_with_substitution(
                stmt->as.expr_stmt.expression, params, param_count, args, arg_count);
            break;

        case STMT_RETURN:
            new_stmt->as.return_stmt.value = copy_expr_with_substitution(
                stmt->as.return_stmt.value, params, param_count, args, arg_count);
            break;

        case STMT_IF:
            new_stmt->as.if_stmt.condition = copy_expr_with_substitution(
                stmt->as.if_stmt.condition, params, param_count, args, arg_count);
            new_stmt->as.if_stmt.then_branch = copy_stmt_with_substitution(
                stmt->as.if_stmt.then_branch, params, param_count, args, arg_count);
            new_stmt->as.if_stmt.else_branch = copy_stmt_with_substitution(
                stmt->as.if_stmt.else_branch, params, param_count, args, arg_count);
            break;

        case STMT_WHILE:
            new_stmt->as.while_stmt.condition = copy_expr_with_substitution(
                stmt->as.while_stmt.condition, params, param_count, args, arg_count);
            new_stmt->as.while_stmt.body = copy_stmt_with_substitution(
                stmt->as.while_stmt.body, params, param_count, args, arg_count);
            break;

        case STMT_FOR:
            new_stmt->as.for_stmt.initializer = copy_expr_with_substitution(
                stmt->as.for_stmt.initializer, params, param_count, args, arg_count);
            new_stmt->as.for_stmt.condition = copy_expr_with_substitution(
                stmt->as.for_stmt.condition, params, param_count, args, arg_count);
            new_stmt->as.for_stmt.increment = copy_stmt_with_substitution(
                stmt->as.for_stmt.increment, params, param_count, args, arg_count);
            new_stmt->as.for_stmt.body = copy_stmt_with_substitution(
                stmt->as.for_stmt.body, params, param_count, args, arg_count);
            break;

        case STMT_BLOCK:
            new_stmt->as.block.statements = malloc(stmt->as.block.stmt_count * sizeof(Stmt*));
            for (int i = 0; i < stmt->as.block.stmt_count; i++) {
                new_stmt->as.block.statements[i] = copy_stmt_with_substitution(
                    stmt->as.block.statements[i], params, param_count, args, arg_count);
            }
            break;

        case STMT_PRINT:
            new_stmt->as.print.expression = copy_expr_with_substitution(
                stmt->as.print.expression, params, param_count, args, arg_count);
            break;

        case STMT_ASSIGNMENT:
            new_stmt->as.assignment.target = copy_expr_with_substitution(
                stmt->as.assignment.target, params, param_count, args, arg_count);
            new_stmt->as.assignment.value = copy_expr_with_substitution(
                stmt->as.assignment.value, params, param_count, args, arg_count);
            break;

        default:
            break;
    }

    return new_stmt;
}

// Mangle generic name
char* mangle_generic_name(const char* base, DataType* type_args, int count) {
    size_t len = strlen(base) + 1;
    for (int i = 0; i < count; i++) {
        len += 16; // conservative estimate per type
    }

    char* mangled = malloc(len);
    if (!mangled) return NULL;
    
    /* Use safe concatenation */
    snprintf(mangled, len, "%s", base);
    size_t current_len = strlen(mangled);

    for (int i = 0; i < count; i++) {
        snprintf(mangled + current_len, len - current_len, "_");
        current_len++;
        
        const char* type_name = "Unknown";
        switch (type_args[i]) {
            case TYPE_INT:    type_name = "Int"; break;
            case TYPE_FLOAT:  type_name = "Float"; break;
            case TYPE_STRING: type_name = "String"; break;
            case TYPE_BOOL:   type_name = "Bool"; break;
            default:          break;
        }
        snprintf(mangled + current_len, len - current_len, "%s", type_name);
        current_len = strlen(mangled);
    }
    return mangled;
}

// Find existing instantiation
GenericInstance* find_instantiation(const char* base_name, DataType* type_args, int arg_count,
                                    MonoContext* ctx) {
    GenericInstance* current = ctx->instances;

    while (current) {
        if (strcmp(current->base_name, base_name) == 0 &&
            current->type_arg_count == arg_count) {

            // Check if type arguments match
            bool match = true;
            for (int i = 0; i < arg_count; i++) {
                if (current->type_args[i] != type_args[i]) {
                    match = false;
                    break;
                }
            }

            if (match) {
                return current;
            }
        }

        current = current->next;
    }

    return NULL;
}

// Monomorphize a generic class
GenericInstance* monomorphize_class(ClassStmt* generic_class, DataType* type_args, int arg_count,
                                    MonoContext* ctx) {
    // Check if already instantiated
    GenericInstance* existing = find_instantiation(generic_class->name, type_args, arg_count, ctx);
    if (existing) {
        return existing;
    }

    // Create new instance
    GenericInstance* instance = malloc(sizeof(GenericInstance));
    instance->base_name = strdup(generic_class->name);
    instance->type_args = arg_count > 0 ? malloc(arg_count * sizeof(DataType)) : NULL;
    if (arg_count > 0 && type_args) {
        memcpy(instance->type_args, type_args, arg_count * sizeof(DataType));
    }
    instance->type_arg_count = arg_count;

    // Generate mangled name
    instance->mangled_name = mangle_generic_name(generic_class->name, type_args, arg_count);

    // Create specialized class
    instance->specialized_class = malloc(sizeof(ClassStmt));
    memcpy(instance->specialized_class, generic_class, sizeof(ClassStmt));
    instance->specialized_class->name = strdup(instance->mangled_name);
    instance->specialized_class->type_params = NULL;  // No longer generic
    instance->specialized_class->type_param_count = 0;

    // Specialize fields
    instance->specialized_class->fields = malloc(generic_class->field_count * sizeof(FieldDecl));
    for (int i = 0; i < generic_class->field_count; i++) {
        FieldDecl* orig_field = &generic_class->fields[i];
        FieldDecl* new_field = &instance->specialized_class->fields[i];

        memcpy(new_field, orig_field, sizeof(FieldDecl));
        new_field->name = strdup(orig_field->name);

        // Substitute type
        for (int j = 0; j < arg_count && j < generic_class->type_param_count; j++) {
            if (orig_field->type == TYPE_GENERIC && orig_field->class_name &&
                strcmp(orig_field->class_name, generic_class->type_params[j].name) == 0) {
                new_field->type = type_args[j];
                new_field->class_name = NULL;
            }
        }
    }

    // Specialize methods
    instance->specialized_class->methods = malloc(generic_class->method_count * sizeof(MethodDecl));
    for (int i = 0; i < generic_class->method_count; i++) {
        MethodDecl* orig_method = &generic_class->methods[i];
        MethodDecl* new_method = &instance->specialized_class->methods[i];

        memcpy(new_method, orig_method, sizeof(MethodDecl));
        new_method->name = strdup(orig_method->name);

        // Substitute in method body
        new_method->body = copy_stmt_with_substitution(
            orig_method->body, generic_class->type_params, generic_class->type_param_count, type_args, arg_count);

        // Substitute return type
        for (int j = 0; j < arg_count && j < generic_class->type_param_count; j++) {
            if (orig_method->return_type == TYPE_GENERIC) {
                new_method->return_type = type_args[j];
            }
        }
    }

    // Add to instance list
    instance->next = ctx->instances;
    ctx->instances = instance;
    ctx->instance_count++;

    return instance;
}

// Monomorphize a generic function
FunctionStmt* monomorphize_function(FunctionStmt* generic_func, DataType* type_args, int arg_count,
                                    MonoContext* ctx) {
    (void)ctx;  // May be used for caching

    FunctionStmt* specialized = malloc(sizeof(FunctionStmt));
    memcpy(specialized, generic_func, sizeof(FunctionStmt));

    // Generate mangled name
    char* mangled_name = mangle_generic_name(generic_func->name, type_args, arg_count);
    specialized->name = mangled_name;
    specialized->type_params = NULL;
    specialized->type_param_count = 0;

    // Substitute body
    specialized->body = copy_stmt_with_substitution(
        generic_func->body, generic_func->type_params, generic_func->type_param_count, type_args, arg_count);

    // Substitute return type
    for (int i = 0; i < arg_count && i < generic_func->type_param_count; i++) {
        if (generic_func->return_type == TYPE_GENERIC) {
            specialized->return_type = type_args[i];
        }
    }

    return specialized;
}

// Type substitution helpers
Expr* substitute_type_in_expr(Expr* expr, TypeParam* params, int param_count,
                              DataType* args, int arg_count) {
    return copy_expr_with_substitution(expr, params, param_count, args, arg_count);
}

Stmt* substitute_type_in_stmt(Stmt* stmt, TypeParam* params, int param_count,
                              DataType* args, int arg_count) {
    return copy_stmt_with_substitution(stmt, params, param_count, args, arg_count);
}

// Free monomorphization context
void free_mono_context(MonoContext* ctx) {
    GenericInstance* current = ctx->instances;

    while (current) {
        GenericInstance* next = current->next;

        free(current->base_name);
        free(current->mangled_name);
        free(current->type_args);
        if (current->specialized_class) {
            ClassStmt* cls = current->specialized_class;
            if (cls->name) free(cls->name);
            if (cls->fields) {
                for (int i = 0; i < cls->field_count; i++) {
                    if (cls->fields[i].name) free(cls->fields[i].name);
                }
                free(cls->fields);
            }
            if (cls->methods) {
                for (int i = 0; i < cls->method_count; i++) {
                    if (cls->methods[i].name) free(cls->methods[i].name);
                    /* Body is deep-copied; ideally we'd have free_stmt() but
                     * for now we just free the array. */
                }
                free(cls->methods);
            }
            free(cls);
        }
        free(current);

        current = next;
    }

    if (ctx->type_environment) {
        free_symbol_table(ctx->type_environment);
        free(ctx->type_environment);
    }
}
