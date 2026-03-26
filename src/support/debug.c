/*
 * Casprix Compiler - Debug and Visualization Implementation
 * Copyright (c) 2026 Casprix Project
 */

#include "support/debug.h"
#include <stdlib.h>
#include <string.h>

/* Global debug configuration */
DebugConfig g_debug_config = {
    .dump_tokens = false,
    .dump_ast = false,
    .dump_symbols = false,
    .dump_ir = false,
    .step_by_step = false,
    .verbose = false,
    .output = NULL,
    .indent_size = 2
};

/* ----------------------------------------------------------------------------
 * Token Type Names
 * --------------------------------------------------------------------------*/

static const char* token_names[] = {
    [TOKEN_INTEGER] = "INTEGER",
    [TOKEN_FLOAT] = "FLOAT",
    [TOKEN_STRING] = "STRING",
    [TOKEN_TRUE] = "TRUE",
    [TOKEN_FALSE] = "FALSE",
    [TOKEN_IDENTIFIER] = "IDENTIFIER",
    [TOKEN_INT_TYPE] = "INT_TYPE",
    [TOKEN_FLOAT_TYPE] = "FLOAT_TYPE",
    [TOKEN_STRING_TYPE] = "STRING_TYPE",
    [TOKEN_BOOL_TYPE] = "BOOL_TYPE",
    [TOKEN_VOID_TYPE] = "VOID_TYPE",
    [TOKEN_PRINT] = "PRINT",
    [TOKEN_IF] = "IF",
    [TOKEN_ELIF] = "ELIF",
    [TOKEN_ELSE] = "ELSE",
    [TOKEN_FOR] = "FOR",
    [TOKEN_WHILE] = "WHILE",
    [TOKEN_FUNC] = "FUNC",
    [TOKEN_RETURN] = "RETURN",
    [TOKEN_BREAK] = "BREAK",
    [TOKEN_CONTINUE] = "CONTINUE",
    [TOKEN_INCLUDE] = "INCLUDE",
    [TOKEN_IMPORT] = "IMPORT",
    [TOKEN_CLASS] = "CLASS",
    [TOKEN_EXTENDS] = "EXTENDS",
    [TOKEN_THIS] = "THIS",
    [TOKEN_NEW] = "NEW",
    [TOKEN_STATIC] = "STATIC",
    [TOKEN_SUPER] = "SUPER",
    [TOKEN_ABSTRACT] = "ABSTRACT",
    [TOKEN_PRIVATE] = "PRIVATE",
    [TOKEN_PUBLIC] = "PUBLIC",
    [TOKEN_PROTECTED] = "PROTECTED",
    [TOKEN_EXTERN] = "EXTERN",
    [TOKEN_CONST] = "CONST",
    [TOKEN_MUT] = "MUT",
    [TOKEN_COPY] = "COPY",
    [TOKEN_WHERE] = "WHERE",
    [TOKEN_PLUS] = "PLUS",
    [TOKEN_MINUS] = "MINUS",
    [TOKEN_STAR] = "STAR",
    [TOKEN_SLASH] = "SLASH",
    [TOKEN_PERCENT] = "PERCENT",
    [TOKEN_ASSIGN] = "ASSIGN",
    [TOKEN_EQUAL] = "EQUAL",
    [TOKEN_NOT_EQUAL] = "NOT_EQUAL",
    [TOKEN_LESS] = "LESS",
    [TOKEN_LESS_EQUAL] = "LESS_EQUAL",
    [TOKEN_GREATER] = "GREATER",
    [TOKEN_GREATER_EQUAL] = "GREATER_EQUAL",
    [TOKEN_AND] = "AND",
    [TOKEN_OR] = "OR",
    [TOKEN_NOT] = "NOT",
    [TOKEN_LPAREN] = "LPAREN",
    [TOKEN_RPAREN] = "RPAREN",
    [TOKEN_LBRACE] = "LBRACE",
    [TOKEN_RBRACE] = "RBRACE",
    [TOKEN_LBRACKET] = "LBRACKET",
    [TOKEN_RBRACKET] = "RBRACKET",
    [TOKEN_SEMICOLON] = "SEMICOLON",
    [TOKEN_COLON] = "COLON",
    [TOKEN_COMMA] = "COMMA",
    [TOKEN_ARROW] = "ARROW",
    [TOKEN_DOT] = "DOT",
    [TOKEN_PIPE] = "PIPE",
    [TOKEN_FAT_ARROW] = "FAT_ARROW",
    [TOKEN_QUESTION] = "QUESTION",
    [TOKEN_ERROR] = "ERROR",
    [TOKEN_EOF] = "EOF"
};

