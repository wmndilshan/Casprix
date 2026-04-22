/*
 * Casprix Compiler - Closure Implementation
 */

#include "compiler/middle/closure.h"
#include "support/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int next_lambda_id = 0;

void init_closure_analyzer(void) {
    next_lambda_id = 0;
}

// Helper: Calculate size of a data type
static int sizeof_type(DataType type) {
    switch (type) {
        case TYPE_INT:    return 8;
        case TYPE_FLOAT:  return 8;
        case TYPE_BOOL:   return 1;
        case TYPE_STRING: return 8;  // Pointer
        case TYPE_CLASS:  return 8;  // Pointer
        case TYPE_FUNC:   return 8;  // Pointer (closure)
        default:          return 8;
    }
}

// Helper: Find all free variables in an expression
static void find_free_vars_expr(Expr* expr, SymbolTable* local_table, SymbolTable* parent_table,
                                CapturedVar** captures, int* capture_count) {
    if (!expr) return;

    switch (expr->type) {
        case EXPR_VARIABLE: {
            const char* var_name = expr->as.variable.name;

            // Check if it's a local variable
            Symbol* local_sym = lookup_symbol(local_table, var_name);
            if (local_sym) {
                // It's local, not a capture
                return;
            }

            // Check if it's in parent scope
            Symbol* parent_sym = lookup_symbol(parent_table, var_name);
            if (parent_sym &&
                (parent_sym->kind == SYMBOL_VARIABLE ||
                 parent_sym->kind == SYMBOL_PARAMETER)) {
                // Check if already captured
                for (int i = 0; i < *capture_count; i++) {
                    if (strcmp((*captures)[i].var_name, var_name) == 0) {
                        return;  // Already captured
                    }
                }

                // Add to captures
                size_t new_size = (*capture_count + 1) * sizeof(CapturedVar);
                CapturedVar* new_caps = realloc(*captures, new_size);
                if (!new_caps) return;
                *captures = new_caps;

                CapturedVar* cap = &(*captures)[*capture_count];
                cap->var_name = strdup(var_name);
                cap->type = parent_sym->type;
                cap->class_name = NULL;  // Will be filled if needed
                cap->mode = CAPTURE_BY_VALUE;  // Default to by-value
                cap->parent_offset = 0;  // Will be computed during code gen
                cap->is_mutable = false;
                (*capture_count)++;
            }
            break;
        }

        case EXPR_BINARY:
            find_free_vars_expr(expr->as.binary.left, local_table, parent_table, captures, capture_count);
            find_free_vars_expr(expr->as.binary.right, local_table, parent_table, captures, capture_count);
            break;

        case EXPR_UNARY:
            find_free_vars_expr(expr->as.unary.operand, local_table, parent_table, captures, capture_count);
            break;

        case EXPR_CALL:
            find_free_vars_expr(expr->as.call.callee, local_table, parent_table,
                                captures, capture_count);
            for (int i = 0; i < expr->as.call.arg_count; i++) {
                find_free_vars_expr(expr->as.call.arguments[i], local_table, parent_table, captures, capture_count);
            }
            break;

        case EXPR_MEMBER_ACCESS:
            find_free_vars_expr(expr->as.member.object, local_table, parent_table, captures, capture_count);
            if (expr->as.member.is_method_call) {
                for (int i = 0; i < expr->as.member.arg_count; i++) {
                    find_free_vars_expr(expr->as.member.arguments[i], local_table, parent_table, captures, capture_count);
                }
            }
            break;

        case EXPR_INDEX:
            find_free_vars_expr(expr->as.index.array, local_table, parent_table, captures, capture_count);
            find_free_vars_expr(expr->as.index.index, local_table, parent_table, captures, capture_count);
            break;

        case EXPR_NEW:
            for (int i = 0; i < expr->as.new_expr.arg_count; i++) {
                find_free_vars_expr(expr->as.new_expr.arguments[i], local_table, parent_table, captures, capture_count);
            }
            break;

        case EXPR_STATIC_ACCESS:
            if (expr->as.static_access.is_method_call) {
                for (int i = 0; i < expr->as.static_access.arg_count; i++) {
                    find_free_vars_expr(expr->as.static_access.arguments[i], local_table, parent_table, captures, capture_count);
                }
            }
            break;

        default:
            break;
    }
}

