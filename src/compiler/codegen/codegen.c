#include "compiler/codegen/codegen.h"

static void emit_indent(CodeGenerator* gen);
static void emit(CodeGenerator* gen, const char* format, ...);
static void generate_expr(CodeGenerator* gen, Expr* expr);
static void generate_stmt(CodeGenerator* gen, Stmt* stmt);

void init_code_generator(CodeGenerator* gen, FILE* output) {
    gen->output = output;
    gen->indent_level = 0;
    gen->temp_count = 0;
    gen->label_count = 0;
}

static void emit_indent(CodeGenerator* gen) {
    for (int i = 0; i < gen->indent_level; i++) {
        fprintf(gen->output, "    ");
    }
}

static void emit(CodeGenerator* gen, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(gen->output, format, args);
    va_end(args);
}

static const char* type_to_c_type(DataType type) {
    switch (type) {
        case TYPE_INT: return "long long";
        case TYPE_FLOAT: return "double";
        case TYPE_STRING: return "char*";
        case TYPE_BOOL: return "int";
        case TYPE_VOID: return "void";
        default: return "void";
    }
}

static void generate_binary_expr(CodeGenerator* gen, Expr* expr) {
    BinaryExpr* binary = &expr->as.binary;
    
    // Handle string concatenation specially
    if (binary->operator == TOKEN_PLUS && expr->data_type == TYPE_STRING) {
        emit(gen, "str_concat(");
        generate_expr(gen, binary->left);
        emit(gen, ", ");
        generate_expr(gen, binary->right);
        emit(gen, ")");
        return;
    }
    
    emit(gen, "(");
    generate_expr(gen, binary->left);
    
    switch (binary->operator) {
        case TOKEN_PLUS:
            emit(gen, " + ");
            break;
        case TOKEN_MINUS: emit(gen, " - "); break;
        case TOKEN_STAR: emit(gen, " * "); break;
        case TOKEN_SLASH: emit(gen, " / "); break;
        case TOKEN_LESS: emit(gen, " < "); break;
        case TOKEN_LESS_EQUAL: emit(gen, " <= "); break;
        case TOKEN_GREATER: emit(gen, " > "); break;
        case TOKEN_GREATER_EQUAL: emit(gen, " >= "); break;
        case TOKEN_EQUAL: emit(gen, " == "); break;
        case TOKEN_NOT_EQUAL: emit(gen, " != "); break;
        case TOKEN_AND: emit(gen, " && "); break;
        case TOKEN_OR: emit(gen, " || "); break;
        default: emit(gen, " ? "); break;
    }
    
    generate_expr(gen, binary->right);
    emit(gen, ")");
}

static void generate_unary_expr(CodeGenerator* gen, Expr* expr) {
    UnaryExpr* unary = &expr->as.unary;
    
    switch (unary->operator) {
        case TOKEN_MINUS: emit(gen, "-"); break;
        case TOKEN_NOT: emit(gen, "!"); break;
        default: break;
    }
    
    emit(gen, "(");
    generate_expr(gen, unary->operand);
    emit(gen, ")");
}

static void generate_literal_expr(CodeGenerator* gen, Expr* expr) {
    LiteralExpr* literal = &expr->as.literal;
    
    switch (literal->type) {
        case TYPE_INT:
            emit(gen, "%lldLL", literal->value.int_value);
            break;
        case TYPE_FLOAT:
            emit(gen, "%f", literal->value.float_value);
            break;
        case TYPE_STRING:
            emit(gen, "\"%s\"", literal->value.string_value);
            break;
        case TYPE_BOOL:
            emit(gen, "%d", literal->value.bool_value ? 1 : 0);
            break;
        default:
            break;
    }
}

static void generate_variable_expr(CodeGenerator* gen, Expr* expr) {
    emit(gen, "%s", expr->as.variable.name);
}

static void generate_call_expr(CodeGenerator* gen, Expr* expr) {
    CallExpr* call = &expr->as.call;
    
    if (call->callee) {
        generate_expr(gen, call->callee);
    } else if (call->name) {
        emit(gen, "%s", call->name);
    } else {
        emit(gen, "/* invalid call */");
    }
    emit(gen, "(");
    
    for (int i = 0; i < call->arg_count; i++) {
        if (i > 0) emit(gen, ", ");
        generate_expr(gen, call->arguments[i]);
    }
    
    emit(gen, ")");
}

static void generate_expr(CodeGenerator* gen, Expr* expr) {
    if (!expr) return;
    
    switch (expr->type) {
        case EXPR_BINARY:
            generate_binary_expr(gen, expr);
            break;
        case EXPR_UNARY:
            generate_unary_expr(gen, expr);
            break;
        case EXPR_LITERAL:
            generate_literal_expr(gen, expr);
            break;
        case EXPR_VARIABLE:
            generate_variable_expr(gen, expr);
            break;
        case EXPR_CALL:
            generate_call_expr(gen, expr);
            break;
    }
}