/* ----------------------------------------------------------------------------
 * Data Type Names
 * --------------------------------------------------------------------------*/

static const char* type_names[] = {
    [TYPE_INT] = "Int",
    [TYPE_FLOAT] = "Float",
    [TYPE_STRING] = "String",
    [TYPE_BOOL] = "Bool",
    [TYPE_VOID] = "Void",
    [TYPE_CLASS] = "Class",
    [TYPE_FUNC] = "Func",
    [TYPE_GENERIC] = "Generic",
    [TYPE_ERROR] = "Error"
};

/* ----------------------------------------------------------------------------
 * Initialization
 * --------------------------------------------------------------------------*/

void debug_init(void) {
    g_debug_config.output = stdout;
}

/* ----------------------------------------------------------------------------
 * Utility Functions
 * --------------------------------------------------------------------------*/

void debug_indent(int level) {
    FILE* out = g_debug_config.output ? g_debug_config.output : stdout;
    for (int i = 0; i < level * g_debug_config.indent_size; i++) {
        fputc(' ', out);
    }
}

void debug_line(char c, int width) {
    FILE* out = g_debug_config.output ? g_debug_config.output : stdout;
    for (int i = 0; i < width; i++) {
        fputc(c, out);
    }
    fputc('\n', out);
}

void debug_header(const char* title) {
    FILE* out = g_debug_config.output ? g_debug_config.output : stdout;
    int len = (int)strlen(title);
    int width = len + 4;

    fprintf(out, "\n");
    debug_line('=', width);
    fprintf(out, "| %s |\n", title);
    debug_line('=', width);
}

/* ----------------------------------------------------------------------------
 * Token Visualization
 * --------------------------------------------------------------------------*/

const char* debug_token_type_name(TokenType type) {
    if (type >= 0 && type < (int)(sizeof(token_names) / sizeof(token_names[0]))) {
        return token_names[type] ? token_names[type] : "UNKNOWN";
    }
    return "UNKNOWN";
}

void debug_print_token(Token* token) {
    FILE* out = g_debug_config.output ? g_debug_config.output : stdout;

    fprintf(out, "  [%3d:%2d] %-15s ",
            token->line, token->column,
            debug_token_type_name(token->type));

    /* Print lexeme */
    if (token->length > 0 && token->start) {
        fprintf(out, "'%.*s'", token->length, token->start);
    }

    /* Print literal value if applicable */
    switch (token->type) {
        case TOKEN_INTEGER:
            fprintf(out, " = %lld", (long long)token->literal.int_value);
            break;
        case TOKEN_FLOAT:
            fprintf(out, " = %f", token->literal.float_value);
            break;
        case TOKEN_STRING:
            if (token->literal.string_value) {
                fprintf(out, " = \"%s\"", token->literal.string_value);
            }
            break;
        case TOKEN_TRUE:
            fprintf(out, " = true");
            break;
        case TOKEN_FALSE:
            fprintf(out, " = false");
            break;
        default:
            break;
    }

    fprintf(out, "\n");
}

void debug_dump_tokens(const char* source) {
    FILE* out = g_debug_config.output ? g_debug_config.output : stdout;

    debug_header("LEXER OUTPUT - Token Stream");
    fprintf(out, "\n");
    fprintf(out, "  [Ln:Col] Type            Lexeme\n");
    debug_line('-', 60);

    Lexer lexer;
    init_lexer(&lexer, source);

    int token_count = 0;
    Token token;

    do {
        token = scan_token(&lexer);
        debug_print_token(&token);
        token_count++;
    } while (token.type != TOKEN_EOF && token.type != TOKEN_ERROR);

    debug_line('-', 60);
    fprintf(out, "  Total tokens: %d\n\n", token_count);
}