// Helper: Find free variables in statements
static void find_free_vars_stmt(Stmt* stmt, SymbolTable* local_table, SymbolTable* parent_table,
                                CapturedVar** captures, int* capture_count) {
    if (!stmt) return;

    switch (stmt->type) {
        case STMT_DECLARATION:
        case STMT_CONST_DECL:
            if (stmt->as.declaration.initializer) {
                find_free_vars_expr(stmt->as.declaration.initializer,
                                    local_table, parent_table, captures, capture_count);
            }
            if (stmt->as.declaration.name) {
                add_symbol(local_table, stmt->as.declaration.name,
                           SYMBOL_VARIABLE, stmt->as.declaration.type, 0);
            }
            break;

        case STMT_EXPR:
            find_free_vars_expr(stmt->as.expr_stmt.expression, local_table, parent_table, captures, capture_count);
            break;

        case STMT_ASSIGNMENT:
            find_free_vars_expr(stmt->as.assignment.target, local_table, parent_table, captures, capture_count);
            find_free_vars_expr(stmt->as.assignment.value, local_table, parent_table, captures, capture_count);

            // Mark as mutable if assigning to captured var
            if (stmt->as.assignment.target->type == EXPR_VARIABLE) {
                for (int i = 0; i < *capture_count; i++) {
                    if (strcmp((*captures)[i].var_name, stmt->as.assignment.target->as.variable.name) == 0) {
                        (*captures)[i].is_mutable = true;
                        (*captures)[i].mode = CAPTURE_BY_REFERENCE;
                    }
                }
            }
            break;

        case STMT_RETURN:
            find_free_vars_expr(stmt->as.return_stmt.value, local_table, parent_table, captures, capture_count);
            break;

        case STMT_IF:
            find_free_vars_expr(stmt->as.if_stmt.condition, local_table, parent_table, captures, capture_count);
            find_free_vars_stmt(stmt->as.if_stmt.then_branch, local_table, parent_table, captures, capture_count);
            find_free_vars_stmt(stmt->as.if_stmt.else_branch, local_table, parent_table, captures, capture_count);
            break;

        case STMT_WHILE:
            find_free_vars_expr(stmt->as.while_stmt.condition, local_table, parent_table, captures, capture_count);
            find_free_vars_stmt(stmt->as.while_stmt.body, local_table, parent_table, captures, capture_count);
            break;

        case STMT_FOR:
            find_free_vars_expr(stmt->as.for_stmt.initializer, local_table, parent_table, captures, capture_count);
            find_free_vars_expr(stmt->as.for_stmt.condition, local_table, parent_table, captures, capture_count);
            find_free_vars_stmt(stmt->as.for_stmt.increment, local_table, parent_table, captures, capture_count);
            find_free_vars_stmt(stmt->as.for_stmt.body, local_table, parent_table, captures, capture_count);
            break;

        case STMT_BLOCK:
            for (int i = 0; i < stmt->as.block.stmt_count; i++) {
                find_free_vars_stmt(stmt->as.block.statements[i], local_table, parent_table, captures, capture_count);
            }
            break;

        case STMT_PRINT:
            find_free_vars_expr(stmt->as.print.expression, local_table, parent_table, captures, capture_count);
            break;

        default:
            break;
    }
}

// Analyze closure captures
ClosureMeta* analyze_closure(LambdaExpr* lambda, SymbolTable* symtable, SymbolTable* parent_table) {
    (void)symtable;
    ClosureMeta* meta = calloc(1, sizeof(ClosureMeta));
    meta->lambda_id = next_lambda_id++;

    // Create environment
    meta->environment = calloc(1, sizeof(ClosureEnv));
    CapturedVar* captures = NULL;
    int capture_count = 0;

    // Build local symbol table for lambda parameters
    SymbolTable local_table;
    init_symbol_table(&local_table);
    int scope_depth = 0;

    for (int i = 0; i < lambda->param_count; i++) {
        add_symbol(&local_table, lambda->parameters[i].name,
                   SYMBOL_PARAMETER, lambda->parameters[i].type, scope_depth);
    }

    // Find all free variables
    if (lambda->is_expression) {
        find_free_vars_expr(lambda->expr_body, &local_table, parent_table, &captures, &capture_count);
    } else {
        find_free_vars_stmt(lambda->block_body, &local_table, parent_table, &captures, &capture_count);
    }

    meta->environment->variables = captures;
    meta->environment->count = capture_count;
    meta->needs_heap_alloc = (capture_count > 0);

    // Calculate layout
    layout_closure_env(meta->environment);

    // Generate mangled name
    meta->mangled_name = mangle_lambda_name("lambda", meta->lambda_id);

    free_symbol_table(&local_table);
    return meta;
}