static void generate_declaration_stmt(CodeGenerator* gen, Stmt* stmt) {
    DeclarationStmt* decl = &stmt->as.declaration;
    
    emit_indent(gen);
    emit(gen, "%s %s", type_to_c_type(decl->type), decl->name);
    
    if (decl->initializer) {
        emit(gen, " = ");
        generate_expr(gen, decl->initializer);
    }
    
    emit(gen, ";\n");
}

static void generate_assignment_stmt(CodeGenerator* gen, Stmt* stmt) {
    AssignmentStmt* assign = &stmt->as.assignment;

    emit_indent(gen);
    // Generate target expression (left side of assignment)
    if (assign->target) {
        generate_expr(gen, assign->target);
    }
    emit(gen, " = ");
    generate_expr(gen, assign->value);
    emit(gen, ";\n");
}

static void generate_print_stmt(CodeGenerator* gen, Stmt* stmt) {
    PrintStmt* print = &stmt->as.print;
    
    emit_indent(gen);
    
    DataType type = print->expression->data_type;
    
    switch (type) {
        case TYPE_INT:
            // Use portable format for long long
#ifdef _WIN32
            emit(gen, "printf(\"%%I64d\\n\", (long long)(");
#else
            emit(gen, "printf(\"%%lld\\n\", (long long)(");
#endif
            generate_expr(gen, print->expression);
            emit(gen, "));\n");
            break;
        case TYPE_FLOAT:
            emit(gen, "printf(\"%%f\\n\", (double)(");
            generate_expr(gen, print->expression);
            emit(gen, "));\n");
            break;
        case TYPE_STRING:
            emit(gen, "printf(\"%%s\\n\", ");
            generate_expr(gen, print->expression);
            emit(gen, ");\n");
            break;
        case TYPE_BOOL:
            emit(gen, "printf(\"%%s\\n\", (");
            generate_expr(gen, print->expression);
            emit(gen, ") ? \"True\" : \"False\");\n");
            break;
        default:
            emit(gen, "printf(\"<unknown>\\n\");\n");
            break;
    }
}

static void generate_if_stmt(CodeGenerator* gen, Stmt* stmt) {
    IfStmt* if_stmt = &stmt->as.if_stmt;
    
    emit_indent(gen);
    emit(gen, "if (");
    generate_expr(gen, if_stmt->condition);
    emit(gen, ") ");
    
    if (if_stmt->then_branch->type == STMT_BLOCK) {
        emit(gen, "{\n");
        gen->indent_level++;
        
        BlockStmt* block = &if_stmt->then_branch->as.block;
        for (int i = 0; i < block->stmt_count; i++) {
            generate_stmt(gen, block->statements[i]);
        }
        
        gen->indent_level--;
        emit_indent(gen);
        emit(gen, "}");
    } else {
        emit(gen, "{\n");
        gen->indent_level++;
        generate_stmt(gen, if_stmt->then_branch);
        gen->indent_level--;
        emit_indent(gen);
        emit(gen, "}");
    }
    
    if (if_stmt->else_branch) {
        emit(gen, " else ");
        
        if (if_stmt->else_branch->type == STMT_BLOCK) {
            emit(gen, "{\n");
            gen->indent_level++;
            
            BlockStmt* block = &if_stmt->else_branch->as.block;
            for (int i = 0; i < block->stmt_count; i++) {
                generate_stmt(gen, block->statements[i]);
            }
            
            gen->indent_level--;
            emit_indent(gen);
            emit(gen, "}\n");
        } else {
            emit(gen, "{\n");
            gen->indent_level++;
            generate_stmt(gen, if_stmt->else_branch);
            gen->indent_level--;
            emit_indent(gen);
            emit(gen, "}\n");
        }
    } else {
        emit(gen, "\n");
    }
}

static void generate_for_stmt(CodeGenerator* gen, Stmt* stmt) {
    ForStmt* for_stmt = &stmt->as.for_stmt;
    
    emit_indent(gen);
    emit(gen, "{\n");
    gen->indent_level++;
    
    // Declaration
    emit_indent(gen);
    emit(gen, "%s %s = ", type_to_c_type(for_stmt->var_type), for_stmt->variable);
    generate_expr(gen, for_stmt->initializer);
    emit(gen, ";\n");
    
    // Loop
    emit_indent(gen);
    emit(gen, "while (");
    generate_expr(gen, for_stmt->condition);
    emit(gen, ") {\n");
    gen->indent_level++;
    
    // Body
    generate_stmt(gen, for_stmt->body);
    
    // Increment
    generate_stmt(gen, for_stmt->increment);
    
    gen->indent_level--;
    emit_indent(gen);
    emit(gen, "}\n");
    
    gen->indent_level--;
    emit_indent(gen);
    emit(gen, "}\n");
}

static void generate_while_stmt(CodeGenerator* gen, Stmt* stmt) {
    WhileStmt* while_stmt = &stmt->as.while_stmt;
    
    emit_indent(gen);
    emit(gen, "while (");
    generate_expr(gen, while_stmt->condition);
    emit(gen, ") {\n");
    
    gen->indent_level++;
    generate_stmt(gen, while_stmt->body);
    gen->indent_level--;
    
    emit_indent(gen);
    emit(gen, "}\n");
}