/* ----------------------------------------------------------------------------
 * AST Visualization
 * --------------------------------------------------------------------------*/

const char* debug_data_type_name(DataType type) {
    if (type >= 0 && type < (int)(sizeof(type_names) / sizeof(type_names[0]))) {
        return type_names[type] ? type_names[type] : "Unknown";
    }
    return "Unknown";
}

static const char* expr_type_name(ExprType type) {
    switch (type) {
        case EXPR_BINARY: return "Binary";
        case EXPR_UNARY: return "Unary";
        case EXPR_LITERAL: return "Literal";
        case EXPR_VARIABLE: return "Variable";
        case EXPR_CALL: return "Call";
        case EXPR_MEMBER_ACCESS: return "MemberAccess";
        case EXPR_THIS: return "This";
        case EXPR_SUPER: return "Super";
        case EXPR_NEW: return "New";
        case EXPR_STATIC_ACCESS: return "StaticAccess";
        case EXPR_INDEX: return "Index";
        case EXPR_LAMBDA: return "Lambda";
        case EXPR_GENERIC_INST: return "GenericInst";
        default: return "Unknown";
    }
}

static const char* stmt_type_name(StmtType type) {
    switch (type) {
        case STMT_PRINT: return "Print";
        case STMT_DECLARATION: return "Declaration";
        case STMT_ASSIGNMENT: return "Assignment";
        case STMT_IF: return "If";
        case STMT_WHILE: return "While";
        case STMT_FOR: return "For";
        case STMT_FUNCTION: return "Function";
        case STMT_RETURN: return "Return";
        case STMT_BLOCK: return "Block";
        case STMT_EXPR: return "Expression";
        case STMT_INCLUDE: return "Include";
        case STMT_CLASS: return "Class";
        case STMT_BREAK: return "Break";
        case STMT_CONTINUE: return "Continue";
        case STMT_EXTERN: return "Extern";
        default: return "Unknown";
    }
}

static const char* operator_name(TokenType op) {
    switch (op) {
        case TOKEN_PLUS: return "+";
        case TOKEN_MINUS: return "-";
        case TOKEN_STAR: return "*";
        case TOKEN_SLASH: return "/";
        case TOKEN_PERCENT: return "%";
        case TOKEN_EQUAL: return "==";
        case TOKEN_NOT_EQUAL: return "!=";
        case TOKEN_LESS: return "<";
        case TOKEN_LESS_EQUAL: return "<=";
        case TOKEN_GREATER: return ">";
        case TOKEN_GREATER_EQUAL: return ">=";
        case TOKEN_AND: return "&&";
        case TOKEN_OR: return "||";
        case TOKEN_NOT: return "!";
        default: return "?";
    }
}

