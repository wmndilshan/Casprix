#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include "compiler/frontend/ast.h"

// Expression creators
Expr* create_binary_expr(Expr* left, TokenType op, Expr* right, int line, int col) {
    Expr* expr = ALLOCATE(Expr, 1);
    expr->type = EXPR_BINARY;
    expr->data_type = TYPE_ERROR;
    expr->class_name = NULL;
    expr->type_info = NULL;
    expr->line = line;
    expr->column = col;
    expr->as.binary.left = left;
    expr->as.binary.operator = op;
    expr->as.binary.right = right;
    return expr;
}

Expr* create_unary_expr(TokenType op, Expr* operand, int line, int col) {
    Expr* expr = ALLOCATE(Expr, 1);
    expr->type = EXPR_UNARY;
    expr->data_type = TYPE_ERROR;
    expr->class_name = NULL;
    expr->type_info = NULL;
    expr->line = line;
    expr->column = col;
    expr->as.unary.operator = op;
    expr->as.unary.operand = operand;
    return expr;
}

Expr* create_literal_expr(LiteralExpr literal, int line, int col) {
    Expr* expr = ALLOCATE(Expr, 1);
    expr->type = EXPR_LITERAL;
    expr->data_type = literal.type;
    expr->class_name = NULL;
    expr->type_info = NULL;
    expr->line = line;
    expr->column = col;
    expr->as.literal = literal;
    return expr;
}

Expr* create_variable_expr(char* name, int line, int col) {
    Expr* expr = ALLOCATE(Expr, 1);
    expr->type = EXPR_VARIABLE;
    expr->data_type = TYPE_ERROR;
    expr->class_name = NULL;
    expr->type_info = NULL;
    expr->line = line;
    expr->column = col;
    expr->as.variable.name = name;
    expr->as.variable.is_move = false;
    expr->as.variable.is_closure_value = false;
    expr->as.variable.closure_capture_count = 0;
    expr->as.variable.closure_lambda_id = -1;
    expr->as.variable.closure_capture_types = NULL;
    return expr;
}

Expr* create_call_expr(Expr* callee, Expr** args, int arg_count, int line, int col) {
    Expr* expr = ALLOCATE(Expr, 1);
    expr->type = EXPR_CALL;
    expr->data_type = TYPE_ERROR;
    expr->class_name = NULL;
    expr->type_info = NULL;
    expr->line = line;
    expr->column = col;
    expr->as.call.callee = callee;
    expr->as.call.name = NULL;
    if (callee) {
        if (callee->type == EXPR_VARIABLE && callee->as.variable.name) {
            expr->as.call.name = strdup(callee->as.variable.name);
        } else if (callee->type == EXPR_GENERIC_INST && callee->as.generic_inst.base_name) {
            expr->as.call.name = strdup(callee->as.generic_inst.base_name);
        }
    }
    expr->as.call.arguments = args;
    expr->as.call.arg_count = arg_count;
    return expr;
}

Expr* create_member_access_expr(Expr* object, char* member_name, bool is_method_call,
                                Expr** args, int arg_count, int line, int col) {
    Expr* expr = ALLOCATE(Expr, 1);
    expr->type = EXPR_MEMBER_ACCESS;
    expr->data_type = TYPE_ERROR;
    expr->class_name = NULL;
    expr->type_info = NULL;
    expr->line = line;
    expr->column = col;
    expr->as.member.object = object;
    expr->as.member.member_name = member_name;
    expr->as.member.is_method_call = is_method_call;
    expr->as.member.arguments = args;
    expr->as.member.arg_count = arg_count;
    return expr;
}

Expr* create_static_access_expr(char* class_name, char* member_name, bool is_method_call,
                                Expr** args, int arg_count, int line, int col) {
    Expr* expr = ALLOCATE(Expr, 1);
    expr->type = EXPR_STATIC_ACCESS;
    expr->data_type = TYPE_ERROR;
    expr->class_name = NULL;
    expr->type_info = NULL;
    expr->line = line;
    expr->column = col;
    expr->as.static_access.class_name = class_name;
    expr->as.static_access.member_name = member_name;
    expr->as.static_access.is_method_call = is_method_call;
    expr->as.static_access.arguments = args;
    expr->as.static_access.arg_count = arg_count;
    return expr;
}

Expr* create_this_expr(char* class_name, int line, int col) {
    Expr* expr = ALLOCATE(Expr, 1);
    expr->type = EXPR_THIS;
    expr->data_type = TYPE_CLASS;
    expr->class_name = class_name ? strdup(class_name) : NULL;  // Set class name
    expr->type_info = NULL;
    expr->line = line;
    expr->column = col;
    expr->as.this_expr.class_name = class_name;
    return expr;
}

Expr* create_new_expr(char* class_name, Expr** arguments, int arg_count, int line, int col) {
    Expr* expr = ALLOCATE(Expr, 1);
    expr->type = EXPR_NEW;
    expr->data_type = TYPE_CLASS;
    expr->class_name = strdup(class_name);  // Set class name
    expr->type_info = NULL;
    expr->line = line;
    expr->column = col;
    expr->as.new_expr.class_name = class_name;
    expr->as.new_expr.arguments = arguments;
    expr->as.new_expr.arg_count = arg_count;
    return expr;
}

Expr* create_index_expr(Expr* array, Expr* index, int line, int col) {
    Expr* expr = ALLOCATE(Expr, 1);
    expr->type = EXPR_INDEX;
    expr->data_type = TYPE_ERROR;
    expr->class_name = NULL;
    expr->type_info = NULL;
    expr->line = line;
    expr->column = col;
    expr->as.index.array = array;
    expr->as.index.index = index;
    return expr;
}

Expr* create_await_expr(Expr* expression, int line, int col) {
    Expr* expr = ALLOCATE(Expr, 1);
    expr->type = EXPR_AWAIT;
    expr->data_type = TYPE_ERROR;
    expr->class_name = NULL;
    expr->type_info = NULL;
    expr->line = line;
    expr->column = col;
    expr->as.await_expr.expression = expression;
    return expr;
}