static void generate_function_stmt(CodeGenerator* gen, Stmt* stmt) {
    FunctionStmt* func = &stmt->as.function;
    
    emit_indent(gen);
    emit(gen, "%s %s(", type_to_c_type(func->return_type), func->name);
    
    for (int i = 0; i < func->param_count; i++) {
        if (i > 0) emit(gen, ", ");
        emit(gen, "%s %s", type_to_c_type(func->parameters[i].type),
            func->parameters[i].name);
    }
    
    emit(gen, ") {\n");
    
    gen->indent_level++;
    generate_stmt(gen, func->body);
    gen->indent_level--;
    
    emit_indent(gen);
    emit(gen, "}\n\n");
}

static void generate_return_stmt(CodeGenerator* gen, Stmt* stmt) {
    ReturnStmt* ret = &stmt->as.return_stmt;
    
    emit_indent(gen);
    emit(gen, "return");
    
    if (ret->value) {
        emit(gen, " ");
        generate_expr(gen, ret->value);
    }
    
    emit(gen, ";\n");
}

static void generate_block_stmt(CodeGenerator* gen, Stmt* stmt) {
    BlockStmt* block = &stmt->as.block;
    
    for (int i = 0; i < block->stmt_count; i++) {
        generate_stmt(gen, block->statements[i]);
    }
}

static void generate_stmt(CodeGenerator* gen, Stmt* stmt) {
    if (!stmt) return;
    
    switch (stmt->type) {
        case STMT_CONST_DECL:  /* fall through — same codegen as declaration */
        case STMT_DECLARATION:
            generate_declaration_stmt(gen, stmt);
            break;
        case STMT_ASSIGNMENT:
            generate_assignment_stmt(gen, stmt);
            break;
        case STMT_PRINT:
            generate_print_stmt(gen, stmt);
            break;
        case STMT_IF:
            generate_if_stmt(gen, stmt);
            break;
        case STMT_FOR:
            generate_for_stmt(gen, stmt);
            break;
        case STMT_WHILE:
            generate_while_stmt(gen, stmt);
            break;
        case STMT_FUNCTION:
            generate_function_stmt(gen, stmt);
            break;
        case STMT_RETURN:
            generate_return_stmt(gen, stmt);
            break;
        case STMT_EXPR:
            emit_indent(gen);
            generate_expr(gen, stmt->as.expr_stmt.expression);
            emit(gen, ";\n");
            break;
        case STMT_BLOCK:
            generate_block_stmt(gen, stmt);
            break;
    }
}

void generate_code(CodeGenerator* gen, Stmt** statements, int count) {
    // Generate C preamble
    emit(gen, "#include <stdio.h>\n");
    emit(gen, "#include <stdlib.h>\n");
    emit(gen, "#include <string.h>\n");
    emit(gen, "#include <stdbool.h>\n");
    emit(gen, "#include <math.h>\n\n");
    
    // Helper function for string concatenation
    emit(gen, "// String concatenation helper\n");
    emit(gen, "static char* str_concat(const char* s1, const char* s2) {\n");
    emit(gen, "    if (!s1) s1 = \"\";\n");
    emit(gen, "    if (!s2) s2 = \"\";\n");
    emit(gen, "    size_t len1 = strlen(s1);\n");
    emit(gen, "    size_t len2 = strlen(s2);\n");
    emit(gen, "    char* result = (char*)malloc(len1 + len2 + 1);\n");
    emit(gen, "    if (!result) return NULL;\n");
    emit(gen, "    strcpy(result, s1);\n");
    emit(gen, "    strcat(result, s2);\n");
    emit(gen, "    return result;\n");
    emit(gen, "}\n\n");
    
    // Forward declarations for functions
    for (int i = 0; i < count; i++) {
        if (statements[i]->type == STMT_FUNCTION) {
            FunctionStmt* func = &statements[i]->as.function;
            emit(gen, "%s %s(", type_to_c_type(func->return_type), func->name);
            
            for (int j = 0; j < func->param_count; j++) {
                if (j > 0) emit(gen, ", ");
                emit(gen, "%s", type_to_c_type(func->parameters[j].type));
            }
            
            emit(gen, ");\n");
        }
    }
    emit(gen, "\n");
    
    // Generate main function wrapper
    emit(gen, "int main() {\n");
    gen->indent_level++;
    
    // Generate all statements
    for (int i = 0; i < count; i++) {
        if (statements[i]->type == STMT_FUNCTION) {
            // Functions are generated outside main
            continue;
        }
        generate_stmt(gen, statements[i]);
    }
    
    emit_indent(gen);
    emit(gen, "return 0;\n");
    gen->indent_level--;
    emit(gen, "}\n\n");
    
    // Generate function definitions
    for (int i = 0; i < count; i++) {
        if (statements[i]->type == STMT_FUNCTION) {
            generate_stmt(gen, statements[i]);
        }
    }
}