void debug_print_expr(Expr* expr, int indent) {
    FILE* out = g_debug_config.output ? g_debug_config.output : stdout;

    if (!expr) {
        debug_indent(indent);
        fprintf(out, "(null)\n");
        return;
    }

    debug_indent(indent);
    fprintf(out, "%s", expr_type_name(expr->type));

    if (expr->data_type != TYPE_ERROR) {
        fprintf(out, " : %s", debug_data_type_name(expr->data_type));
    }

    switch (expr->type) {
        case EXPR_LITERAL:
            /* LiteralExpr.type is DataType (TYPE_INT, etc.) */
            switch (expr->as.literal.type) {
                case TYPE_INT:
                    fprintf(out, " = %lld\n", (long long)expr->as.literal.value.int_value);
                    break;
                case TYPE_FLOAT:
                    fprintf(out, " = %f\n", expr->as.literal.value.float_value);
                    break;
                case TYPE_STRING:
                    fprintf(out, " = \"%s\"\n", expr->as.literal.value.string_value);
                    break;
                case TYPE_BOOL:
                    fprintf(out, " = %s\n", expr->as.literal.value.bool_value ? "true" : "false");
                    break;
                default:
                    fprintf(out, "\n");
            }
            break;

        case EXPR_VARIABLE:
            fprintf(out, " '%s'\n", expr->as.variable.name);
            break;

        case EXPR_BINARY:
            fprintf(out, " [%s]\n", operator_name(expr->as.binary.operator));
            debug_indent(indent + 1);
            fprintf(out, "left:\n");
            debug_print_expr(expr->as.binary.left, indent + 2);
            debug_indent(indent + 1);
            fprintf(out, "right:\n");
            debug_print_expr(expr->as.binary.right, indent + 2);
            break;

        case EXPR_UNARY:
            fprintf(out, " [%s]\n", operator_name(expr->as.unary.operator));
            debug_indent(indent + 1);
            fprintf(out, "operand:\n");
            debug_print_expr(expr->as.unary.operand, indent + 2);
            break;

        case EXPR_CALL:
            if (expr->as.call.name) {
                fprintf(out, " '%s'\n", expr->as.call.name);
            } else {
                fprintf(out, "\n");
                debug_indent(indent + 1);
                fprintf(out, "callee:\n");
                debug_print_expr(expr->as.call.callee, indent + 2);
            }
            if (expr->as.call.arg_count > 0) {
                debug_indent(indent + 1);
                fprintf(out, "arguments: (%d)\n", expr->as.call.arg_count);
                for (int i = 0; i < expr->as.call.arg_count; i++) {
                    debug_print_expr(expr->as.call.arguments[i], indent + 2);
                }
            }
            break;

        case EXPR_MEMBER_ACCESS:
            /* MemberAccessExpr is accessed via expr->as.member */
            fprintf(out, " .%s\n", expr->as.member.member_name);
            debug_indent(indent + 1);
            fprintf(out, "object:\n");
            debug_print_expr(expr->as.member.object, indent + 2);
            if (expr->as.member.is_method_call && expr->as.member.arg_count > 0) {
                debug_indent(indent + 1);
                fprintf(out, "arguments: (%d)\n", expr->as.member.arg_count);
                for (int i = 0; i < expr->as.member.arg_count; i++) {
                    debug_print_expr(expr->as.member.arguments[i], indent + 2);
                }
            }
            break;

        case EXPR_THIS:
            fprintf(out, "\n");
            break;

        case EXPR_SUPER:
            /* SuperExpr has member_name not method_name */
            fprintf(out, " .%s\n", expr->as.super_expr.member_name);
            if (expr->as.super_expr.is_method_call && expr->as.super_expr.arg_count > 0) {
                debug_indent(indent + 1);
                fprintf(out, "arguments: (%d)\n", expr->as.super_expr.arg_count);
                for (int i = 0; i < expr->as.super_expr.arg_count; i++) {
                    debug_print_expr(expr->as.super_expr.arguments[i], indent + 2);
                }
            }
            break;

        case EXPR_NEW:
            fprintf(out, " %s\n", expr->as.new_expr.class_name);
            if (expr->as.new_expr.arg_count > 0) {
                debug_indent(indent + 1);
                fprintf(out, "constructor args: (%d)\n", expr->as.new_expr.arg_count);
                for (int i = 0; i < expr->as.new_expr.arg_count; i++) {
                    debug_print_expr(expr->as.new_expr.arguments[i], indent + 2);
                }
            }
            break;

        case EXPR_STATIC_ACCESS:
            fprintf(out, " %s::%s\n",
                    expr->as.static_access.class_name,
                    expr->as.static_access.member_name);
            if (expr->as.static_access.is_method_call && expr->as.static_access.arg_count > 0) {
                debug_indent(indent + 1);
                fprintf(out, "arguments: (%d)\n", expr->as.static_access.arg_count);
                for (int i = 0; i < expr->as.static_access.arg_count; i++) {
                    debug_print_expr(expr->as.static_access.arguments[i], indent + 2);
                }
            }
            break;

        case EXPR_INDEX:
            fprintf(out, "\n");
            debug_indent(indent + 1);
            fprintf(out, "array:\n");
            debug_print_expr(expr->as.index.array, indent + 2);
            debug_indent(indent + 1);
            fprintf(out, "index:\n");
            debug_print_expr(expr->as.index.index, indent + 2);
            break;

        case EXPR_LAMBDA:
            fprintf(out, " (%d params)\n", expr->as.lambda.param_count);
            if (expr->as.lambda.param_count > 0) {
                debug_indent(indent + 1);
                fprintf(out, "params:\n");
                for (int i = 0; i < expr->as.lambda.param_count; i++) {
                    debug_indent(indent + 2);
                    fprintf(out, "%s : %s\n",
                            expr->as.lambda.parameters[i].name,
                            debug_data_type_name(expr->as.lambda.parameters[i].type));
                }
            }
            if (expr->as.lambda.is_expression && expr->as.lambda.expr_body) {
                debug_indent(indent + 1);
                fprintf(out, "body (expr):\n");
                debug_print_expr(expr->as.lambda.expr_body, indent + 2);
            }
            break;

        case EXPR_GENERIC_INST:
            fprintf(out, " %s<", expr->as.generic_inst.base_name);
            for (int i = 0; i < expr->as.generic_inst.type_arg_count; i++) {
                if (i > 0) fprintf(out, ", ");
                fprintf(out, "%s", debug_data_type_name(expr->as.generic_inst.type_args[i]));
            }
            fprintf(out, ">\n");
            break;

        default:
            fprintf(out, "\n");
    }
}