Expr* create_array_literal_expr(Expr** elements, int count, int line, int col) {
    Expr* expr = ALLOCATE(Expr, 1);
    expr->type = EXPR_ARRAY_LITERAL;
    expr->data_type = TYPE_CLASS;
    expr->class_name = strdup("Array");
    expr->type_info = NULL;
    expr->line = line;
    expr->column = col;
    expr->as.array_literal.elements = elements;
    expr->as.array_literal.element_count = count;
    return expr;
}

// Statement creators
Stmt* create_declaration_stmt(char* name, DataType type, Expr* init, int line, int col) {
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_DECLARATION;
    stmt->line = line;
    stmt->column = col;
    stmt->as.declaration.name = name;
    stmt->as.declaration.type = type;
    stmt->as.declaration.class_name = NULL;  // Will be set by parser for class types
    stmt->as.declaration.initializer = init;
    stmt->as.declaration.type_info = NULL;
    stmt->as.declaration.ownership = OWNERSHIP_OWNED;
    stmt->as.declaration.is_const = false;
    stmt->as.declaration.is_mutable = true;
    return stmt;
}

Stmt* create_assignment_stmt(Expr* target, Expr* value, int line, int col) {
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_ASSIGNMENT;
    stmt->line = line;
    stmt->column = col;
    stmt->as.assignment.target = target;
    stmt->as.assignment.value = value;
    return stmt;
}

Stmt* create_print_stmt(Expr* expr, int line, int col) {
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_PRINT;
    stmt->line = line;
    stmt->column = col;
    stmt->as.print.expression = expr;
    return stmt;
}

Stmt* create_if_stmt(Expr* cond, Stmt* then_br, Stmt* else_br, int line, int col) {
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_IF;
    stmt->line = line;
    stmt->column = col;
    stmt->as.if_stmt.condition = cond;
    stmt->as.if_stmt.then_branch = then_br;
    stmt->as.if_stmt.else_branch = else_br;
    return stmt;
}

Stmt* create_for_stmt(char* var, DataType type, Expr* init, Expr* cond,
                      Stmt* incr, Stmt* body, int line, int col) {
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_FOR;
    stmt->line = line;
    stmt->column = col;
    stmt->as.for_stmt.variable = var;
    stmt->as.for_stmt.var_type = type;
    stmt->as.for_stmt.initializer = init;
    stmt->as.for_stmt.condition = cond;
    stmt->as.for_stmt.increment = incr;
    stmt->as.for_stmt.body = body;
    return stmt;
}

Stmt* create_while_stmt(Expr* cond, Stmt* body, int line, int col) {
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_WHILE;
    stmt->line = line;
    stmt->column = col;
    stmt->as.while_stmt.condition = cond;
    stmt->as.while_stmt.body = body;
    return stmt;
}

Stmt* create_function_stmt(char* name, Parameter* params, int param_count,
                           DataType ret_type, Stmt* body, int line, int col) {
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_FUNCTION;
    stmt->line = line;
    stmt->column = col;
    stmt->as.function.name = name;
    stmt->as.function.parameters = params;
    stmt->as.function.param_count = param_count;
    stmt->as.function.return_type = ret_type;
    stmt->as.function.return_type_info = NULL;
    stmt->as.function.body = body;
    stmt->as.function.type_params = NULL;
    stmt->as.function.type_param_count = 0;
    return stmt;
}

Stmt* create_return_stmt(Expr* value, int line, int col) {
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_RETURN;
    stmt->line = line;
    stmt->column = col;
    stmt->as.return_stmt.value = value;
    return stmt;
}

Stmt* create_expr_stmt(Expr* expr, int line, int col) {
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_EXPR;
    stmt->line = line;
    stmt->column = col;
    stmt->as.expr_stmt.expression = expr;
    return stmt;
}

Stmt* create_block_stmt(Stmt** stmts, int count, int line, int col) {
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_BLOCK;
    stmt->line = line;
    stmt->column = col;
    stmt->as.block.statements = stmts;
    stmt->as.block.stmt_count = count;
    stmt->as.block.is_alloc_scope = false;
    return stmt;
}

Stmt* create_include_stmt(char* module_name, bool is_import, int line, int col) {
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_INCLUDE;
    stmt->line = line;
    stmt->column = col;
    stmt->as.include.module_name = module_name;
    stmt->as.include.is_import = is_import;
    return stmt;
}