// Calculate closure environment layout
void layout_closure_env(ClosureEnv* env) {
    if (!env) return;
    if (env->count == 0) {
        env->total_size = 0;
        env->alignment = 8;
        return;
    }

    int offset = 16;  // Start after function pointer (8) and env pointer (8)
    int max_align = 8;

    for (int i = 0; i < env->count; i++) {
        CapturedVar* var = &env->variables[i];
        int size = sizeof_type(var->type);
        int align = size < 8 ? size : 8;

        // Align offset
        if (offset % align != 0) {
            offset += align - (offset % align);
        }

        var->env_offset = offset;
        offset += size;

        if (align > max_align) {
            max_align = align;
        }
    }

    // Align total size
    if (offset % max_align != 0) {
        offset += max_align - (offset % max_align);
    }

    env->total_size = offset;
    env->alignment = max_align;
}

// Check if variable is captured
bool is_variable_captured(const char* var_name, ClosureEnv* env) {
    if (!env) return false;

    for (int i = 0; i < env->count; i++) {
        if (strcmp(env->variables[i].var_name, var_name) == 0) {
            return true;
        }
    }
    return false;
}

// Get captured variable info
CapturedVar* get_captured_var(const char* var_name, ClosureEnv* env) {
    if (!env) return NULL;

    for (int i = 0; i < env->count; i++) {
        if (strcmp(env->variables[i].var_name, var_name) == 0) {
            return &env->variables[i];
        }
    }
    return NULL;
}

// Generate mangled name for lambda
char* mangle_lambda_name(const char* context, int lambda_id) {
    size_t len = strlen(context) + 32;
    char* name = malloc(len);
    if (!name) return NULL;
    snprintf(name, len, "_lambda_%s_%d", context, lambda_id);
    return name;
}

// Free closure metadata
void free_closure_meta(ClosureMeta* meta) {
    if (!meta) return;

    if (meta->environment) {
        free_closure_env(meta->environment);
    }
    if (meta->mangled_name) {
        free(meta->mangled_name);
    }
    free(meta);
}

void free_closure_env(ClosureEnv* env) {
    if (!env) return;

    if (env->variables) {
        for (int i = 0; i < env->count; i++) {
            free(env->variables[i].var_name);
            if (env->variables[i].class_name) {
                free(env->variables[i].class_name);
            }
        }
        free(env->variables);
    }
    free(env);
}

// Code generation: Allocate closure on heap
void emit_closure_allocation(ClosureMeta* meta, FILE* out) {
    if (!meta->needs_heap_alloc) {
        // No captures, just allocate function pointer wrapper
        fprintf(out, "    ; Allocate closure (no captures)\n");
        fprintf(out, "    mov rdi, 16           ; Size: func_ptr + env_ptr\n");
        fprintf(out, "    call malloc\n");
        fprintf(out, "    mov [closure_ptr], rax\n");
        return;
    }

    fprintf(out, "    ; Allocate closure with %d bytes for captures\n", meta->environment->total_size);
    fprintf(out, "    mov rdi, %d\n", meta->environment->total_size);
    fprintf(out, "    call malloc\n");
    fprintf(out, "    mov [closure_ptr], rax\n");
}

// Code generation: Setup closure environment
void emit_closure_setup(ClosureMeta* meta, FILE* out) {
    fprintf(out, "    ; Setup closure for %s\n", meta->mangled_name);
    fprintf(out, "    mov rax, [closure_ptr]\n");
    fprintf(out, "    lea rdx, [%s]         ; Function pointer\n", meta->mangled_name);
    fprintf(out, "    mov [rax], rdx\n");
    fprintf(out, "    mov [rax+8], rax      ; Environment pointer (self)\n");

    if (!meta->needs_heap_alloc) return;

    // Copy captured variables
    for (int i = 0; i < meta->environment->count; i++) {
        CapturedVar* var = &meta->environment->variables[i];
        fprintf(out, "    ; Capture %s\n", var->var_name);
        fprintf(out, "    mov rdx, [rbp%+d]    ; Load from parent frame\n", var->parent_offset);
        fprintf(out, "    mov [rax%+d], rdx    ; Store in environment\n", var->env_offset);
    }
}

// Code generation: Call closure
void emit_closure_call(Expr* closure_expr, Expr** args, int arg_count, FILE* out) {
    (void)closure_expr;  // Used for context
    (void)args;
    (void)arg_count;

    fprintf(out, "    ; Call closure\n");
    fprintf(out, "    mov rax, [closure_ptr]\n");
    fprintf(out, "    mov rsi, [rax+8]      ; Load environment\n");
    fprintf(out, "    mov rdx, [rax]        ; Load function pointer\n");

    // Setup arguments (assume already on stack/registers)
    fprintf(out, "    call rdx\n");
}