void debug_print_stmt(Stmt* stmt, int indent) {
    FILE* out = g_debug_config.output ? g_debug_config.output : stdout;

    if (!stmt) {
        debug_indent(indent);
        fprintf(out, "(null statement)\n");
        return;
    }

    debug_indent(indent);
    fprintf(out, "%s", stmt_type_name(stmt->type));

    switch (stmt->type) {
        case STMT_DECLARATION:
            /* DeclarationStmt accessed via stmt->as.declaration */
            fprintf(out, " '%s' : %s",
                    stmt->as.declaration.name,
                    debug_data_type_name(stmt->as.declaration.type));
            if (stmt->as.declaration.class_name) {
                fprintf(out, "<%s>", stmt->as.declaration.class_name);
            }
            fprintf(out, "\n");
            if (stmt->as.declaration.initializer) {
                debug_indent(indent + 1);
                fprintf(out, "init:\n");
                debug_print_expr(stmt->as.declaration.initializer, indent + 2);
            }
            break;

        case STMT_ASSIGNMENT:
            fprintf(out, "\n");
            debug_indent(indent + 1);
            fprintf(out, "target:\n");
            debug_print_expr(stmt->as.assignment.target, indent + 2);
            debug_indent(indent + 1);
            fprintf(out, "value:\n");
            debug_print_expr(stmt->as.assignment.value, indent + 2);
            break;

        case STMT_PRINT:
            fprintf(out, "\n");
            debug_print_expr(stmt->as.print.expression, indent + 1);
            break;

        case STMT_IF:
            fprintf(out, "\n");
            debug_indent(indent + 1);
            fprintf(out, "condition:\n");
            debug_print_expr(stmt->as.if_stmt.condition, indent + 2);
            debug_indent(indent + 1);
            fprintf(out, "then:\n");
            debug_print_stmt(stmt->as.if_stmt.then_branch, indent + 2);
            if (stmt->as.if_stmt.else_branch) {
                debug_indent(indent + 1);
                fprintf(out, "else:\n");
                debug_print_stmt(stmt->as.if_stmt.else_branch, indent + 2);
            }
            break;

        case STMT_WHILE:
            fprintf(out, "\n");
            debug_indent(indent + 1);
            fprintf(out, "condition:\n");
            debug_print_expr(stmt->as.while_stmt.condition, indent + 2);
            debug_indent(indent + 1);
            fprintf(out, "body:\n");
            debug_print_stmt(stmt->as.while_stmt.body, indent + 2);
            break;

        case STMT_FOR:
            /* ForStmt has: variable, var_type, initializer, condition, increment, body */
            fprintf(out, " '%s' : %s\n",
                    stmt->as.for_stmt.variable,
                    debug_data_type_name(stmt->as.for_stmt.var_type));
            if (stmt->as.for_stmt.initializer) {
                debug_indent(indent + 1);
                fprintf(out, "init:\n");
                debug_print_expr(stmt->as.for_stmt.initializer, indent + 2);
            }
            if (stmt->as.for_stmt.condition) {
                debug_indent(indent + 1);
                fprintf(out, "condition:\n");
                debug_print_expr(stmt->as.for_stmt.condition, indent + 2);
            }
            if (stmt->as.for_stmt.increment) {
                debug_indent(indent + 1);
                fprintf(out, "increment:\n");
                debug_print_stmt(stmt->as.for_stmt.increment, indent + 2);
            }
            debug_indent(indent + 1);
            fprintf(out, "body:\n");
            debug_print_stmt(stmt->as.for_stmt.body, indent + 2);
            break;

        case STMT_FUNCTION:
            fprintf(out, " '%s' -> %s",
                    stmt->as.function.name,
                    debug_data_type_name(stmt->as.function.return_type));
            fprintf(out, "\n");

            if (stmt->as.function.type_param_count > 0) {
                debug_indent(indent + 1);
                fprintf(out, "type params: (%d)\n", stmt->as.function.type_param_count);
                for (int i = 0; i < stmt->as.function.type_param_count; i++) {
                    debug_indent(indent + 2);
                    fprintf(out, "%s", stmt->as.function.type_params[i].name);
                    if (stmt->as.function.type_params[i].constraint) {
                        fprintf(out, " : %s", stmt->as.function.type_params[i].constraint);
                    }
                    fprintf(out, "\n");
                }
            }

            if (stmt->as.function.param_count > 0) {
                debug_indent(indent + 1);
                fprintf(out, "params: (%d)\n", stmt->as.function.param_count);
                for (int i = 0; i < stmt->as.function.param_count; i++) {
                    debug_indent(indent + 2);
                    fprintf(out, "%s : %s\n",
                            stmt->as.function.parameters[i].name,
                            debug_data_type_name(stmt->as.function.parameters[i].type));
                }
            }

            debug_indent(indent + 1);
            fprintf(out, "body:\n");
            debug_print_stmt(stmt->as.function.body, indent + 2);
            break;

        case STMT_RETURN:
            fprintf(out, "\n");
            if (stmt->as.return_stmt.value) {
                debug_print_expr(stmt->as.return_stmt.value, indent + 1);
            }
            break;

        case STMT_BLOCK:
            /* BlockStmt has stmt_count not count */
            fprintf(out, " (%d statements)\n", stmt->as.block.stmt_count);
            for (int i = 0; i < stmt->as.block.stmt_count; i++) {
                debug_print_stmt(stmt->as.block.statements[i], indent + 1);
            }
            break;

        case STMT_EXPR:
            /* ExprStmt accessed via stmt->as.expr_stmt */
            fprintf(out, "\n");
            debug_print_expr(stmt->as.expr_stmt.expression, indent + 1);
            break;

        case STMT_CLASS:
            fprintf(out, " '%s'", stmt->as.class_stmt.name);
            if (stmt->as.class_stmt.parent_name) {
                fprintf(out, " extends %s", stmt->as.class_stmt.parent_name);
            }
            if (stmt->as.class_stmt.is_abstract) {
                fprintf(out, " [abstract]");
            }
            fprintf(out, "\n");

            if (stmt->as.class_stmt.type_param_count > 0) {
                debug_indent(indent + 1);
                fprintf(out, "type params: (%d)\n", stmt->as.class_stmt.type_param_count);
                for (int i = 0; i < stmt->as.class_stmt.type_param_count; i++) {
                    debug_indent(indent + 2);
                    fprintf(out, "%s\n", stmt->as.class_stmt.type_params[i].name);
                }
            }

            if (stmt->as.class_stmt.field_count > 0) {
                debug_indent(indent + 1);
                fprintf(out, "fields: (%d)\n", stmt->as.class_stmt.field_count);
                for (int i = 0; i < stmt->as.class_stmt.field_count; i++) {
                    debug_indent(indent + 2);
                    FieldDecl* f = &stmt->as.class_stmt.fields[i];
                    if (f->is_static) fprintf(out, "[static] ");
                    fprintf(out, "%s : %s",
                            f->name,
                            debug_data_type_name(f->type));
                    if (f->class_name) {
                        fprintf(out, "<%s>", f->class_name);
                    }
                    fprintf(out, "\n");
                }
            }

            if (stmt->as.class_stmt.method_count > 0) {
                debug_indent(indent + 1);
                fprintf(out, "methods: (%d)\n", stmt->as.class_stmt.method_count);
                for (int i = 0; i < stmt->as.class_stmt.method_count; i++) {
                    debug_indent(indent + 2);
                    MethodDecl* m = &stmt->as.class_stmt.methods[i];
                    if (m->is_static) fprintf(out, "[static] ");
                    if (m->is_abstract) fprintf(out, "[abstract] ");
                    if (m->is_constructor) fprintf(out, "[constructor] ");
                    fprintf(out, "%s -> %s (%d params)\n",
                            m->name,
                            debug_data_type_name(m->return_type),
                            m->param_count);
                }
            }
            break;

        case STMT_INCLUDE:
            fprintf(out, " '%s'", stmt->as.include.module_name);
            if (stmt->as.include.is_import) {
                fprintf(out, " [import]");
            }
            fprintf(out, "\n");
            break;

        case STMT_EXTERN:
            fprintf(out, " '%s' -> %s (%d params)\n",
                    stmt->as.extern_stmt.name,
                    debug_data_type_name(stmt->as.extern_stmt.return_type),
                    stmt->as.extern_stmt.param_count);
            break;

        case STMT_BREAK:
        case STMT_CONTINUE:
            fprintf(out, "\n");
            break;

        default:
            fprintf(out, " (unhandled)\n");
    }
}