Stmt* create_break_stmt(int line, int col) {
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_BREAK;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* create_continue_stmt(int line, int col) {
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_CONTINUE;
    stmt->line = line;
    stmt->column = col;
    return stmt;
}

Stmt* create_class_stmt(char* name, char* parent_name, FieldDecl* fields, int field_count,
                        MethodDecl* methods, int method_count, int line, int col) {
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_CLASS;
    stmt->line = line;
    stmt->column = col;
    stmt->as.class_stmt.name = name;
    stmt->as.class_stmt.parent_name = parent_name;
    stmt->as.class_stmt.is_abstract = false;
    stmt->as.class_stmt.fields = fields;
    stmt->as.class_stmt.field_count = field_count;
    stmt->as.class_stmt.methods = methods;
    stmt->as.class_stmt.method_count = method_count;
    stmt->as.class_stmt.type_params = NULL;
    stmt->as.class_stmt.type_param_count = 0;
    stmt->as.class_stmt.implements = NULL;
    stmt->as.class_stmt.implements_count = 0;
    return stmt;
}

Stmt* create_extern_stmt(char* name, Parameter* params, int param_count,
                         DataType ret_type, char* class_name, int line, int col) {
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_EXTERN;
    stmt->line = line;
    stmt->column = col;
    stmt->as.extern_stmt.name = name;
    stmt->as.extern_stmt.parameters = params;
    stmt->as.extern_stmt.param_count = param_count;
    stmt->as.extern_stmt.return_type = ret_type;
    stmt->as.extern_stmt.class_name = class_name;
    return stmt;
}

// Memory management
void free_expr(Expr* expr) {
    if (!expr) return;

    // Free class_name if set
    if (expr->class_name) {
        free(expr->class_name);
    }
    // Free extended type info
    if (expr->type_info) {
        free_type_info(expr->type_info);
    }

    switch (expr->type) {
        case EXPR_BINARY:
            free_expr(expr->as.binary.left);
            free_expr(expr->as.binary.right);
            break;
        case EXPR_UNARY:
            free_expr(expr->as.unary.operand);
            break;
        case EXPR_LITERAL:
            if (expr->data_type == TYPE_STRING) {
                free(expr->as.literal.value.string_value);
            }
            break;
        case EXPR_VARIABLE:
            free(expr->as.variable.name);
            if (expr->as.variable.closure_capture_types) {
                free(expr->as.variable.closure_capture_types);
            }
            break;
        case EXPR_CALL:
            free_expr(expr->as.call.callee);
            free(expr->as.call.name);
            for (int i = 0; i < expr->as.call.arg_count; i++) {
                free_expr(expr->as.call.arguments[i]);
            }
            free(expr->as.call.arguments);
            break;
        case EXPR_MEMBER_ACCESS:
            free_expr(expr->as.member.object);
            free(expr->as.member.member_name);
            for (int i = 0; i < expr->as.member.arg_count; i++) {
                free_expr(expr->as.member.arguments[i]);
            }
            if (expr->as.member.arguments) {
                free(expr->as.member.arguments);
            }
            break;
        case EXPR_THIS:
            if (expr->as.this_expr.class_name) {
                free(expr->as.this_expr.class_name);
            }
            break;
        case EXPR_NEW:
            if (expr->as.new_expr.class_name) {
                free(expr->as.new_expr.class_name);
            }
            for (int i = 0; i < expr->as.new_expr.arg_count; i++) {
                free_expr(expr->as.new_expr.arguments[i]);
            }
            if (expr->as.new_expr.arguments) {
                free(expr->as.new_expr.arguments);
            }
            break;
        case EXPR_INDEX:
            free_expr(expr->as.index.array);
            free_expr(expr->as.index.index);
            break;
        case EXPR_STATIC_ACCESS:
            if (expr->as.static_access.class_name) {
                free(expr->as.static_access.class_name);
            }
            if (expr->as.static_access.member_name) {
                free(expr->as.static_access.member_name);
            }
            for (int i = 0; i < expr->as.static_access.arg_count; i++) {
                free_expr(expr->as.static_access.arguments[i]);
            }
            if (expr->as.static_access.arguments) {
                free(expr->as.static_access.arguments);
            }
            break;
        case EXPR_SUPER:
            if (expr->as.super_expr.member_name) {
                free(expr->as.super_expr.member_name);
            }
            for (int i = 0; i < expr->as.super_expr.arg_count; i++) {
                free_expr(expr->as.super_expr.arguments[i]);
            }
            if (expr->as.super_expr.arguments) {
                free(expr->as.super_expr.arguments);
            }
            break;
        case EXPR_LAMBDA:
            // Free parameters
            for (int i = 0; i < expr->as.lambda.param_count; i++) {
                free(expr->as.lambda.parameters[i].name);
                if (expr->as.lambda.parameters[i].class_name) {
                    free(expr->as.lambda.parameters[i].class_name);
                }
                if (expr->as.lambda.parameters[i].type_info) {
                    free_type_info(expr->as.lambda.parameters[i].type_info);
                }
            }
            if (expr->as.lambda.parameters) {
                free(expr->as.lambda.parameters);
            }
            if (expr->as.lambda.return_class_name) {
                free(expr->as.lambda.return_class_name);
            }
            if (expr->as.lambda.expr_body) {
                free_expr(expr->as.lambda.expr_body);
            }
            if (expr->as.lambda.block_body) {
                free_stmt(expr->as.lambda.block_body);
            }
            // Free captured variables
            for (int i = 0; i < expr->as.lambda.capture_count; i++) {
                free(expr->as.lambda.captured_vars[i]);
            }
            if (expr->as.lambda.captured_vars) {
                free(expr->as.lambda.captured_vars);
            }
            if (expr->as.lambda.captured_types) {
                free(expr->as.lambda.captured_types);
            }
            if (expr->as.lambda.captured_is_mutable) {
                free(expr->as.lambda.captured_is_mutable);
            }
            break;
        case EXPR_GENERIC_INST:
            if (expr->as.generic_inst.base_name) {
                free(expr->as.generic_inst.base_name);
            }
            if (expr->as.generic_inst.type_args) {
                free(expr->as.generic_inst.type_args);
            }
            for (int i = 0; i < expr->as.generic_inst.type_arg_count; i++) {
                if (expr->as.generic_inst.type_arg_classes[i]) {
                    free(expr->as.generic_inst.type_arg_classes[i]);
                }
            }
            if (expr->as.generic_inst.type_arg_classes) {
                free(expr->as.generic_inst.type_arg_classes);
            }
            break;
        case EXPR_AWAIT:
            free_expr(expr->as.await_expr.expression);
            break;
        case EXPR_ARRAY_LITERAL:
            for (int i = 0; i < expr->as.array_literal.element_count; i++) {
                free_expr(expr->as.array_literal.elements[i]);
            }
            if (expr->as.array_literal.elements) {
                free(expr->as.array_literal.elements);
            }
            break;
    }

    free(expr);
}

Expr* clone_expr(const Expr* expr) {
    if (!expr) return NULL;

    Expr* clone = ALLOCATE(Expr, 1);
    clone->type = expr->type;
    clone->data_type = expr->data_type;
    clone->class_name = expr->class_name ? strdup(expr->class_name) : NULL;
    clone->type_info = expr->type_info ? clone_type_info(expr->type_info) : NULL;
    clone->line = expr->line;
    clone->column = expr->column;

    switch (expr->type) {
        case EXPR_BINARY:
            clone->as.binary.left = clone_expr(expr->as.binary.left);
            clone->as.binary.operator = expr->as.binary.operator;
            clone->as.binary.right = clone_expr(expr->as.binary.right);
            break;
        case EXPR_UNARY:
            clone->as.unary.operator = expr->as.unary.operator;
            clone->as.unary.operand = clone_expr(expr->as.unary.operand);
            break;
        case EXPR_LITERAL:
            clone->as.literal.type = expr->as.literal.type;
            if (expr->as.literal.type == TYPE_STRING) {
                clone->as.literal.value.string_value = expr->as.literal.value.string_value ? strdup(expr->as.literal.value.string_value) : NULL;
            } else {
                clone->as.literal.value = expr->as.literal.value;
            }
            break;
        case EXPR_VARIABLE:
            clone->as.variable.name = expr->as.variable.name ? strdup(expr->as.variable.name) : NULL;
            clone->as.variable.is_move = expr->as.variable.is_move;
            clone->as.variable.is_closure_value = expr->as.variable.is_closure_value;
            clone->as.variable.closure_capture_count = expr->as.variable.closure_capture_count;
            clone->as.variable.closure_lambda_id = expr->as.variable.closure_lambda_id;
            if (expr->as.variable.closure_capture_types && expr->as.variable.closure_capture_count > 0) {
                clone->as.variable.closure_capture_types = ALLOCATE(DataType, expr->as.variable.closure_capture_count);
                memcpy(clone->as.variable.closure_capture_types, expr->as.variable.closure_capture_types, sizeof(DataType) * expr->as.variable.closure_capture_count);
            } else {
                clone->as.variable.closure_capture_types = NULL;
            }
            break;
        case EXPR_CALL:
            clone->as.call.callee = clone_expr(expr->as.call.callee);
            clone->as.call.name = expr->as.call.name ? strdup(expr->as.call.name) : NULL;
            clone->as.call.arg_count = expr->as.call.arg_count;
            if (expr->as.call.arguments) {
                clone->as.call.arguments = ALLOCATE(Expr*, expr->as.call.arg_count);
                for (int i = 0; i < expr->as.call.arg_count; i++) {
                    clone->as.call.arguments[i] = clone_expr(expr->as.call.arguments[i]);
                }
            } else {
                clone->as.call.arguments = NULL;
            }
            break;
        case EXPR_MEMBER_ACCESS:
            clone->as.member.object = clone_expr(expr->as.member.object);
            clone->as.member.member_name = expr->as.member.member_name ? strdup(expr->as.member.member_name) : NULL;
            clone->as.member.is_method_call = expr->as.member.is_method_call;
            clone->as.member.arg_count = expr->as.member.arg_count;
            if (expr->as.member.arguments) {
                clone->as.member.arguments = ALLOCATE(Expr*, expr->as.member.arg_count);
                for (int i = 0; i < expr->as.member.arg_count; i++) {
                    clone->as.member.arguments[i] = clone_expr(expr->as.member.arguments[i]);
                }
            } else {
                clone->as.member.arguments = NULL;
            }
            break;
        case EXPR_STATIC_ACCESS:
            clone->as.static_access.class_name = expr->as.static_access.class_name ? strdup(expr->as.static_access.class_name) : NULL;
            clone->as.static_access.member_name = expr->as.static_access.member_name ? strdup(expr->as.static_access.member_name) : NULL;
            clone->as.static_access.is_method_call = expr->as.static_access.is_method_call;
            clone->as.static_access.arg_count = expr->as.static_access.arg_count;
            if (expr->as.static_access.arguments) {
                clone->as.static_access.arguments = ALLOCATE(Expr*, expr->as.static_access.arg_count);
                for (int i = 0; i < expr->as.static_access.arg_count; i++) {
                    clone->as.static_access.arguments[i] = clone_expr(expr->as.static_access.arguments[i]);
                }
            } else {
                clone->as.static_access.arguments = NULL;
            }
            break;
        case EXPR_THIS:
            clone->as.this_expr.class_name = expr->as.this_expr.class_name ? strdup(expr->as.this_expr.class_name) : NULL;
            break;
        case EXPR_SUPER:
            clone->as.super_expr.member_name = expr->as.super_expr.member_name ? strdup(expr->as.super_expr.member_name) : NULL;
            clone->as.super_expr.is_method_call = expr->as.super_expr.is_method_call;
            clone->as.super_expr.arg_count = expr->as.super_expr.arg_count;
            if (expr->as.super_expr.arguments) {
                clone->as.super_expr.arguments = ALLOCATE(Expr*, expr->as.super_expr.arg_count);
                for (int i = 0; i < expr->as.super_expr.arg_count; i++) {
                    clone->as.super_expr.arguments[i] = clone_expr(expr->as.super_expr.arguments[i]);
                }
            } else {
                clone->as.super_expr.arguments = NULL;
            }
            break;
        case EXPR_NEW:
            clone->as.new_expr.class_name = expr->as.new_expr.class_name ? strdup(expr->as.new_expr.class_name) : NULL;
            clone->as.new_expr.arg_count = expr->as.new_expr.arg_count;
            if (expr->as.new_expr.arguments) {
                clone->as.new_expr.arguments = ALLOCATE(Expr*, expr->as.new_expr.arg_count);
                for (int i = 0; i < expr->as.new_expr.arg_count; i++) {
                    clone->as.new_expr.arguments[i] = clone_expr(expr->as.new_expr.arguments[i]);
                }
            } else {
                clone->as.new_expr.arguments = NULL;
            }
            break;
        case EXPR_INDEX:
            clone->as.index.array = clone_expr(expr->as.index.array);
            clone->as.index.index = clone_expr(expr->as.index.index);
            break;
        case EXPR_AWAIT:
            clone->as.await_expr.expression = clone_expr(expr->as.await_expr.expression);
            break;
        case EXPR_ARRAY_LITERAL:
            clone->as.array_literal.element_count = expr->as.array_literal.element_count;
            if (expr->as.array_literal.elements) {
                clone->as.array_literal.elements = ALLOCATE(Expr*, expr->as.array_literal.element_count);
                for (int i = 0; i < expr->as.array_literal.element_count; i++) {
                    clone->as.array_literal.elements[i] = clone_expr(expr->as.array_literal.elements[i]);
                }
            } else {
                clone->as.array_literal.elements = NULL;
            }
            break;
        default:
            break;
    }

    return clone;
}

void free_stmt(Stmt* stmt) {
    if (!stmt) return;
    
    switch (stmt->type) {
        case STMT_DECLARATION:
            free(stmt->as.declaration.name);
            if (stmt->as.declaration.class_name) {
                free(stmt->as.declaration.class_name);
            }
            if (stmt->as.declaration.type_info) {
                free_type_info(stmt->as.declaration.type_info);
            }
            free_expr(stmt->as.declaration.initializer);
            break;
        case STMT_ASSIGNMENT:
            free_expr(stmt->as.assignment.target);
            free_expr(stmt->as.assignment.value);
            break;
        case STMT_PRINT:
            free_expr(stmt->as.print.expression);
            break;
        case STMT_IF:
            free_expr(stmt->as.if_stmt.condition);
            free_stmt(stmt->as.if_stmt.then_branch);
            free_stmt(stmt->as.if_stmt.else_branch);
            break;
        case STMT_FOR:
            free(stmt->as.for_stmt.variable);
            free_expr(stmt->as.for_stmt.initializer);
            free_expr(stmt->as.for_stmt.condition);
            free_stmt(stmt->as.for_stmt.increment);
            free_stmt(stmt->as.for_stmt.body);
            break;
        case STMT_WHILE:
            free_expr(stmt->as.while_stmt.condition);
            free_stmt(stmt->as.while_stmt.body);
            break;
        case STMT_FUNCTION:
            free(stmt->as.function.name);
            for (int i = 0; i < stmt->as.function.param_count; i++) {
                free(stmt->as.function.parameters[i].name);
                if (stmt->as.function.parameters[i].class_name) {
                    free(stmt->as.function.parameters[i].class_name);
                }
                if (stmt->as.function.parameters[i].type_info) {
                    free_type_info(stmt->as.function.parameters[i].type_info);
                }
            }
            free(stmt->as.function.parameters);
            if (stmt->as.function.return_type_info) {
                free_type_info(stmt->as.function.return_type_info);
            }
            free_stmt(stmt->as.function.body);
            // Free generic type parameters
            for (int i = 0; i < stmt->as.function.type_param_count; i++) {
                free(stmt->as.function.type_params[i].name);
                if (stmt->as.function.type_params[i].constraint) {
                    free(stmt->as.function.type_params[i].constraint);
                }
                if (stmt->as.function.type_params[i].default_class) {
                    free(stmt->as.function.type_params[i].default_class);
                }
            }
            if (stmt->as.function.type_params) {
                free(stmt->as.function.type_params);
            }
            break;
        case STMT_RETURN:
            free_expr(stmt->as.return_stmt.value);
            break;
        case STMT_EXPR:
            free_expr(stmt->as.expr_stmt.expression);
            break;
        case STMT_BLOCK:
            for (int i = 0; i < stmt->as.block.stmt_count; i++) {
                free_stmt(stmt->as.block.statements[i]);
            }
            free(stmt->as.block.statements);
            break;
        case STMT_INCLUDE:
            free(stmt->as.include.module_name);
            break;
        case STMT_BREAK:
        case STMT_CONTINUE:
            // No data to free
            break;
        case STMT_CLASS:
            free(stmt->as.class_stmt.name);
            if (stmt->as.class_stmt.parent_name) {
                free(stmt->as.class_stmt.parent_name);
            }
            // Free fields
            for (int i = 0; i < stmt->as.class_stmt.field_count; i++) {
                free(stmt->as.class_stmt.fields[i].name);
                if (stmt->as.class_stmt.fields[i].class_name) {
                    free(stmt->as.class_stmt.fields[i].class_name);
                }
                if (stmt->as.class_stmt.fields[i].default_value) {
                    free_expr(stmt->as.class_stmt.fields[i].default_value);
                }
            }
            if (stmt->as.class_stmt.fields) {
                free(stmt->as.class_stmt.fields);
            }
            // Free methods
            for (int i = 0; i < stmt->as.class_stmt.method_count; i++) {
                free(stmt->as.class_stmt.methods[i].name);
                for (int j = 0; j < stmt->as.class_stmt.methods[i].param_count; j++) {
                    free(stmt->as.class_stmt.methods[i].parameters[j].name);
                    if (stmt->as.class_stmt.methods[i].parameters[j].class_name) {
                        free(stmt->as.class_stmt.methods[i].parameters[j].class_name);
                    }
                    if (stmt->as.class_stmt.methods[i].parameters[j].type_info) {
                        free_type_info(stmt->as.class_stmt.methods[i].parameters[j].type_info);
                    }
                }
                if (stmt->as.class_stmt.methods[i].parameters) {
                    free(stmt->as.class_stmt.methods[i].parameters);
                }
                free_stmt(stmt->as.class_stmt.methods[i].body);
            }
            if (stmt->as.class_stmt.methods) {
                free(stmt->as.class_stmt.methods);
            }
            // Free generic type parameters
            for (int i = 0; i < stmt->as.class_stmt.type_param_count; i++) {
                free(stmt->as.class_stmt.type_params[i].name);
                if (stmt->as.class_stmt.type_params[i].constraint) {
                    free(stmt->as.class_stmt.type_params[i].constraint);
                }
                if (stmt->as.class_stmt.type_params[i].default_class) {
                    free(stmt->as.class_stmt.type_params[i].default_class);
                }
            }
            if (stmt->as.class_stmt.type_params) {
                free(stmt->as.class_stmt.type_params);
            }
            for (int i = 0; i < stmt->as.class_stmt.implements_count; i++) {
                free(stmt->as.class_stmt.implements[i]);
            }
            if (stmt->as.class_stmt.implements) {
                free(stmt->as.class_stmt.implements);
            }
            break;
        case STMT_EXTERN:
            free(stmt->as.extern_stmt.name);
            for (int i = 0; i < stmt->as.extern_stmt.param_count; i++) {
                free(stmt->as.extern_stmt.parameters[i].name);
                if (stmt->as.extern_stmt.parameters[i].class_name) {
                    free(stmt->as.extern_stmt.parameters[i].class_name);
                }
                if (stmt->as.extern_stmt.parameters[i].type_info) {
                    free_type_info(stmt->as.extern_stmt.parameters[i].type_info);
                }
            }
            free(stmt->as.extern_stmt.parameters);
            if (stmt->as.extern_stmt.class_name) {
                free(stmt->as.extern_stmt.class_name);
            }
            break;
        case STMT_STRUCT:
            free(stmt->as.struct_stmt.name);
            for (int i = 0; i < stmt->as.struct_stmt.field_count; i++) {
                free(stmt->as.struct_stmt.fields[i].name);
                if (stmt->as.struct_stmt.fields[i].class_name) {
                    free(stmt->as.struct_stmt.fields[i].class_name);
                }
                if (stmt->as.struct_stmt.fields[i].type_info) {
                    free_type_info(stmt->as.struct_stmt.fields[i].type_info);
                }
            }
            if (stmt->as.struct_stmt.fields) free(stmt->as.struct_stmt.fields);
            break;
        case STMT_ENUM:
            free(stmt->as.enum_stmt.name);
            for (int i = 0; i < stmt->as.enum_stmt.variant_count; i++) {
                free(stmt->as.enum_stmt.variants[i].name);
                if (stmt->as.enum_stmt.variants[i].payload_types)
                    free(stmt->as.enum_stmt.variants[i].payload_types);
                if (stmt->as.enum_stmt.variants[i].payload_names) {
                    for (int j = 0; j < stmt->as.enum_stmt.variants[i].payload_count; j++) {
                        if (stmt->as.enum_stmt.variants[i].payload_names[j])
                            free(stmt->as.enum_stmt.variants[i].payload_names[j]);
                    }
                    free(stmt->as.enum_stmt.variants[i].payload_names);
                }
            }
            if (stmt->as.enum_stmt.variants) free(stmt->as.enum_stmt.variants);
            break;
        case STMT_UNION:
            free(stmt->as.union_stmt.name);
            for (int i = 0; i < stmt->as.union_stmt.field_count; i++) {
                free(stmt->as.union_stmt.fields[i].name);
                if (stmt->as.union_stmt.fields[i].type_info) {
                    free_type_info(stmt->as.union_stmt.fields[i].type_info);
                }
            }
            if (stmt->as.union_stmt.fields) free(stmt->as.union_stmt.fields);
            break;

        /* ── New statement types ── */

        case STMT_FOR_IN:
            if (stmt->as.for_in_stmt.var_name) free(stmt->as.for_in_stmt.var_name);
            free_expr(stmt->as.for_in_stmt.iterable);
            free_stmt(stmt->as.for_in_stmt.body);
            break;

        case STMT_MATCH: {
            free_expr(stmt->as.match_stmt.subject);
            MatchStmt* ms = &stmt->as.match_stmt;
            for (int i = 0; i < ms->arm_count; i++) {
                free_expr(ms->arms[i].pattern);
                free_stmt(ms->arms[i].body);
            }
            if (ms->arms) free(ms->arms);
            break;
        }

        case STMT_THROW:
            free_expr(stmt->as.throw_stmt.value);
            break;

        case STMT_TRY: {
            TryStmt* t = &stmt->as.try_stmt;
            free_stmt(t->try_body);
            for (int i = 0; i < t->catch_count; i++) {
                if (t->catches[i].exception_var) free(t->catches[i].exception_var);
                if (t->catches[i].exception_type) free(t->catches[i].exception_type);
                free_stmt(t->catches[i].body);
            }
            if (t->catches) free(t->catches);
            free_stmt(t->finally_body);
            break;
        }

        case STMT_TRAIT:
            if (stmt->as.trait_stmt.type_params) {
                for (int i = 0; i < stmt->as.trait_stmt.type_param_count; i++) {
                    if (stmt->as.trait_stmt.type_params[i].name) {
                        free(stmt->as.trait_stmt.type_params[i].name);
                    }
                    if (stmt->as.trait_stmt.type_params[i].constraint) {
                        free(stmt->as.trait_stmt.type_params[i].constraint);
                    }
                    if (stmt->as.trait_stmt.type_params[i].default_class) {
                        free(stmt->as.trait_stmt.type_params[i].default_class);
                    }
                }
                free(stmt->as.trait_stmt.type_params);
            }
            if (stmt->as.trait_stmt.super_traits) {
                for (int i = 0; i < stmt->as.trait_stmt.super_count; i++) {
                    if (stmt->as.trait_stmt.super_traits[i]) {
                        free(stmt->as.trait_stmt.super_traits[i]);
                    }
                }
                free(stmt->as.trait_stmt.super_traits);
            }
            if (stmt->as.trait_stmt.methods) {
                for (int i = 0; i < stmt->as.trait_stmt.method_count; i++) {
                    TraitMethodDecl* method = &stmt->as.trait_stmt.methods[i];
                    if (method->name) {
                        free(method->name);
                    }
                    if (method->return_class_name) {
                        free(method->return_class_name);
                    }
                    if (method->parameters) {
                        for (int j = 0; j < method->param_count; j++) {
                            if (method->parameters[j].name) {
                                free(method->parameters[j].name);
                            }
                            if (method->parameters[j].class_name) {
                                free(method->parameters[j].class_name);
                            }
                            if (method->parameters[j].type_info) {
                                free_type_info(method->parameters[j].type_info);
                            }
                        }
                        free(method->parameters);
                    }
                    if (method->default_body) {
                        free_stmt(method->default_body);
                    }
                }
                free(stmt->as.trait_stmt.methods);
            }
            if (stmt->as.trait_stmt.name) {
                free(stmt->as.trait_stmt.name);
            }
            break;

        case STMT_CONST_DECL:
            /* Same layout as STMT_DECLARATION */
            free(stmt->as.declaration.name);
            if (stmt->as.declaration.class_name) {
                free(stmt->as.declaration.class_name);
            }
            if (stmt->as.declaration.type_info) {
                free_type_info(stmt->as.declaration.type_info);
            }
            free_expr(stmt->as.declaration.initializer);
            break;

        case STMT_IMPL:
            if (stmt->as.impl_stmt.target_name) free(stmt->as.impl_stmt.target_name);
            if (stmt->as.impl_stmt.trait_name) free(stmt->as.impl_stmt.trait_name);
            for (int i = 0; i < stmt->as.impl_stmt.method_count; i++) {
                free_stmt(stmt->as.impl_stmt.methods[i]);
            }
            if (stmt->as.impl_stmt.methods) free(stmt->as.impl_stmt.methods);
            for (int i = 0; i < stmt->as.impl_stmt.type_param_count; i++) {
                free(stmt->as.impl_stmt.type_params[i].name);
                if (stmt->as.impl_stmt.type_params[i].constraint)
                    free(stmt->as.impl_stmt.type_params[i].constraint);
                if (stmt->as.impl_stmt.type_params[i].default_class)
                    free(stmt->as.impl_stmt.type_params[i].default_class);
            }
            if (stmt->as.impl_stmt.type_params) free(stmt->as.impl_stmt.type_params);
            break;
    }

    free(stmt);
}

// ============================================================================
// TypeInfo - Creation, management, and type utility functions
// ============================================================================

TypeInfo* create_type_info(DataType base) {
    TypeInfo* info = ALLOCATE(TypeInfo, 1);
    info->base = base;
    info->element_type = NULL;
    info->static_size = 0;
    info->tensor_dims = NULL;
    info->tensor_ndims = 0;
    info->type_name = NULL;
    info->param_types = NULL;
    info->param_count = 0;
    info->return_type = NULL;
    return info;
}

TypeInfo* create_parameterized_type(DataType base, TypeInfo* element) {
    TypeInfo* info = create_type_info(base);
    info->element_type = element;
    return info;
}

TypeInfo* create_static_array_type(TypeInfo* element, int size) {
    TypeInfo* info = create_type_info(TYPE_STATIC_ARRAY);
    info->element_type = element;
    info->static_size = size;
    return info;
}

TypeInfo* create_vec_type(TypeInfo* element, int dim) {
    TypeInfo* info = create_type_info(TYPE_VEC);
    info->element_type = element;
    info->static_size = dim;  // 2, 3, 4, 8, or 16
    return info;
}

TypeInfo* create_mat_type(TypeInfo* element, int dim) {
    TypeInfo* info = create_type_info(TYPE_MAT);
    info->element_type = element;
    info->static_size = dim;  // 2, 3, or 4
    return info;
}

TypeInfo* create_tensor_type(TypeInfo* element, int* dims, int ndims) {
    TypeInfo* info = create_type_info(TYPE_TENSOR);
    info->element_type = element;
    info->tensor_ndims = ndims;
    if (ndims > 0 && dims) {
        info->tensor_dims = ALLOCATE(int, ndims);
        memcpy(info->tensor_dims, dims, sizeof(int) * ndims);
    }
    return info;
}

TypeInfo* create_named_type(DataType base, const char* name) {
    TypeInfo* info = create_type_info(base);
    info->type_name = strdup(name);
    return info;
}

void free_type_info(TypeInfo* info) {
    if (!info) return;
    if (info->element_type) free_type_info(info->element_type);
    if (info->tensor_dims) free(info->tensor_dims);
    if (info->type_name) free(info->type_name);
    if (info->param_types) {
        for (int i = 0; i < info->param_count; i++) {
            if (info->param_types[i]) free_type_info(info->param_types[i]);
        }
        free(info->param_types);
    }
    if (info->return_type) free_type_info(info->return_type);
    free(info);
}

TypeInfo* clone_type_info(const TypeInfo* info) {
    if (!info) return NULL;
    TypeInfo* clone = create_type_info(info->base);
    clone->static_size = info->static_size;
    clone->tensor_ndims = info->tensor_ndims;
    if (info->element_type) clone->element_type = clone_type_info(info->element_type);
    if (info->tensor_dims && info->tensor_ndims > 0) {
        clone->tensor_dims = ALLOCATE(int, info->tensor_ndims);
        memcpy(clone->tensor_dims, info->tensor_dims, sizeof(int) * info->tensor_ndims);
    }
    if (info->type_name) clone->type_name = strdup(info->type_name);
    if (info->param_types && info->param_count > 0) {
        clone->param_count = info->param_count;
        clone->param_types = ALLOCATE(TypeInfo*, info->param_count);
        for (int i = 0; i < info->param_count; i++) {
            clone->param_types[i] = clone_type_info(info->param_types[i]);
        }
    }
    if (info->return_type) clone->return_type = clone_type_info(info->return_type);
    return clone;
}

// ============================================================================
// Type utility functions
// ============================================================================

int type_size_bytes(DataType type) {
    switch (type) {
        case TYPE_I8:    case TYPE_U8:    case TYPE_BOOL:   return 1;
        case TYPE_I16:   case TYPE_U16:   case TYPE_F16:    case TYPE_BF16: return 2;
        case TYPE_I32:   case TYPE_U32:   case TYPE_F32:    case TYPE_CHAR: return 4;
        case TYPE_I64:   case TYPE_U64:   case TYPE_F64:    return 8;
        case TYPE_I128:  case TYPE_U128:  return 16;
        case TYPE_RAWPTR: return 8;  // 64-bit pointer
        case TYPE_FUNC:   return 8;  // Function pointer / closure handle
        case TYPE_DYN:    return 8;  // Opaque erased handle in the current runtime model
        case TYPE_STRING: return 16; // length + pointer
        case TYPE_STRBUF: return 24; // length + capacity + pointer
        default: return 0;  // Compound types depend on contents
    }
}

int type_alignment(DataType type) {
    switch (type) {
        case TYPE_I8:    case TYPE_U8:    case TYPE_BOOL:  return 1;
        case TYPE_I16:   case TYPE_U16:   case TYPE_F16:   case TYPE_BF16: return 2;
        case TYPE_I32:   case TYPE_U32:   case TYPE_F32:   case TYPE_CHAR: return 4;
        case TYPE_I64:   case TYPE_U64:   case TYPE_F64:   return 8;
        case TYPE_I128:  case TYPE_U128:  return 16;
        case TYPE_RAWPTR: case TYPE_PTR: case TYPE_REF: case TYPE_FUNC: case TYPE_DYN: return 8;
        case TYPE_STRING: case TYPE_STRBUF: return 8;
        case TYPE_VEC:   return 16;  // SSE alignment minimum
        default: return 8;
    }
}

bool type_is_integer(DataType type) {
    return (type >= TYPE_I8 && type <= TYPE_U128);
}

bool type_is_unsigned(DataType type) {
    return (type >= TYPE_U8 && type <= TYPE_U128);
}

bool type_is_signed(DataType type) {
    return (type >= TYPE_I8 && type <= TYPE_I128);
}

bool type_is_float(DataType type) {
    return (type >= TYPE_F16 && type <= TYPE_BF16);
}

bool type_is_numeric(DataType type) {
    return type_is_integer(type) || type_is_float(type);
}

bool type_is_primitive(DataType type) {
    return type_is_numeric(type) || type == TYPE_BOOL || type == TYPE_CHAR;
}

bool type_is_simd(DataType type) {
    return type == TYPE_VEC || type == TYPE_MAT;
}

bool type_is_pointer(DataType type) {
    return type == TYPE_PTR || type == TYPE_RAWPTR || type == TYPE_REF ||
           type == TYPE_FUNC || type == TYPE_DYN;
}

bool type_is_collection(DataType type) {
    return type == TYPE_ARRAY || type == TYPE_STATIC_ARRAY || type == TYPE_SLICE;
}

const char* datatype_to_string(DataType type) {
    switch (type) {
        case TYPE_I8:           return "i8";
        case TYPE_I16:          return "i16";
        case TYPE_I32:          return "i32";
        case TYPE_I64:          return "i64";
        case TYPE_I128:         return "i128";
        case TYPE_U8:           return "u8";
        case TYPE_U16:          return "u16";
        case TYPE_U32:          return "u32";
        case TYPE_U64:          return "u64";
        case TYPE_U128:         return "u128";
        case TYPE_F16:          return "f16";
        case TYPE_F32:          return "f32";
        case TYPE_F64:          return "f64";
        case TYPE_BF16:         return "bf16";
        case TYPE_BOOL:         return "bool";
        case TYPE_CHAR:         return "char";
        case TYPE_STRING:       return "string";
        case TYPE_STRBUF:       return "strbuf";
        case TYPE_VOID:         return "void";
        case TYPE_STRUCT:       return "struct";
        case TYPE_ENUM:         return "enum";
        case TYPE_UNION:        return "union";
        case TYPE_CLASS:        return "class";
        case TYPE_ARRAY:        return "array";
        case TYPE_STATIC_ARRAY: return "static_array";
        case TYPE_SLICE:        return "slice";
        case TYPE_PTR:          return "ptr";
        case TYPE_RAWPTR:       return "rawptr";
        case TYPE_REF:          return "ref";
        case TYPE_VEC:          return "vec";
        case TYPE_MAT:          return "mat";
        case TYPE_TENSOR:       return "tensor";
        case TYPE_FUNC:         return "func";
        case TYPE_DYN:          return "dyn";
        case TYPE_GENERIC:      return "generic";
        case TYPE_ERROR:        return "error";
        default:                return "unknown";
    }
}

// ============================================================================
// Struct/Enum/Union statement creators
// ============================================================================

Stmt* create_struct_stmt(char* name, StructField* fields, int field_count, int line, int col) {
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_STRUCT;
    stmt->line = line;
    stmt->column = col;
    stmt->as.struct_stmt.name = name;
    stmt->as.struct_stmt.fields = fields;
    stmt->as.struct_stmt.field_count = field_count;
    stmt->as.struct_stmt.type_params = NULL;
    stmt->as.struct_stmt.type_param_count = 0;
    return stmt;
}

Stmt* create_enum_stmt(char* name, EnumVariant* variants, int variant_count, int line, int col) {
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_ENUM;
    stmt->line = line;
    stmt->column = col;
    stmt->as.enum_stmt.name = name;
    stmt->as.enum_stmt.variants = variants;
    stmt->as.enum_stmt.variant_count = variant_count;
    stmt->as.enum_stmt.type_params = NULL;
    stmt->as.enum_stmt.type_param_count = 0;
    return stmt;
}

Stmt* create_union_stmt(char* name, UnionField* fields, int field_count, int line, int col) {
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_UNION;
    stmt->line = line;
    stmt->column = col;
    stmt->as.union_stmt.name = name;
    stmt->as.union_stmt.fields = fields;
    stmt->as.union_stmt.field_count = field_count;
    return stmt;
}