void debug_dump_ast(Stmt** statements, int count) {
    FILE* out = g_debug_config.output ? g_debug_config.output : stdout;

    debug_header("ABSTRACT SYNTAX TREE");
    fprintf(out, "\n");
    fprintf(out, "Program (%d top-level statements)\n", count);
    debug_line('-', 50);

    for (int i = 0; i < count; i++) {
        fprintf(out, "\n[%d] ", i);
        debug_print_stmt(statements[i], 0);
    }

    fprintf(out, "\n");
    debug_line('-', 50);
    fprintf(out, "End of AST\n\n");
}

/* ----------------------------------------------------------------------------
 * Symbol Table Visualization
 * --------------------------------------------------------------------------*/

static const char* symbol_kind_name(SymbolKind kind) {
    switch (kind) {
        case SYMBOL_VARIABLE: return "Variable";
        case SYMBOL_FUNCTION: return "Function";
        case SYMBOL_PARAMETER: return "Parameter";
        case SYMBOL_CLASS: return "Class";
        default: return "Unknown";
    }
}

static const char* access_modifier_name(AccessModifier access) {
    switch (access) {
        case ACCESS_PUBLIC: return "public";
        case ACCESS_PRIVATE: return "private";
        case ACCESS_PROTECTED: return "protected";
        default: return "unknown";
    }
}

void debug_print_symbol(Symbol* symbol, int indent) {
    FILE* out = g_debug_config.output ? g_debug_config.output : stdout;

    if (!symbol) return;

    debug_indent(indent);
    fprintf(out, "%s '%s' : %s",
            symbol_kind_name(symbol->kind),
            symbol->name,
            debug_data_type_name(symbol->type));

    fprintf(out, " [scope=%d]", symbol->scope_depth);

    if (symbol->kind == SYMBOL_FUNCTION) {
        fprintf(out, " params=%d", symbol->param_count);
        if (symbol->is_extern) {
            fprintf(out, " [extern]");
        }
    }

    fprintf(out, "\n");
}

void debug_print_class(ClassSymbol* cls, int indent) {
    FILE* out = g_debug_config.output ? g_debug_config.output : stdout;

    if (!cls) return;

    debug_indent(indent);
    fprintf(out, "Class '%s'", cls->name);
    /* ClassSymbol has parent_class not parent_name */
    if (cls->parent_class) {
        fprintf(out, " extends '%s'", cls->parent_class);
    }
    if (cls->is_abstract) {
        fprintf(out, " [abstract]");
    }
    fprintf(out, " (size=%d bytes)\n", cls->instance_size);

    /* Fields */
    if (cls->field_count > 0) {
        debug_indent(indent + 1);
        fprintf(out, "Fields (%d):\n", cls->field_count);
        for (int i = 0; i < cls->field_count; i++) {
            debug_indent(indent + 2);
            FieldSymbol* f = &cls->fields[i];
            if (f->is_static) fprintf(out, "[static] ");
            fprintf(out, "(%s) '%s' : %s",
                    access_modifier_name(f->access),
                    f->name,
                    debug_data_type_name(f->type));
            if (f->class_name) {
                fprintf(out, "<%s>", f->class_name);
            }
            fprintf(out, " offset=%d\n", f->offset);
        }
    }

    /* Methods */
    if (cls->method_count > 0) {
        debug_indent(indent + 1);
        fprintf(out, "Methods (%d):\n", cls->method_count);
        for (int i = 0; i < cls->method_count; i++) {
            debug_indent(indent + 2);
            MethodSymbol* m = &cls->methods[i];
            fprintf(out, "(%s) ", access_modifier_name(m->access));
            if (m->is_static) fprintf(out, "[static] ");
            if (m->is_abstract) fprintf(out, "[abstract] ");
            if (m->is_constructor) fprintf(out, "[constructor] ");
            fprintf(out, "'%s' -> %s",
                    m->name,
                    debug_data_type_name(m->return_type));
            fprintf(out, " (%d params) vtable=%d\n",
                    m->param_count,
                    m->vtable_index);
        }
    }
}

void debug_dump_symbols(SymbolTable* table) {
    FILE* out = g_debug_config.output ? g_debug_config.output : stdout;

    if (!table) return;

    debug_header("SYMBOL TABLE");
    fprintf(out, "\n");
    fprintf(out, "Total symbols: %d\n", table->count);
    debug_line('-', 60);

    /* Variables and Functions */
    fprintf(out, "\nVariables and Functions:\n");
    for (int i = 0; i < table->count; i++) {
        if (table->symbols[i]->kind != SYMBOL_CLASS) {
            debug_print_symbol(table->symbols[i], 1);
        }
    }

    /* Classes */
    fprintf(out, "\nClasses:\n");
    for (int i = 0; i < table->count; i++) {
        if (table->symbols[i]->kind == SYMBOL_CLASS && table->symbols[i]->class_info) {
            debug_print_class(table->symbols[i]->class_info, 1);
        }
    }

    fprintf(out, "\n");
    debug_line('-', 60);
    fprintf(out, "End of Symbol Table\n\n");
}

/* ----------------------------------------------------------------------------
 * Phase Helpers
 * --------------------------------------------------------------------------*/

void debug_phase_start(const char* phase_name) {
    FILE* out = g_debug_config.output ? g_debug_config.output : stdout;

    fprintf(out, "\n");
    debug_line('*', 60);
    fprintf(out, "* PHASE: %s\n", phase_name);
    debug_line('*', 60);
}

void debug_phase_end(const char* phase_name) {
    FILE* out = g_debug_config.output ? g_debug_config.output : stdout;
    fprintf(out, "* %s completed\n", phase_name);
}

void debug_step_wait(void) {
    if (g_debug_config.step_by_step) {
        FILE* out = g_debug_config.output ? g_debug_config.output : stdout;
        fprintf(out, "\nPress Enter to continue...");
        fflush(out);
        getchar();
    }
}
