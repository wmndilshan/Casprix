#include "compiler/codegen/asmgen.h"
#include "compiler/sema/semantic.h"
#include "compiler/sema/drop_planner.h"
#include "compiler/sema/escape_analysis.h"
#include <stdio.h>
#include <string.h>

// Forward declarations
static void emit_asm(AssemblyGenerator* gen, const char* format, ...);
static void emit_load_var(AssemblyGenerator* gen, const char* name, const char* reg);
static void generate_asm_expr(AssemblyGenerator* gen, Expr* expr, const char* reg, SymbolTable* symbols);
static void generate_asm_stmt(AssemblyGenerator* gen, Stmt* stmt, SymbolTable* symbols);
static void collect_strings(AssemblyGenerator* gen, Expr* expr);
static void collect_strings_from_stmt(AssemblyGenerator* gen, Stmt* stmt);
static void prescan_expr_locals(AssemblyGenerator* gen, Expr* expr);
static void prescan_locals(AssemblyGenerator* gen, Stmt* stmt);
static void emit_lambda_functions_from_expr(AssemblyGenerator* gen, Expr* expr, SymbolTable* symbols);
static void emit_lambda_functions_from_stmt(AssemblyGenerator* gen, Stmt* stmt, SymbolTable* symbols);
static int alloc_local(AssemblyGenerator* gen, const char* name);
static int find_local(AssemblyGenerator* gen, const char* name);
static void format_global_var_symbol(const char* name, char* buf, size_t buf_size);

/* --- Calling Convention ABI --- */
#ifdef _WIN32
    static const char* ABI_I_REGS[] = {"rcx", "rdx", "r8", "r9"};
    static const int ABI_I_REG_COUNT = 4;
    static const int ABI_SHADOW_SPACE = 32;
#else
    static const char* ABI_I_REGS[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    static const int ABI_I_REG_COUNT = 6;
    static const int ABI_SHADOW_SPACE = 0; // Linux doesn't use shadow space
#endif

static MethodSymbol* find_constructor_symbol(ClassSymbol* class_sym) {
    if (!class_sym) return NULL;

    for (int i = 0; i < class_sym->method_count; i++) {
        if (class_sym->methods[i].is_constructor &&
            strcmp(class_sym->methods[i].name, class_sym->name) == 0) {
            return &class_sym->methods[i];
        }
    }

    for (int i = 0; i < class_sym->method_count; i++) {
        if (class_sym->methods[i].is_constructor) {
            return &class_sym->methods[i];
        }
    }

    return NULL;
}

// Helper function to convert DataType to string
static const char* type_to_string(DataType type) {
    return datatype_to_string(type);
}

static bool lambda_value_supported(const LambdaExpr* lambda) {
    return lambda && !lambda->has_mutable_capture;
}

static bool lambda_direct_call_supported(const LambdaExpr* lambda) {
    return lambda && !lambda->has_mutable_capture;
}

static DataType lambda_call_arg_type(const LambdaExpr* lambda, int arg_index) {
    if (!lambda) return TYPE_I64;
    if (arg_index < lambda->capture_count) {
        return TYPE_PTR;
    }
    arg_index -= lambda->capture_count;
    if (arg_index >= 0 && arg_index < lambda->param_count) {
        return lambda->parameters[arg_index].type;
    }
    return TYPE_I64;
}

static void format_closure_slot_name(int closure_id, int slot_index,
                                     char* buf, size_t buf_size) {
    snprintf(buf, buf_size, "__closure_%d_slot_%d", closure_id, slot_index);
}

static void alloc_closure_slots(AssemblyGenerator* gen, const LambdaExpr* lambda) {
    if (!lambda || lambda->capture_count <= 0 || lambda->has_mutable_capture) {
        return;
    }

    for (int i = lambda->capture_count; i >= 0; i--) {
        char slot_name[64];
        format_closure_slot_name(lambda->closure_id, i, slot_name, sizeof(slot_name));
        alloc_local(gen, slot_name);
    }
}

static void emit_load_var_addr(AssemblyGenerator* gen, const char* name, const char* reg) {
    int off = find_local(gen, name);
    if (off) {
        emit_asm(gen, "    lea %s, [rbp - %d]\n", reg, off);
    } else {
        char global_name[256];
        format_global_var_symbol(name, global_name, sizeof(global_name));
        emit_asm(gen, "    lea %s, [rel %s]\n", reg, global_name);
    }
}

static DataType closure_call_arg_type(const Symbol* symbol, int arg_index) {
    if (!symbol) return TYPE_I64;
    if (arg_index < symbol->closure_capture_count) {
        return TYPE_PTR;
    }
    arg_index -= symbol->closure_capture_count;
    if (symbol->param_types && arg_index >= 0 && arg_index < symbol->param_count) {
        return symbol->param_types[arg_index];
    }
    return TYPE_I64;
}

static void emit_lambda_call_arg(AssemblyGenerator* gen, const LambdaExpr* lambda,
                                 Expr** call_args, int call_arg_count, int arg_index,
                                 const char* reg, SymbolTable* symbols) {
    if (arg_index < lambda->capture_count) {
        emit_load_var_addr(gen, lambda->captured_vars[arg_index], reg);
        return;
    }

    arg_index -= lambda->capture_count;
    if (arg_index >= 0 && arg_index < call_arg_count) {
        generate_asm_expr(gen, call_args[arg_index], reg, symbols);
        return;
    }

    emit_asm(gen, "    xor %s, %s\n", reg, reg);
}

static void emit_closure_call_arg(AssemblyGenerator* gen, const Symbol* symbol,
                                  Expr** call_args, int call_arg_count, int arg_index,
                                  const char* reg, SymbolTable* symbols) {
    if (symbol && arg_index < symbol->closure_capture_count) {
        emit_asm(gen, "    mov %s, [r12 + %d]\n", reg, 8 * (arg_index + 1));
        return;
    }

    if (symbol) {
        arg_index -= symbol->closure_capture_count;
    }

    if (arg_index >= 0 && arg_index < call_arg_count) {
        generate_asm_expr(gen, call_args[arg_index], reg, symbols);
        return;
    }

    emit_asm(gen, "    xor %s, %s\n", reg, reg);
}

static void format_global_var_symbol(const char* name, char* buf, size_t buf_size) {
    snprintf(buf, buf_size, "__var_%s", name ? name : "unnamed");
}

void init_asm_generator(AssemblyGenerator* gen, FILE* output, void* unused) {
    (void)unused;  /* Logger parameter deprecated */
    gen->output = output;
    gen->label_count = 0;
    gen->string_count = 0;
    gen->var_count = 0;
    gen->temp_count = 0;
    gen->string_capacity = 16;
    gen->string_size = 0;
    gen->string_literals = ALLOCATE(char*, gen->string_capacity);
    gen->current_class = NULL;
    gen->loop_depth = 0;
    gen->local_names = NULL;
    gen->local_offsets = NULL;
    gen->local_count = 0;
    gen->local_capacity = 0;
    gen->frame_size = 0;
    drop_planner_init(&gen->drop_ctx);
}

void free_asm_generator(AssemblyGenerator* gen) {
    for (int i = 0; i < gen->string_size; i++) {
        free(gen->string_literals[i]);
    }
    free(gen->string_literals);
}

static void emit_asm(AssemblyGenerator* gen, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(gen->output, format, args);
    va_end(args);
}

/* NASM `db "..."` must not contain raw newlines/quotes; escape for printf + NASM. */
static void emit_data_string_literal(AssemblyGenerator* gen, int idx, const char* raw) {
    char esc[4096];
    size_t j = 0;
    for (size_t i = 0; raw[i] && j + 8 < sizeof(esc); i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c == '%') {
            esc[j++] = '%';
            esc[j++] = '%';
        } else if (c == '\\') {
            esc[j++] = '\\';
            esc[j++] = '\\';
        } else if (c == '"') {
            esc[j++] = '\\';
            esc[j++] = '"';
        } else if (c == '\n') {
            esc[j++] = '\\';
            esc[j++] = 'n';
        } else if (c == '\r') {
            esc[j++] = '\\';
            esc[j++] = 'r';
        } else if (c == '\t') {
            esc[j++] = '\\';
            esc[j++] = 't';
        } else if (c >= 32 && c < 127) {
            esc[j++] = (char)c;
        } else {
            int n = snprintf(esc + j, sizeof(esc) - j, "\\x%02X", (unsigned)c);
            if (n > 0) j += (size_t)n;
        }
    }
    esc[j] = '\0';
    emit_asm(gen, "    str_%d db \"%s\", 0\n", idx, esc);
}

/* ── Local variable frame management ─────────────────────────────────────── */

/* Begin a new function scope. Call before emitting method/function prologue. */
static void begin_local_frame(AssemblyGenerator* gen) {
    /* Free previous frame */
    for (int i = 0; i < gen->local_count; i++) free(gen->local_names[i]);
    gen->local_count = 0;
    gen->frame_size = 0;
}

/* Allocate a slot for 'name' on the current stack frame.
 * Returns the positive displacement from the frame base
 * (access as [rbp - offset]). */
static int alloc_local(AssemblyGenerator* gen, const char* name) {
    /* Check if already allocated */
    for (int i = 0; i < gen->local_count; i++) {
        if (strcmp(gen->local_names[i], name) == 0)
            return gen->local_offsets[i];
    }
    /* Grow arrays */
    if (gen->local_count >= gen->local_capacity) {
        int nc = gen->local_capacity == 0 ? 16 : gen->local_capacity * 2;
        gen->local_names   = realloc(gen->local_names,   sizeof(char*) * nc);
        gen->local_offsets = realloc(gen->local_offsets, sizeof(int)   * nc);
        gen->local_capacity = nc;
    }
    gen->frame_size += 8;
    gen->local_names[gen->local_count]   = strdup(name);
    gen->local_offsets[gen->local_count] = gen->frame_size;
    gen->local_count++;
    return gen->frame_size;
}

/* Find the stack offset for 'name', or 0 if it is a global. */
static int find_local(AssemblyGenerator* gen, const char* name) {
    for (int i = 0; i < gen->local_count; i++)
        if (strcmp(gen->local_names[i], name) == 0)
            return gen->local_offsets[i];
    return 0;  /* not a local — fall back to .bss global */
}

/* Emit a load: mov reg, <variable>.  Prefers stack slot, falls back to .bss. */
static void emit_load_var(AssemblyGenerator* gen, const char* name, const char* reg) {
    int off = find_local(gen, name);
    if (off)
        emit_asm(gen, "    mov %s, [rbp - %d]\n", reg, off);
    else {
        char global_name[256];
        format_global_var_symbol(name, global_name, sizeof(global_name));
        emit_asm(gen, "    mov %s, [rel %s]\n", reg, global_name);
    }
}

/* Emit a store: mov <variable>, reg.  Prefers stack slot, falls back to .bss. */
static void emit_store_var(AssemblyGenerator* gen, const char* name, const char* reg) {
    int off = find_local(gen, name);
    if (off)
        emit_asm(gen, "    mov [rbp - %d], %s\n", off, reg);
    else {
        char global_name[256];
        format_global_var_symbol(name, global_name, sizeof(global_name));
        emit_asm(gen, "    mov [rel %s], %s\n", global_name, reg);
    }
}

/* Same for float (movq). */
static void emit_load_var_xmm(AssemblyGenerator* gen, const char* name, int xmm_n) __attribute__((unused));
static void emit_load_var_xmm(AssemblyGenerator* gen, const char* name, int xmm_n) {
    int off = find_local(gen, name);
    if (off)
        emit_asm(gen, "    movq xmm%d, [rbp - %d]\n", xmm_n, off);
    else {
        char global_name[256];
        format_global_var_symbol(name, global_name, sizeof(global_name));
        emit_asm(gen, "    movq xmm%d, [rel %s]\n", xmm_n, global_name);
    }
}

static void emit_store_var_xmm(AssemblyGenerator* gen, const char* name, int xmm_n) __attribute__((unused));
static void emit_store_var_xmm(AssemblyGenerator* gen, const char* name, int xmm_n) {
    int off = find_local(gen, name);
    if (off)
        emit_asm(gen, "    movq [rbp - %d], xmm%d\n", off, xmm_n);
    else {
        char global_name[256];
        format_global_var_symbol(name, global_name, sizeof(global_name));
        emit_asm(gen, "    movq [rel %s], xmm%d\n", global_name, xmm_n);
    }
}

/* Emit the function prologue with the computed frame size.
 * Must be called AFTER all alloc_local() calls for this function. */
static void emit_prologue_stack(AssemblyGenerator* gen) {
    /* Round frame_size up to 16-byte alignment (required by ABI) */
    int aligned = (gen->frame_size + 15) & ~15;
    if (aligned > 0)
        emit_asm(gen, "    sub rsp, %d  ; local frame\n", aligned);
}

/* ── End local frame helpers ─────────────────────────────────────────────── */

/* ── Drop emission (RAII cleanup at scope/function exit) ────────────────── */

/**
 * Emit ARC release calls for variables that need cleanup.
 * Uses the drop planner to determine which variables need dropping
 * and emits the corresponding arc_release / rc_release calls.
 *
 * This is called:
 *   - At function epilogue (normal exit)
 *   - At return statements (early exit — drops all enclosing scopes)
 *   - At block scope exit (inner blocks)
 */
static void emit_scope_drops(AssemblyGenerator* gen, const DropEntry* drops,
                              int drop_count) {
    if (!drops || drop_count <= 0) return;

    emit_asm(gen, "    ; --- scope drops (%d vars) ---\n", drop_count);
    for (int i = 0; i < drop_count; i++) {
        const DropEntry* d = &drops[i];

        /* Skip moved, borrowed, and no-op entries */
        if (d->is_moved || d->is_borrowed || d->kind == DROP_NONE ||
            d->kind == DROP_REGION)
            continue;

        emit_asm(gen, "    ; drop '%s' (kind=%d)\n",
                 d->var_name ? d->var_name : "?", d->kind);

        /* Load the variable into the first argument register */
        if (d->stack_offset > 0) {
            emit_asm(gen, "    mov %s, [rbp - %d]\n", ABI_I_REGS[0], d->stack_offset);
        } else if (d->var_name) {
            int off = find_local(gen, d->var_name);
            if (off)
                emit_asm(gen, "    mov %s, [rbp - %d]\n", ABI_I_REGS[0], off);
            else
                emit_asm(gen, "    mov %s, [rel %s]\n", ABI_I_REGS[0], d->var_name);
        } else {
            continue;
        }

        /* NULL check */
        emit_asm(gen, "    test %s, %s\n", ABI_I_REGS[0], ABI_I_REGS[0]);
        int skip_label = gen->label_count++;
        emit_asm(gen, "    jz .drop_skip_%d\n", skip_label);

        int drop_stack_adj = ABI_SHADOW_SPACE > 0 ? ABI_SHADOW_SPACE : 16;

        switch (d->kind) {
            case DROP_ARC:
                emit_asm(gen, "    sub rsp, %d\n", drop_stack_adj);
                emit_asm(gen, "    call arc_release\n");
                emit_asm(gen, "    add rsp, %d\n", drop_stack_adj);
                break;
            case DROP_RC:
                emit_asm(gen, "    sub rsp, %d\n", drop_stack_adj);
                emit_asm(gen, "    call rc_release\n");
                emit_asm(gen, "    add rsp, %d\n", drop_stack_adj);
                break;
            case DROP_DTOR:
                if (d->dtor_name) {
                    emit_asm(gen, "    sub rsp, %d\n", drop_stack_adj);
                    emit_asm(gen, "    call %s\n", d->dtor_name);
                    emit_asm(gen, "    add rsp, %d\n", drop_stack_adj);
                }
                break;
            case DROP_SCOPE_GUARD:
                emit_asm(gen, "    sub rsp, %d\n", drop_stack_adj);
                emit_asm(gen, "    call scope_guard_drop_extern\n");
                emit_asm(gen, "    add rsp, %d\n", drop_stack_adj);
                break;
            default:
                break;
        }
        emit_asm(gen, ".drop_skip_%d:\n", skip_label);
    }
    emit_asm(gen, "    ; --- end scope drops ---\n");
}

/* ─────────────────────────────────────────────────────────────────────────── */

static int add_string_literal(AssemblyGenerator* gen, const char* str) {
    // Check if string already exists
    for (int i = 0; i < gen->string_size; i++) {
        if (strcmp(gen->string_literals[i], str) == 0) {
            return i;
        }
    }
    
    // Add new string
    if (gen->string_size >= gen->string_capacity) {
        gen->string_capacity = GROW_CAPACITY(gen->string_capacity);
        gen->string_literals = GROW_ARRAY(char*, gen->string_literals, 
                                         gen->string_size, gen->string_capacity);
    }
    
    gen->string_literals[gen->string_size] = strdup(str);
    return gen->string_size++;
}

static void collect_strings(AssemblyGenerator* gen, Expr* expr) {
    if (!expr) return;
    
    switch (expr->type) {
        case EXPR_LITERAL: {
            LiteralExpr* lit = &expr->as.literal;
            if (lit->type == TYPE_STRING) {
                add_string_literal(gen, lit->value.string_value);
            }
            break;
        }
        case EXPR_BINARY: {
            BinaryExpr* binary = &expr->as.binary;
            collect_strings(gen, binary->left);
            collect_strings(gen, binary->right);
            break;
        }
        case EXPR_UNARY: {
            UnaryExpr* unary = &expr->as.unary;
            collect_strings(gen, unary->operand);
            break;
        }
        case EXPR_CALL: {
            CallExpr* call = &expr->as.call;
            for (int i = 0; i < call->arg_count; i++) {
                collect_strings(gen, call->arguments[i]);
            }
            break;
        }
        case EXPR_MEMBER_ACCESS: {
            MemberAccessExpr* member = &expr->as.member;
            collect_strings(gen, member->object);
            if (member->is_method_call) {
                for (int i = 0; i < member->arg_count; i++) {
                    collect_strings(gen, member->arguments[i]);
                }
            }
            break;
        }
        case EXPR_NEW: {
            NewExpr* new_expr = &expr->as.new_expr;
            for (int i = 0; i < new_expr->arg_count; i++) {
                collect_strings(gen, new_expr->arguments[i]);
            }
            break;
        }
        case EXPR_INDEX: {
            IndexExpr* index = &expr->as.index;
            collect_strings(gen, index->array);
            collect_strings(gen, index->index);
            break;
        }
        case EXPR_LAMBDA: {
            LambdaExpr* lambda = &expr->as.lambda;
            if (lambda->is_expression && lambda->expr_body) {
                collect_strings(gen, lambda->expr_body);
            }
            // Block body strings are collected via statement traversal
            break;
        }
        case EXPR_SUPER: {
            SuperExpr* super = &expr->as.super_expr;
            if (super->is_method_call) {
                for (int i = 0; i < super->arg_count; i++) {
                    collect_strings(gen, super->arguments[i]);
                }
            }
            break;
        }
        case EXPR_STATIC_ACCESS: {
            StaticAccessExpr* static_access = &expr->as.static_access;
            if (static_access->is_method_call) {
                for (int i = 0; i < static_access->arg_count; i++) {
                    collect_strings(gen, static_access->arguments[i]);
                }
            }
            break;
        }
        default:
            break;
    }
}

// Variable name collection
typedef struct {
    char** names;
    int count;
    int capacity;
} VarList;

static VarList var_list = {NULL, 0, 0};

static void add_variable_name(const char* name) {
    // Check if already exists
    for (int i = 0; i < var_list.count; i++) {
        if (strcmp(var_list.names[i], name) == 0) {
            return;
        }
    }
    
    // Grow if needed
    if (var_list.count >= var_list.capacity) {
        var_list.capacity = var_list.capacity == 0 ? 16 : var_list.capacity * 2;
        var_list.names = GROW_ARRAY(char*, var_list.names, var_list.count, var_list.capacity);
    }
    
    var_list.names[var_list.count++] = strdup(name);
}

static void collect_variables_from_stmt(Stmt* stmt) {
    if (!stmt) return;
    
    switch (stmt->type) {
        case STMT_CONST_DECL:  /* fall through */
        case STMT_DECLARATION: {
            DeclarationStmt* decl = &stmt->as.declaration;
            if (decl->name) {
                add_variable_name(decl->name);
            }
            break;
        }
        case STMT_FOR: {
            ForStmt* for_stmt = &stmt->as.for_stmt;
            if (for_stmt->variable) {
                add_variable_name(for_stmt->variable);
            }
            collect_variables_from_stmt(for_stmt->body);
            collect_variables_from_stmt(for_stmt->increment);
            break;
        }
        case STMT_FUNCTION: {
            FunctionStmt* func = &stmt->as.function;
            for (int i = 0; i < func->param_count; i++) {
                if (func->parameters[i].name) {
                    add_variable_name(func->parameters[i].name);
                }
            }
            collect_variables_from_stmt(func->body);
            break;
        }
        case STMT_IF: {
            IfStmt* if_stmt = &stmt->as.if_stmt;
            collect_variables_from_stmt(if_stmt->then_branch);
            collect_variables_from_stmt(if_stmt->else_branch);
            break;
        }
        case STMT_WHILE: {
            WhileStmt* while_stmt = &stmt->as.while_stmt;
            collect_variables_from_stmt(while_stmt->body);
            break;
        }
        case STMT_BLOCK: {
            BlockStmt* block = &stmt->as.block;
            for (int i = 0; i < block->stmt_count; i++) {
                collect_variables_from_stmt(block->statements[i]);
            }
            break;
        }
        case STMT_CLASS: {
            ClassStmt* class_stmt = &stmt->as.class_stmt;
            // Collect variables from all methods
            for (int i = 0; i < class_stmt->method_count; i++) {
                MethodDecl* method = &class_stmt->methods[i];
                // Add method parameters
                for (int j = 0; j < method->param_count; j++) {
                    if (method->parameters[j].name) {
                        add_variable_name(method->parameters[j].name);
                    }
                }
                // Collect variables from method body
                collect_variables_from_stmt(method->body);
            }
            break;
        }
        default:
            break;
    }
}

static void collect_strings_from_stmt(AssemblyGenerator* gen, Stmt* stmt) {
    if (!stmt) return;
    
    switch (stmt->type) {
        case STMT_PRINT: {
            PrintStmt* print = &stmt->as.print;
            collect_strings(gen, print->expression);
            break;
        }
        case STMT_CONST_DECL:  /* fall through */
        case STMT_DECLARATION: {
            DeclarationStmt* decl = &stmt->as.declaration;
            collect_strings(gen, decl->initializer);
            break;
        }
        case STMT_ASSIGNMENT: {
            AssignmentStmt* assign = &stmt->as.assignment;
            collect_strings(gen, assign->value);
            break;
        }
        case STMT_IF: {
            IfStmt* if_stmt = &stmt->as.if_stmt;
            collect_strings(gen, if_stmt->condition);
            collect_strings_from_stmt(gen, if_stmt->then_branch);
            collect_strings_from_stmt(gen, if_stmt->else_branch);
            break;
        }
        case STMT_WHILE: {
            WhileStmt* while_stmt = &stmt->as.while_stmt;
            collect_strings(gen, while_stmt->condition);
            collect_strings_from_stmt(gen, while_stmt->body);
            break;
        }
        case STMT_FOR: {
            ForStmt* for_stmt = &stmt->as.for_stmt;
            collect_strings(gen, for_stmt->initializer);
            collect_strings(gen, for_stmt->condition);
            collect_strings_from_stmt(gen, for_stmt->increment);
            collect_strings_from_stmt(gen, for_stmt->body);
            break;
        }
        case STMT_FUNCTION: {
            FunctionStmt* func = &stmt->as.function;
            collect_strings_from_stmt(gen, func->body);
            break;
        }
        case STMT_RETURN: {
            ReturnStmt* ret = &stmt->as.return_stmt;
            collect_strings(gen, ret->value);
            break;
        }
        case STMT_BLOCK: {
            BlockStmt* block = &stmt->as.block;
            for (int i = 0; i < block->stmt_count; i++) {
                collect_strings_from_stmt(gen, block->statements[i]);
            }
            break;
        }
        case STMT_EXPR: {
            ExprStmt* expr_stmt = &stmt->as.expr_stmt;
            collect_strings(gen, expr_stmt->expression);
            break;
        }
        case STMT_INCLUDE:
            // Include statements don't have expressions to collect
            break;
        case STMT_BREAK:
        case STMT_CONTINUE:
            // No expressions to collect
            break;
        case STMT_EXTERN:
            // Extern declarations don't have Casprix expressions to collect strings from
            break;
        case STMT_STRUCT:
        case STMT_ENUM:
        case STMT_UNION:
            // Type declarations don't have string literals to collect
            break;
        case STMT_CLASS: {
            ClassStmt* class_stmt = &stmt->as.class_stmt;
            // Collect strings from all methods
            for (int i = 0; i < class_stmt->method_count; i++) {
                MethodDecl* method = &class_stmt->methods[i];
                collect_strings_from_stmt(gen, method->body);
            }
            // Collect strings from field default values
            for (int i = 0; i < class_stmt->field_count; i++) {
                if (class_stmt->fields[i].default_value) {
                    collect_strings(gen, class_stmt->fields[i].default_value);
                }
            }
            break;
        }

        /* ── New statement types ── */
        case STMT_FOR_IN:
            collect_strings(gen, stmt->as.for_in_stmt.iterable);
            collect_strings_from_stmt(gen, stmt->as.for_in_stmt.body);
            break;

        case STMT_MATCH: {
            MatchStmt* ms = &stmt->as.match_stmt;
            collect_strings(gen, ms->subject);
            for (int i = 0; i < ms->arm_count; i++) {
                collect_strings(gen, ms->arms[i].pattern);
                collect_strings_from_stmt(gen, ms->arms[i].body);
            }
            break;
        }

        case STMT_THROW:
            collect_strings(gen, stmt->as.throw_stmt.value);
            break;

        case STMT_TRY: {
            TryStmt* t = &stmt->as.try_stmt;
            collect_strings_from_stmt(gen, t->try_body);
            for (int i = 0; i < t->catch_count; i++)
                collect_strings_from_stmt(gen, t->catches[i].body);
            collect_strings_from_stmt(gen, t->finally_body);
            break;
        }

        case STMT_TRAIT:
            /* trait declarations have no string literals */
            break;

        case STMT_IMPL:
            /* Walk method bodies to collect strings */
            for (int i = 0; i < stmt->as.impl_stmt.method_count; i++) {
                collect_strings_from_stmt(gen, stmt->as.impl_stmt.methods[i]);
            }
            break;
    }
}

static void generate_asm_literal(AssemblyGenerator* gen, Expr* expr, const char* reg) {
    LiteralExpr* lit = &expr->as.literal;
    
    switch (lit->type) {
        case TYPE_INT:
            emit_asm(gen, "    mov %s, %lld\n", reg, lit->value.int_value);
            break;
        case TYPE_FLOAT: {
            // Emit IEEE 754 double bit pattern (not truncated integer)
            double dval = lit->value.float_value;
            uint64_t bits;
            memcpy(&bits, &dval, sizeof(bits));
            emit_asm(gen, "    mov %s, 0x%llX  ; float %g\n", reg,
                    (unsigned long long)bits, dval);
            break;
        }
        case TYPE_STRING: {
            int str_id = add_string_literal(gen, lit->value.string_value);
            emit_asm(gen, "    lea %s, [rel str_%d]\n", reg, str_id);
            break;
        }
        case TYPE_BOOL:
            emit_asm(gen, "    mov %s, %d\n", reg, lit->value.bool_value ? 1 : 0);
            break;
        default:
            break;
    }
}

/* Pre-scan a statement tree and allocate stack slots for all declared locals */
static void prescan_expr_locals(AssemblyGenerator* gen, Expr* expr) {
    if (!expr) return;

    switch (expr->type) {
        case EXPR_BINARY:
            prescan_expr_locals(gen, expr->as.binary.left);
            prescan_expr_locals(gen, expr->as.binary.right);
            break;
        case EXPR_UNARY:
            prescan_expr_locals(gen, expr->as.unary.operand);
            break;
        case EXPR_CALL:
            prescan_expr_locals(gen, expr->as.call.callee);
            for (int i = 0; i < expr->as.call.arg_count; i++) {
                prescan_expr_locals(gen, expr->as.call.arguments[i]);
            }
            break;
        case EXPR_MEMBER_ACCESS:
            prescan_expr_locals(gen, expr->as.member.object);
            for (int i = 0; i < expr->as.member.arg_count; i++) {
                prescan_expr_locals(gen, expr->as.member.arguments[i]);
            }
            break;
        case EXPR_STATIC_ACCESS:
            for (int i = 0; i < expr->as.static_access.arg_count; i++) {
                prescan_expr_locals(gen, expr->as.static_access.arguments[i]);
            }
            break;
        case EXPR_INDEX:
            prescan_expr_locals(gen, expr->as.index.array);
            prescan_expr_locals(gen, expr->as.index.index);
            break;
        case EXPR_NEW:
            for (int i = 0; i < expr->as.new_expr.arg_count; i++) {
                prescan_expr_locals(gen, expr->as.new_expr.arguments[i]);
            }
            break;
        case EXPR_SUPER:
            for (int i = 0; i < expr->as.super_expr.arg_count; i++) {
                prescan_expr_locals(gen, expr->as.super_expr.arguments[i]);
            }
            break;
        case EXPR_LAMBDA:
            alloc_closure_slots(gen, &expr->as.lambda);
            if (expr->as.lambda.is_expression) {
                prescan_expr_locals(gen, expr->as.lambda.expr_body);
            } else {
                prescan_locals(gen, expr->as.lambda.block_body);
            }
            break;
        default:
            break;
    }
}

static void prescan_locals(AssemblyGenerator* gen, Stmt* stmt) {
    if (!stmt) return;
    switch (stmt->type) {
        case STMT_CONST_DECL:  /* fall through */
        case STMT_DECLARATION:
            alloc_local(gen, stmt->as.declaration.name);
            prescan_expr_locals(gen, stmt->as.declaration.initializer);
            break;
        case STMT_FOR:
            prescan_expr_locals(gen, stmt->as.for_stmt.initializer);
            prescan_expr_locals(gen, stmt->as.for_stmt.condition);
            if (stmt->as.for_stmt.variable)
                alloc_local(gen, stmt->as.for_stmt.variable);
            prescan_locals(gen, stmt->as.for_stmt.body);
            prescan_locals(gen, stmt->as.for_stmt.increment);
            break;
        case STMT_FOR_IN:
            /* loop variable + hidden index counter */
            alloc_local(gen, stmt->as.for_in_stmt.var_name);
            alloc_local(gen, "__cpx_for_idx");
            prescan_expr_locals(gen, stmt->as.for_in_stmt.iterable);
            prescan_locals(gen, stmt->as.for_in_stmt.body);
            break;
        case STMT_TRY: {
            /* jmp_buf slot (we store only 8 bytes here; full setjmp buf grows from there) */
            alloc_local(gen, "__cpx_jbuf");
            TryStmt* t = &stmt->as.try_stmt;
            prescan_locals(gen, t->try_body);
            for (int i = 0; i < t->catch_count; i++) {
                if (t->catches[i].exception_var)
                    alloc_local(gen, t->catches[i].exception_var);
                prescan_locals(gen, t->catches[i].body);
            }
            prescan_locals(gen, t->finally_body);
            break;
        }
        case STMT_MATCH: {
            MatchStmt* m = &stmt->as.match_stmt;
            prescan_expr_locals(gen, m->subject);
            for (int i = 0; i < m->arm_count; i++)
                prescan_locals(gen, m->arms[i].body);
            break;
        }
        case STMT_IF:
            prescan_expr_locals(gen, stmt->as.if_stmt.condition);
            prescan_locals(gen, stmt->as.if_stmt.then_branch);
            prescan_locals(gen, stmt->as.if_stmt.else_branch);
            break;
        case STMT_WHILE:
            prescan_expr_locals(gen, stmt->as.while_stmt.condition);
            prescan_locals(gen, stmt->as.while_stmt.body);
            break;
        case STMT_BLOCK: {
            BlockStmt* b = &stmt->as.block;
            for (int i = 0; i < b->stmt_count; i++)
                prescan_locals(gen, b->statements[i]);
            break;
        }
        case STMT_EXPR:
            prescan_expr_locals(gen, stmt->as.expr_stmt.expression);
            break;
        case STMT_PRINT:
            prescan_expr_locals(gen, stmt->as.print.expression);
            break;
        case STMT_ASSIGNMENT:
            prescan_expr_locals(gen, stmt->as.assignment.target);
            prescan_expr_locals(gen, stmt->as.assignment.value);
            break;
        case STMT_RETURN:
            prescan_expr_locals(gen, stmt->as.return_stmt.value);
            break;
        default: break;
    }
}

static void generate_asm_variable(AssemblyGenerator* gen, Expr* expr, const char* reg,
                                  SymbolTable* symbols) {
    VariableExpr* var = &expr->as.variable;

    if (symbols && var->name) {
        Symbol* symbol = lookup_symbol(symbols, var->name);
        if (symbol && symbol->kind == SYMBOL_FUNCTION) {
            if (symbol->is_extern) {
                emit_asm(gen, "    lea %s, [rel %s]\n", reg, var->name);
            } else {
                emit_asm(gen, "    lea %s, [rel __casprix_%s]\n", reg, var->name);
            }
            return;
        }
    }

    emit_load_var(gen, var->name, reg);
    /* move semantics: null-out source after loading */
    if (var->is_move) {
        int off = find_local(gen, var->name);
        if (off)
            emit_asm(gen, "    mov qword [rbp - %d], 0  ; move: invalidate source\n", off);
        else {
            char global_name[256];
            format_global_var_symbol(var->name, global_name, sizeof(global_name));
            emit_asm(gen, "    mov qword [rel %s], 0  ; move: invalidate source\n", global_name);
        }
    }
}

static void generate_asm_binary(AssemblyGenerator* gen, Expr* expr, const char* reg, SymbolTable* symbols) {
    BinaryExpr* binary = &expr->as.binary;
    
    // Generate left operand into reg
    generate_asm_expr(gen, binary->left, reg, symbols);
    
    // Save left operand
    emit_asm(gen, "    push %s\n", reg);
    
    // Generate right operand into reg
    generate_asm_expr(gen, binary->right, reg, symbols);
    
    // Move right to different register
    emit_asm(gen, "    mov rbx, %s\n", reg);
    
    // Restore left operand
    emit_asm(gen, "    pop %s\n", reg);
    
    // Perform operation
    switch (binary->operator) {
        case TOKEN_PLUS:
            if (expr->data_type == TYPE_STRING) {
                // String concatenation: call nuwan_string_concat(left, right)
                // Windows x64 calling convention: first arg in RCX, second in RDX
                // Left operand is in reg, right is in rbx
                emit_asm(gen, "    mov %s, %s  ; First arg (left string)\n", ABI_I_REGS[0], reg);
                emit_asm(gen, "    mov %s, rbx  ; Second arg (right string)\n", ABI_I_REGS[1]);
                int str_stack_adj = ABI_SHADOW_SPACE > 0 ? ABI_SHADOW_SPACE : 16;
                emit_asm(gen, "    sub rsp, %d  ; Shadow space/alignment\n", str_stack_adj);
                emit_asm(gen, "    call nuwan_string_concat\n");
                emit_asm(gen, "    add rsp, %d  ; Restore stack\n", str_stack_adj);
                emit_asm(gen, "    mov %s, rax  ; Result\n", reg);
            } else {
                emit_asm(gen, "    add %s, rbx\n", reg);
            }
            break;
        case TOKEN_MINUS:
            emit_asm(gen, "    sub %s, rbx\n", reg);
            break;
        case TOKEN_STAR:
            emit_asm(gen, "    imul %s, rbx\n", reg);
            break;
        case TOKEN_SLASH:
            emit_asm(gen, "    mov rdx, 0\n");
            emit_asm(gen, "    idiv rbx\n");
            emit_asm(gen, "    mov %s, rax\n", reg);
            break;
        case TOKEN_PERCENT:
            emit_asm(gen, "    mov rdx, 0\n");
            emit_asm(gen, "    idiv rbx\n");
            emit_asm(gen, "    mov %s, rdx\n", reg);  // Modulo result is in rdx
            break;
        case TOKEN_EQUAL:
            emit_asm(gen, "    cmp %s, rbx\n", reg);
            emit_asm(gen, "    sete al\n");
            emit_asm(gen, "    movzx %s, al\n", reg);
            break;
        case TOKEN_NOT_EQUAL:
            emit_asm(gen, "    cmp %s, rbx\n", reg);
            emit_asm(gen, "    setne al\n");
            emit_asm(gen, "    movzx %s, al\n", reg);
            break;
        case TOKEN_LESS:
            emit_asm(gen, "    cmp %s, rbx\n", reg);
            emit_asm(gen, "    setl al\n");
            emit_asm(gen, "    movzx %s, al\n", reg);
            break;
        case TOKEN_LESS_EQUAL:
            emit_asm(gen, "    cmp %s, rbx\n", reg);
            emit_asm(gen, "    setle al\n");
            emit_asm(gen, "    movzx %s, al\n", reg);
            break;
        case TOKEN_GREATER:
            emit_asm(gen, "    cmp %s, rbx\n", reg);
            emit_asm(gen, "    setg al\n");
            emit_asm(gen, "    movzx %s, al\n", reg);
            break;
        case TOKEN_GREATER_EQUAL:
            emit_asm(gen, "    cmp %s, rbx\n", reg);
            emit_asm(gen, "    setge al\n");
            emit_asm(gen, "    movzx %s, al\n", reg);
            break;
        case TOKEN_AND:
            emit_asm(gen, "    and %s, rbx\n", reg);
            break;
        case TOKEN_OR:
            emit_asm(gen, "    or %s, rbx\n", reg);
            break;
        /* ── Bitwise operators ── */
        case TOKEN_BITAND:
            emit_asm(gen, "    and %s, rbx\n", reg);  /* bitwise AND */
            break;
        case TOKEN_PIPE:  /* bitwise OR (TOKEN_PIPE doubles as | in non-lambda context) */
            emit_asm(gen, "    or %s, rbx\n", reg);
            break;
        case TOKEN_BITXOR:
            emit_asm(gen, "    xor %s, rbx\n", reg);  /* bitwise XOR */
            break;
        case TOKEN_LSHIFT:
            /* shl reg, cl — move count to cl first */
            emit_asm(gen, "    mov rcx, rbx\n");
            emit_asm(gen, "    shl %s, cl\n", reg);
            break;
        case TOKEN_RSHIFT:
            emit_asm(gen, "    mov rcx, rbx\n");
            emit_asm(gen, "    sar %s, cl\n", reg);   /* arithmetic right shift */
            break;
        default:
            break;
    }
}

static void generate_asm_expr(AssemblyGenerator* gen, Expr* expr, const char* reg, SymbolTable* symbols) {
    if (!expr) return;
    
    switch (expr->type) {
        case EXPR_LITERAL:
            generate_asm_literal(gen, expr, reg);
            break;
        case EXPR_VARIABLE:
            generate_asm_variable(gen, expr, reg, symbols);
            break;
        case EXPR_AWAIT:
            /* Await is handled by async lowering; no-op in asm gen */
            break;
        case EXPR_BINARY:
            generate_asm_binary(gen, expr, reg, symbols);
            break;
        case EXPR_UNARY: {
            UnaryExpr* unary = &expr->as.unary;
            // Generate the operand
            generate_asm_expr(gen, unary->operand, reg, symbols);

            // Apply unary operator
            switch (unary->operator) {
                case TOKEN_MINUS:
                    // Arithmetic negation: -value
                    emit_asm(gen, "    neg %s\n", reg);
                    break;
                case TOKEN_NOT:
                    // Logical NOT: !value (convert to boolean 0/1 then flip)
                    emit_asm(gen, "    test %s, %s\n", reg, reg);
                    emit_asm(gen, "    setz al\n");
                    emit_asm(gen, "    movzx %s, al\n", reg);
                    break;
                case TOKEN_BITNOT:
                    // Bitwise complement: ~value
                    emit_asm(gen, "    not %s\n", reg);
                    break;
                default:
                    break;
            }
            break;
        }
        case EXPR_CALL: {
            CallExpr* call = &expr->as.call;
            if (call->callee &&
                call->callee->type == EXPR_LAMBDA &&
                call->callee->as.lambda.capture_count > 0) {
                LambdaExpr* lambda = &call->callee->as.lambda;
                int total_args = lambda->capture_count + call->arg_count;
                int extra_args = (total_args > ABI_I_REG_COUNT) ? (total_args - ABI_I_REG_COUNT) : 0;
                int stack_size = ABI_SHADOW_SPACE + (extra_args * 8);
                if (stack_size % 16 != 0) stack_size += 8;
                
                emit_asm(gen, "    sub rsp, %d\n", stack_size);

                for (int i = total_args - 1; i >= ABI_I_REG_COUNT; i--) {
                    int offset = ABI_SHADOW_SPACE + (i - ABI_I_REG_COUNT) * 8;
                    emit_lambda_call_arg(gen, lambda, call->arguments, call->arg_count,
                                         i, "rax", symbols);
                    emit_asm(gen, "    mov [rsp + %d], rax\n", offset);
                }

                int n_reg = total_args < ABI_I_REG_COUNT ? total_args : ABI_I_REG_COUNT;
                for (int i = 0; i < n_reg; i++) {
                    emit_lambda_call_arg(gen, lambda, call->arguments, call->arg_count,
                                         i, ABI_I_REGS[i], symbols);
                }

                {
                    const char* iregs[] = {"rcx", "rdx", "r8", "r9"};
                    int n = total_args < 4 ? total_args : 4;
                    for (int i = 0; i < n; i++) {
                        DataType pt = lambda_call_arg_type(lambda, i);
                        if (pt == TYPE_FLOAT || pt == TYPE_F32 || pt == TYPE_F64) {
                            emit_asm(gen, "    movq xmm%d, %s  ; lambda arg %d\n",
                                     i, iregs[i], i);
                        }
                    }
                }

                if (lambda_direct_call_supported(lambda)) {
                    emit_asm(gen, "    call __lambda_%d\n", lambda->closure_id);
                } else {
                    emit_asm(gen, "    ; ERROR: mutable-capture lambda calls are not implemented\n");
                    emit_asm(gen, "    xor rax, rax\n");
                }

                emit_asm(gen, "    add rsp, %d\n", stack_size);
                emit_asm(gen, "    mov %s, rax\n", reg);
                break;
            }

            if (call->callee &&
                call->callee->type == EXPR_VARIABLE &&
                call->callee->as.variable.name) {
                Symbol* closure_sym = lookup_symbol(symbols, call->callee->as.variable.name);
                if (closure_sym && closure_sym->is_closure_value) {
                    int total_args = closure_sym->closure_capture_count + call->arg_count;
                    int extra_args = (total_args > ABI_I_REG_COUNT) ? (total_args - ABI_I_REG_COUNT) : 0;
                    int stack_size = ABI_SHADOW_SPACE + (extra_args * 8);

                    if (stack_size % 16 != 0) stack_size += 8;

                    generate_asm_expr(gen, call->callee, "rax", symbols);
                    emit_asm(gen, "    mov r12, rax  ; closure handle\n");
                    emit_asm(gen, "    sub rsp, %d\n", stack_size);

                    for (int i = total_args - 1; i >= ABI_I_REG_COUNT; i--) {
                        int offset = ABI_SHADOW_SPACE + (i - ABI_I_REG_COUNT) * 8;
                        emit_closure_call_arg(gen, closure_sym, call->arguments,
                                              call->arg_count, i, "rax", symbols);
                        emit_asm(gen, "    mov [rsp + %d], rax\n", offset);
                    }

                    int n_reg = total_args < ABI_I_REG_COUNT ? total_args : ABI_I_REG_COUNT;
                    for (int i = 0; i < n_reg; i++) {
                        emit_closure_call_arg(gen, closure_sym, call->arguments,
                                              call->arg_count, i, ABI_I_REGS[i], symbols);
                    }

                    {
                        const char* iregs[] = {"rcx", "rdx", "r8", "r9"};
                        int n = total_args < 4 ? total_args : 4;
                        for (int i = 0; i < n; i++) {
                            DataType pt = closure_call_arg_type(closure_sym, i);
                            if (pt == TYPE_FLOAT || pt == TYPE_F32 || pt == TYPE_F64) {
                                emit_asm(gen, "    movq xmm%d, %s  ; closure arg %d\n",
                                         i, iregs[i], i);
                            }
                        }
                    }

                    emit_asm(gen, "    call qword [r12]\n");
                    emit_asm(gen, "    add rsp, %d\n", stack_size);
                    emit_asm(gen, "    mov %s, rax\n", reg);
                    break;
                }
            }

            // ABI-specific argument passing
            int extra_args = (call->arg_count > ABI_I_REG_COUNT) ? (call->arg_count - ABI_I_REG_COUNT) : 0;
            int stack_size = ABI_SHADOW_SPACE + (extra_args * 8);
            // Ensure 16-byte alignment
            if (stack_size % 16 != 0) {
                stack_size += 8;
            }

            emit_asm(gen, "    sub rsp, %d\n", stack_size);

            // Push stack arguments
            for (int i = call->arg_count - 1; i >= ABI_I_REG_COUNT; i--) {
                generate_asm_expr(gen, call->arguments[i], "rax", symbols);
                int offset = ABI_SHADOW_SPACE + (i - ABI_I_REG_COUNT) * 8;
                emit_asm(gen, "    mov [rsp + %d], rax\n", offset);
            }

            // Load register arguments
            int reg_args = call->arg_count < ABI_I_REG_COUNT ? call->arg_count : ABI_I_REG_COUNT;
            for (int i = 0; i < reg_args; i++) {
                generate_asm_expr(gen, call->arguments[i], ABI_I_REGS[i], symbols);
            }

            // Handle float arguments
            Symbol* func_sym = call->name ? lookup_symbol(symbols, call->name) : NULL;
            if (func_sym && func_sym->param_types) {
                int n = call->arg_count < 8 ? call->arg_count : 8; // Linux supports up to 8 XMM
                for (int i = 0; i < n; i++) {
                    DataType pt = func_sym->param_types[i];
                    if (pt == TYPE_FLOAT || pt == TYPE_F32 || pt == TYPE_F64) {
#ifdef _WIN32
                        if (i < 4) emit_asm(gen, "    movq xmm%d, %s\n", i, ABI_I_REGS[i]);
#else
                        // On Linux, floats are often already in XMM if we changed generate_asm_expr,
                        // but here we copy from the GPR we just loaded.
                        emit_asm(gen, "    movq xmm%d, %s\n", i, ABI_I_REGS[i]);
#endif
                    }
                }
            }

            if (func_sym && func_sym->kind == SYMBOL_FUNCTION && func_sym->is_extern) {
                emit_asm(gen, "    call %s\n", call->name);
            } else if (func_sym && func_sym->kind == SYMBOL_FUNCTION) {
                emit_asm(gen, "    call __casprix_%s\n", call->name);
            } else if (call->callee) {
                generate_asm_expr(gen, call->callee, "rax", symbols);
                emit_asm(gen, "    call rax\n");
            } else {
                emit_asm(gen, "    ; ERROR: invalid call target\n");
            }
            emit_asm(gen, "    add rsp, %d\n", stack_size); // Restore stack
            emit_asm(gen, "    mov %s, rax\n", reg);
            break;
        }
        case EXPR_MEMBER_ACCESS: {
            MemberAccessExpr* member = &expr->as.member;

            if (!member->is_method_call) {
                // Field access: load this pointer, then access field at offset
                // Generate code for the object expression (usually 'this')
                generate_asm_expr(gen, member->object, "rax", symbols);

                // Determine the class of the object
                ClassSymbol* class_sym = NULL;
                if (member->object->class_name) {
                    // Object has a specific class type (e.g., nested access: this.field.innerField)
                    class_sym = lookup_class(symbols, member->object->class_name);
                } else if (member->object->type == EXPR_THIS) {
                    // Direct 'this' access
                    class_sym = gen->current_class;
                }

                if (class_sym) {
                    FieldSymbol* field = find_field(class_sym, member->member_name);
                    if (field) {
                        // Load field value: [object_ptr + offset]
                        emit_asm(gen, "    mov %s, [rax + %d]  ; Access field %s\n",
                                reg, field->offset, member->member_name);
                    } else {
                        emit_asm(gen, "    ; ERROR: Field '%s' not found in class '%s'\n",
                                member->member_name, class_sym->name);
                        emit_asm(gen, "    mov %s, 0\n", reg);
                    }
                } else {
                    emit_asm(gen, "    ; ERROR: Cannot determine class for member access\n");
                    emit_asm(gen, "    mov %s, 0\n", reg);
                }
            } else {
                // Method call: obj.method(args)
                emit_asm(gen, "    ; Method call: %s\n", member->member_name);

                // Generate code to get the object pointer
                generate_asm_expr(gen, member->object, "rax", symbols);

                /* Save object pointer in a frame slot — avoids clobbering callee-saved
                   registers (r14/r15) without a matching prologue push. */
                int mc_this_off = alloc_local(gen, "__cpx_this");
                emit_asm(gen, "    mov [rbp - %d], rax  ; save object ptr\n", mc_this_off);

                // Allocate shadow space + stack args (must be 16-byte aligned)
                int reg_args_limit = ABI_I_REG_COUNT - 1; // 1 for 'this'
                int method_stack_args = member->arg_count > reg_args_limit ? member->arg_count - reg_args_limit : 0;
                int method_stack_size = ABI_SHADOW_SPACE + (method_stack_args * 8);
                if (method_stack_size % 16 != 0) method_stack_size += 8;
                emit_asm(gen, "    sub rsp, %d\n", method_stack_size);

                // Evaluate and place arguments directly into registers/stack
                int r_args = member->arg_count < reg_args_limit ? member->arg_count : reg_args_limit;
                for (int i = 0; i < r_args; i++) {
                    generate_asm_expr(gen, member->arguments[i], ABI_I_REGS[i + 1], symbols);
                }
                // Handle remaining args on stack
                for (int i = reg_args_limit; i < member->arg_count; i++) {
                    generate_asm_expr(gen, member->arguments[i], "rax", symbols);
                    emit_asm(gen, "    mov [rsp + %d], rax  ; Arg %d on stack\n",
                            ABI_SHADOW_SPACE + (i - reg_args_limit) * 8, i);
                }

                // Object pointer ('this') goes in the first argument register
                emit_asm(gen, "    mov %s, [rbp - %d]  ; Load object pointer (this)\n", ABI_I_REGS[0], mc_this_off);

                // Determine the class of the object to find the method
                // Use the class_name from the object expression (set by semantic analyzer)
                const char* class_name = NULL;
                if (member->object->class_name) {
                    // Object has a specific class type (e.g., field access: this.taskList)
                    class_name = member->object->class_name;
                } else if (member->object->type == EXPR_THIS) {
                    // Direct 'this' access - use current class
                    class_name = gen->current_class ? gen->current_class->name : NULL;
                }

                if (class_name) {
                    // Look up the class and method to determine if it's virtual
                    ClassSymbol* class_sym = lookup_class(symbols, class_name);
                    MethodSymbol* method_sym = NULL;
                    
                    if (class_sym) {
                        // Find the method
                        for (int m = 0; m < class_sym->method_count; m++) {
                            if (strcmp(class_sym->methods[m].name, member->member_name) == 0) {
                                method_sym = &class_sym->methods[m];
                                break;
                            }
                        }
                    }
                    
                    // Copy float method args into XMM registers
                    // Method param 0 → RDX (pos 1), param 1 → R8 (pos 2), param 2 → R9 (pos 3)
                    if (method_sym && method_sym->param_types) {
                        int mn = method_sym->param_count < (ABI_I_REG_COUNT - 1) ? method_sym->param_count : (ABI_I_REG_COUNT - 1);
                        for (int mi = 0; mi < mn; mi++) {
                            DataType pt = method_sym->param_types[mi];
                            if (pt == TYPE_FLOAT || pt == TYPE_F32 || pt == TYPE_F64) {
                                emit_asm(gen, "    movq xmm%d, %s  ; float method arg %d\n",
                                        mi + 1, ABI_I_REGS[mi + 1], mi);
                            }
                        }
                    }

                    // Check if method is virtual (has vtable_index >= 0)
                    if (method_sym && method_sym->vtable_index >= 0 && !method_sym->is_static) {
                        // Virtual method call - indirect through vtable
                        emit_asm(gen, "    ; Virtual method call: %s (vtable index %d)\n",
                                member->member_name, method_sym->vtable_index);
                        emit_asm(gen, "    mov rbx, [%s]  ; Load vtable pointer from object\n", ABI_I_REGS[0]);
                        emit_asm(gen, "    call [rbx + %d]  ; Indirect call through vtable\n",
                                method_sym->vtable_index * 8);
                    } else {
                        // Direct method call (static or non-virtual)
                        emit_asm(gen, "    call %s_%s\n", class_name, member->member_name);
                    }
                } else {
                    emit_asm(gen, "    ; ERROR: Cannot determine class for method call\n");
                }

                emit_asm(gen, "    add rsp, %d  ; Clean up stack\n", method_stack_size);

                // Result is in rax
                if (strcmp(reg, "rax") != 0) {
                    emit_asm(gen, "    mov %s, rax\n", reg);
                }
            }
            break;
        }
        case EXPR_THIS:
            // Load 'this' pointer from the global variable
            emit_asm(gen, "    mov %s, [rel this_ptr]  ; Load 'this' pointer\n", reg);
            break;
        case EXPR_SUPER: {
            SuperExpr* super_expr = &expr->as.super_expr;
            
            if (super_expr->is_method_call) {
                // Calculate stack space
                int reg_args_limit = ABI_I_REG_COUNT - 1;
                int extra_args = (super_expr->arg_count > reg_args_limit) ? (super_expr->arg_count - reg_args_limit) : 0;
                int stack_size = ABI_SHADOW_SPACE + (extra_args * 8);
                if (stack_size % 16 != 0) stack_size += 8;
                
                emit_asm(gen, "    sub rsp, %d\n", stack_size);
                
                // Push extra arguments onto stack
                for (int i = super_expr->arg_count - 1; i >= reg_args_limit; i--) {
                    generate_asm_expr(gen, super_expr->arguments[i], "rax", symbols);
                    int offset = ABI_SHADOW_SPACE + (i - reg_args_limit) * 8;
                    emit_asm(gen, "    mov [rsp + %d], rax\n", offset);
                }
                
                // First argument is always 'this' for method calls
                emit_asm(gen, "    mov %s, [rel this_ptr]\n", ABI_I_REGS[0]);
                
                // Load remaining arguments into registers
                int s_args = super_expr->arg_count < reg_args_limit ? super_expr->arg_count : reg_args_limit;
                for (int i = 0; i < s_args; i++) {
                    generate_asm_expr(gen, super_expr->arguments[i], ABI_I_REGS[i + 1], symbols);
                }
                
                // Call parent's method directly (no virtual dispatch)
                // Method name format: __casprix_ParentClassName_methodName
                if (gen->current_class && gen->current_class->parent_class) {
                    emit_asm(gen, "    call __casprix_%s_%s\n",
                            gen->current_class->parent_class, super_expr->member_name);
                } else {
                    emit_asm(gen, "    ; ERROR: No parent class for Super.%s()\n",
                            super_expr->member_name);
                }
                
                emit_asm(gen, "    add rsp, %d\n", stack_size);
                emit_asm(gen, "    mov %s, rax\n", reg);
            } else {
                // Super field access: Super.field
                // Load this pointer, then access field at parent's offset
                emit_asm(gen, "    mov rax, [rel this_ptr]\n");
                
                if (gen->current_class && gen->current_class->parent_class) {
                    ClassSymbol* parent_class = lookup_class(symbols, gen->current_class->parent_class);
                    if (parent_class) {
                        FieldSymbol* field = find_field(parent_class, super_expr->member_name);
                        if (field) {
                            emit_asm(gen, "    mov %s, [rax + %d]  ; Super.%s\n",
                                    reg, field->offset, super_expr->member_name);
                        } else {
                            emit_asm(gen, "    ; ERROR: Field '%s' not found in parent\n",
                                    super_expr->member_name);
                            emit_asm(gen, "    mov %s, 0\n", reg);
                        }
                    } else {
                        emit_asm(gen, "    ; ERROR: Parent class not found\n");
                        emit_asm(gen, "    mov %s, 0\n", reg);
                    }
                } else {
                    emit_asm(gen, "    ; ERROR: No parent class for Super.%s\n",
                            super_expr->member_name);
                    emit_asm(gen, "    mov %s, 0\n", reg);
                }
            }
            break;
        }
        case EXPR_NEW: {
            // ARC/Class-based new
            NewExpr* new_expr = &expr->as.new_expr;
            MethodSymbol* ctor = NULL;

            // Look up the class to get instance size
            ClassSymbol* class_sym = lookup_class(symbols, new_expr->class_name);
            if (!class_sym) {
                emit_asm(gen, "    ; ERROR: Class '%s' not found\n", new_expr->class_name);
                emit_asm(gen, "    mov %s, 0\n", reg);
                break;
            }

            ctor = find_constructor_symbol(class_sym);

            emit_asm(gen, "    ; Allocate ARC-managed object: New %s\n", new_expr->class_name);

            // Call arc_alloc_full(size, destructor, scanner)
            int arc_stack_adj = ABI_SHADOW_SPACE > 0 ? ABI_SHADOW_SPACE : 16;
            emit_asm(gen, "    sub rsp, %d  ; Shadow space/alignment\n", arc_stack_adj);
            emit_asm(gen, "    mov %s, %d  ; Object size\n", ABI_I_REGS[0], class_sym->instance_size);
            emit_asm(gen, "    lea %s, [__dtor_%s]  ; Destructor\n", ABI_I_REGS[1], new_expr->class_name);
            emit_asm(gen, "    xor %s, %s  ; Scanner (NULL for now)\n", ABI_I_REGS[2], ABI_I_REGS[2]);
            emit_asm(gen, "    call arc_alloc_full\n");
            emit_asm(gen, "    add rsp, %d\n", arc_stack_adj);
            // Object pointer is now in rax (user data, past ArcHeader)

            // Set VTable pointer at offset 0 (ObjectHeader)
            emit_asm(gen, "    lea rdx, [__vtable_%s]  ; Load vtable address\n", new_expr->class_name);
            emit_asm(gen, "    mov [rax], rdx  ; Set vtable pointer\n");

            // Fields are already zero-initialized by arc_alloc_full

            /* Save object pointer in a frame slot — avoids clobbering callee-saved
               registers (r14/r15) without a matching prologue push. */
            int new_this_off = alloc_local(gen, "__cpx_new_this");
            emit_asm(gen, "    mov [rbp - %d], rax  ; save new object ptr\n", new_this_off);

            // Allocate shadow space + space for stack args (must be 16-byte aligned)
            int c_reg_limit = ABI_I_REG_COUNT - 1; // 1 for 'this'
            int c_stack_args = new_expr->arg_count > c_reg_limit ? new_expr->arg_count - c_reg_limit : 0;
            int c_stack_size = ABI_SHADOW_SPACE + (c_stack_args * 8);
            if (c_stack_size % 16 != 0) c_stack_size += 8;
            emit_asm(gen, "    sub rsp, %d  ; Shadow space + stack args\n", c_stack_size);

            // Evaluate and place arguments
            int c_r_args = new_expr->arg_count < c_reg_limit ? new_expr->arg_count : c_reg_limit;
            for (int i = 0; i < c_r_args; i++) {
                generate_asm_expr(gen, new_expr->arguments[i], ABI_I_REGS[i + 1], symbols);
            }
            // Handle args on stack
            for (int i = c_reg_limit; i < new_expr->arg_count; i++) {
                generate_asm_expr(gen, new_expr->arguments[i], "rax", symbols);
                emit_asm(gen, "    mov [rsp + %d], rax  ; Arg %d on stack\n",
                        ABI_SHADOW_SPACE + (i - c_reg_limit) * 8, i);
            }

            // Object pointer (this) goes in the first argument register
            emit_asm(gen, "    mov %s, [rbp - %d]  ; Load object pointer (this)\n", ABI_I_REGS[0], new_this_off);

            // Copy float constructor args into XMM registers
            if (ctor && ctor->param_types) {
                int cn = ctor->param_count < (ABI_I_REG_COUNT - 1) ? ctor->param_count : (ABI_I_REG_COUNT - 1);
                for (int ci = 0; ci < cn; ci++) {
                    DataType pt = ctor->param_types[ci];
                    if (pt == TYPE_FLOAT || pt == TYPE_F32 || pt == TYPE_F64) {
                        emit_asm(gen, "    movq xmm%d, %s  ; float ctor arg %d\n",
                                ci + 1, ABI_I_REGS[ci + 1], ci);
                    }
                }
            }

            // Call constructor
            if (ctor) {
                emit_asm(gen, "    call %s_%s\n", new_expr->class_name, ctor->name);
            }

            emit_asm(gen, "    add rsp, %d  ; Clean up stack\n", c_stack_size);

            // Constructor returns object pointer in rax
            if (strcmp(reg, "rax") != 0) {
                emit_asm(gen, "    mov %s, rax  ; Move result to target register\n", reg);
            }
            break;
        }
        case EXPR_INDEX: {
            IndexExpr* index_expr = &expr->as.index;

            emit_asm(gen, "    ; Array indexing\n");

            // Evaluate the array expression (gets base address)
            generate_asm_expr(gen, index_expr->array, "rax", symbols);
            emit_asm(gen, "    push rax  ; Save array base address\n");

            // Evaluate the index expression
            generate_asm_expr(gen, index_expr->index, "rbx", symbols);

            // Pop array base address
            emit_asm(gen, "    pop rax   ; Restore array base address\n");

            // Calculate address: base_address + (index * 8)
            // Arrays are stored with layout: [length][data_ptr]
            // So data is at offset 8 from base
            emit_asm(gen, "    mov rax, [rax + 8]  ; Get data pointer\n");
            emit_asm(gen, "    lea %s, [rax + rbx*8]  ; Calculate element address\n", reg);
            emit_asm(gen, "    mov %s, [%s]  ; Load element value\n", reg, reg);
            break;
        }
        case EXPR_STATIC_ACCESS: {
            StaticAccessExpr* static_access = &expr->as.static_access;

            if (static_access->is_method_call) {
                // Static method call: ClassName.staticMethod(args)
                emit_asm(gen, "    ; Static method call: %s.%s\n",
                        static_access->class_name, static_access->member_name);

                // Calculate stack space needed
                int extra_args = (static_access->arg_count > ABI_I_REG_COUNT) ? (static_access->arg_count - ABI_I_REG_COUNT) : 0;
                int stack_size = ABI_SHADOW_SPACE + (extra_args * 8);
                if (stack_size % 16 != 0) stack_size += 8;

                emit_asm(gen, "    sub rsp, %d\n", stack_size);

                // Push extra arguments onto stack
                for (int i = static_access->arg_count - 1; i >= ABI_I_REG_COUNT; i--) {
                    generate_asm_expr(gen, static_access->arguments[i], "rax", symbols);
                    int offset = ABI_SHADOW_SPACE + (i - ABI_I_REG_COUNT) * 8;
                    emit_asm(gen, "    mov [rsp + %d], rax\n", offset);
                }

                // Load first arguments into registers
                int n_reg = static_access->arg_count < ABI_I_REG_COUNT ? static_access->arg_count : ABI_I_REG_COUNT;
                for (int i = 0; i < n_reg; i++) {
                    generate_asm_expr(gen, static_access->arguments[i], ABI_I_REGS[i], symbols);
                }

                // Call static method with mangled name
                emit_asm(gen, "    call __static_%s_%s\n",
                        static_access->class_name, static_access->member_name);
                emit_asm(gen, "    add rsp, %d\n", stack_size);
                emit_asm(gen, "    mov %s, rax\n", reg);
            } else {
                // Static field access: ClassName.staticField
                emit_asm(gen, "    ; Static field access: %s.%s\n",
                        static_access->class_name, static_access->member_name);
                emit_asm(gen, "    mov %s, [rel __static_%s_%s]\n",
                        reg, static_access->class_name, static_access->member_name);
            }
            break;
        }
        case EXPR_LAMBDA: {
            // Lambda/closure expression
            LambdaExpr* lambda = &expr->as.lambda;
            emit_asm(gen, "    ; Lambda expression (closure_id=%d)\n", lambda->closure_id);

            if (lambda->capture_count > 0 && lambda_value_supported(lambda)) {
                char slot_name[64];
                int base_off;

                format_closure_slot_name(lambda->closure_id, 0, slot_name, sizeof(slot_name));
                base_off = find_local(gen, slot_name);

                emit_asm(gen, "    lea rax, [rel __lambda_%d]\n", lambda->closure_id);
                emit_asm(gen, "    mov [rbp - %d], rax\n", base_off);

                for (int i = 0; i < lambda->capture_count; i++) {
                    int slot_off;
                    format_closure_slot_name(lambda->closure_id, i + 1, slot_name, sizeof(slot_name));
                    slot_off = find_local(gen, slot_name);
                    emit_load_var_addr(gen, lambda->captured_vars[i], "rax");
                    emit_asm(gen, "    mov [rbp - %d], rax\n", slot_off);
                }

                emit_asm(gen, "    lea %s, [rbp - %d]\n", reg, base_off);
            } else if (lambda_value_supported(lambda)) {
                emit_asm(gen, "    lea %s, [rel __lambda_%d]\n", reg, lambda->closure_id);
            } else {
                emit_asm(gen, "    xor %s, %s\n", reg, reg);
            }
            break;
        }
        case EXPR_GENERIC_INST: {
            // Generic type instantiation: List<Int>, Map<String, Int>
            // At codegen time, this should have been resolved to a concrete type
            // by monomorphization. For now, just generate a comment.
            GenericInstExpr* generic = &expr->as.generic_inst;
            emit_asm(gen, "    ; Generic instantiation: %s<...>\n", generic->base_name);
            // Generic instantiation as an expression is typically used for:
            // 1. Type annotations (handled at compile time)
            // 2. Generic constructor calls (handled via New expression)
            // For now, load 0 as a placeholder
            emit_asm(gen, "    xor %s, %s\n", reg, reg);
            break;
        }
    }
}

static void generate_asm_declaration(AssemblyGenerator* gen, Stmt* stmt, SymbolTable* symbols) {
    DeclarationStmt* decl = &stmt->as.declaration;

    if (decl->initializer) {
        generate_asm_expr(gen, decl->initializer, "rax", symbols);
        emit_store_var(gen, decl->name, "rax");
    } else {
        int off = find_local(gen, decl->name);
        if (off)
            emit_asm(gen, "    mov qword [rbp - %d], 0\n", off);
        else {
            char global_name[256];
            format_global_var_symbol(decl->name, global_name, sizeof(global_name));
            emit_asm(gen, "    mov qword [rel %s], 0\n", global_name);
        }
    }

    /* ── ARC: Register the variable for drop tracking if it is reference-counted ── */
    if (decl->type == TYPE_CLASS || decl->type == TYPE_STRING || decl->type == TYPE_STRBUF) {
        int off = find_local(gen, decl->name);
        drop_planner_register(&gen->drop_ctx, decl->name,
                               off, DROP_ARC, NULL, false);
    }
}

static void generate_asm_assignment(AssemblyGenerator* gen, Stmt* stmt, SymbolTable* symbols) {
    AssignmentStmt* assign = &stmt->as.assignment;

    // Generate the value to assign (in rax)
    generate_asm_expr(gen, assign->value, "rax", symbols);

    // Handle different types of assignment targets
    if (assign->target->type == EXPR_VARIABLE) {
        // Simple variable assignment
        VariableExpr* var = &assign->target->as.variable;
        emit_store_var(gen, var->name, "rax");
    } else if (assign->target->type == EXPR_MEMBER_ACCESS) {
        // Member access assignment: obj.field = value
        MemberAccessExpr* member = &assign->target->as.member;

        // Save the value we're assigning (in rax) to a temporary location
        emit_asm(gen, "    push rax  ; Save value to assign\n");

        // Generate code to get the object pointer
        generate_asm_expr(gen, member->object, "rbx", symbols);

        // Restore the value to assign
        emit_asm(gen, "    pop rax  ; Restore value\n");

        // Determine the class of the object
        ClassSymbol* class_sym = NULL;
        if (member->object->class_name) {
            class_sym = lookup_class(symbols, member->object->class_name);
        } else if (member->object->type == EXPR_THIS) {
            class_sym = gen->current_class;
        }

        if (class_sym) {
            FieldSymbol* field = find_field(class_sym, member->member_name);
            if (field) {
                // Store value at: [object_ptr + offset]
                emit_asm(gen, "    mov [rbx + %d], rax  ; Assign to field %s\n",
                        field->offset, member->member_name);
            } else {
                emit_asm(gen, "    ; ERROR: Field '%s' not found in class '%s'\n",
                        member->member_name, class_sym->name);
            }
        } else {
            emit_asm(gen, "    ; ERROR: Cannot determine class for member assignment\n");
        }
    } else if (assign->target->type == EXPR_INDEX) {
        // Array index assignment: arr[i] = value
        IndexExpr* index_expr = &assign->target->as.index;

        // Save the value to assign
        emit_asm(gen, "    push rax  ; Save value to assign\n");

        // Evaluate the array expression
        generate_asm_expr(gen, index_expr->array, "rbx", symbols);
        emit_asm(gen, "    push rbx  ; Save array base\n");

        // Evaluate the index expression
        generate_asm_expr(gen, index_expr->index, "rcx", symbols);

        // Restore array base and value
        emit_asm(gen, "    pop rbx   ; Restore array base\n");
        emit_asm(gen, "    pop rax   ; Restore value\n");

        // Calculate address and store
        emit_asm(gen, "    mov rbx, [rbx + 8]  ; Get data pointer\n");
        emit_asm(gen, "    mov [rbx + rcx*8], rax  ; Store value at arr[index]\n");
    } else if (assign->target->type == EXPR_STATIC_ACCESS) {
        // Static field assignment: ClassName.staticField = value
        StaticAccessExpr* static_access = &assign->target->as.static_access;

        if (!static_access->is_method_call) {
            emit_asm(gen, "    ; Static field assignment: %s.%s\n",
                    static_access->class_name, static_access->member_name);
            emit_asm(gen, "    mov [rel __static_%s_%s], rax\n",
                    static_access->class_name, static_access->member_name);
        } else {
            emit_asm(gen, "    ; ERROR: Cannot assign to static method call\n");
        }
    } else {
        emit_asm(gen, "    ; ERROR: Invalid assignment target\n");
    }
}

static void generate_asm_print(AssemblyGenerator* gen, Stmt* stmt, SymbolTable* symbols) {
    PrintStmt* print = &stmt->as.print;
    
    DataType type = print->expression->data_type;
    
    // Calling convention: RCX, RDX (Windows) or RDI, RSI (Linux)
    // Stack must be 16-byte aligned before call
    int stack_adj = ABI_SHADOW_SPACE > 0 ? ABI_SHADOW_SPACE : 16;
    emit_asm(gen, "    sub rsp, %d\n", stack_adj);
    
    generate_asm_expr(gen, print->expression, "rax", symbols);
    
    switch (type) {
        case TYPE_INT:
            emit_asm(gen, "    lea %s, [rel fmt_int]\n", ABI_I_REGS[0]);
            emit_asm(gen, "    mov %s, rax\n", ABI_I_REGS[1]);
            emit_asm(gen, "    call printf\n");
            break;
        case TYPE_STRING:
            emit_asm(gen, "    lea %s, [rel fmt_str]\n", ABI_I_REGS[0]);
            emit_asm(gen, "    mov %s, rax\n", ABI_I_REGS[1]);
            emit_asm(gen, "    call printf\n");
            break;
        case TYPE_FLOAT:
            emit_asm(gen, "    lea %s, [rel fmt_float]\n", ABI_I_REGS[0]);
#ifdef _WIN32
            emit_asm(gen, "    movq xmm1, rax\n");
#else
            emit_asm(gen, "    movq xmm0, rax\n");
#endif
            emit_asm(gen, "    mov rax, 1\n");
            emit_asm(gen, "    call printf\n");
            break;
        case TYPE_BOOL: {
            emit_asm(gen, "    test rax, rax\n");
            char true_label[32], end_label[32];
            snprintf(true_label, sizeof(true_label), "L%d", gen->label_count++);
            snprintf(end_label, sizeof(end_label), "L%d", gen->label_count++);
            emit_asm(gen, "    jnz %s\n", true_label);
            emit_asm(gen, "    lea %s, [rel fmt_false]\n", ABI_I_REGS[0]);
            emit_asm(gen, "    jmp %s\n", end_label);
            emit_asm(gen, "%s:\n", true_label);
            emit_asm(gen, "    lea %s, [rel fmt_true]\n", ABI_I_REGS[0]);
            emit_asm(gen, "%s:\n", end_label);
            emit_asm(gen, "    call printf\n");
            break;
        }
        default:
            break;
    }
    
    emit_asm(gen, "    add rsp, %d\n", stack_adj); // Restore stack
}

static void generate_asm_if(AssemblyGenerator* gen, Stmt* stmt, SymbolTable* symbols) {
    IfStmt* if_stmt = &stmt->as.if_stmt;
    
    generate_asm_expr(gen, if_stmt->condition, "rax", symbols);
    emit_asm(gen, "    test rax, rax\n");
    
    char else_label[32], end_label[32];
    snprintf(else_label, sizeof(else_label), "L%d", gen->label_count++);
    snprintf(end_label, sizeof(end_label), "L%d", gen->label_count++);
    
    if (if_stmt->else_branch) {
        emit_asm(gen, "    jz %s\n", else_label);
    } else {
        emit_asm(gen, "    jz %s\n", end_label);
    }
    
    generate_asm_stmt(gen, if_stmt->then_branch, symbols);
    
    if (if_stmt->else_branch) {
        emit_asm(gen, "    jmp %s\n", end_label);
        emit_asm(gen, "%s:\n", else_label);
        generate_asm_stmt(gen, if_stmt->else_branch, symbols);
    }
    
    emit_asm(gen, "%s:\n", end_label);
}

static void generate_asm_while(AssemblyGenerator* gen, Stmt* stmt, SymbolTable* symbols) {
    WhileStmt* while_stmt = &stmt->as.while_stmt;
    
    char loop_label[32], end_label[32];
    snprintf(loop_label, sizeof(loop_label), "L%d", gen->label_count++);
    snprintf(end_label, sizeof(end_label), "L%d", gen->label_count++);
    
    // Push loop labels onto stack for break/continue
    if (gen->loop_depth < 32) {
        strcpy(gen->loop_start_labels[gen->loop_depth], loop_label);
        strcpy(gen->loop_end_labels[gen->loop_depth], end_label);
    }
    gen->loop_depth++;
    
    emit_asm(gen, "%s:\n", loop_label);
    generate_asm_expr(gen, while_stmt->condition, "rax", symbols);
    emit_asm(gen, "    test rax, rax\n");
    emit_asm(gen, "    jz %s\n", end_label);
    
    generate_asm_stmt(gen, while_stmt->body, symbols);
    emit_asm(gen, "    jmp %s\n", loop_label);
    emit_asm(gen, "%s:\n", end_label);
    
    // Pop loop labels from stack
    gen->loop_depth--;
}

static void generate_asm_for(AssemblyGenerator* gen, Stmt* stmt, SymbolTable* symbols) {
    ForStmt* for_stmt = &stmt->as.for_stmt;
    
    /* ── ARC: enter loop scope (loop variable + body-local variables) ── */
    drop_planner_enter_scope(&gen->drop_ctx);
    
    // Initialization
    generate_asm_expr(gen, for_stmt->initializer, "rax", symbols);
    emit_store_var(gen, for_stmt->variable, "rax");
    
    char loop_label[32], end_label[32], continue_label[32];
    snprintf(loop_label, sizeof(loop_label), "L%d", gen->label_count++);
    snprintf(continue_label, sizeof(continue_label), "L%d", gen->label_count++);
    snprintf(end_label, sizeof(end_label), "L%d", gen->label_count++);
    
    // Push loop labels onto stack for break/continue
    if (gen->loop_depth < 32) {
        strcpy(gen->loop_start_labels[gen->loop_depth], continue_label);
        strcpy(gen->loop_end_labels[gen->loop_depth], end_label);
    }
    gen->loop_depth++;
    
    emit_asm(gen, "%s:\n", loop_label);
    generate_asm_expr(gen, for_stmt->condition, "rax", symbols);
    emit_asm(gen, "    test rax, rax\n");
    emit_asm(gen, "    jz %s\n", end_label);
    
    generate_asm_stmt(gen, for_stmt->body, symbols);
    
    // Continue jumps here, then increment
    emit_asm(gen, "%s:\n", continue_label);
    generate_asm_stmt(gen, for_stmt->increment, symbols);
    
    emit_asm(gen, "    jmp %s\n", loop_label);
    emit_asm(gen, "%s:\n", end_label);
    
    // Pop loop labels from stack
    gen->loop_depth--;
    
    /* ── ARC: drop any objects allocated in the loop scope ── */
    {
        int drop_count = 0;
        const DropEntry* drops = drop_planner_get_scope_drops(&gen->drop_ctx, &drop_count);
        emit_scope_drops(gen, drops, drop_count);
    }
    drop_planner_exit_scope(&gen->drop_ctx);
}

static void generate_asm_function(AssemblyGenerator* gen, Stmt* stmt, SymbolTable* symbols) {
    FunctionStmt* func = &stmt->as.function;

    /* ── Per-function stack frame ── */
    begin_local_frame(gen);

    /* Pre-allocate stack slots for ALL params (first 4 in regs, rest on stack) */
    for (int j = 0; j < func->param_count; j++)
        alloc_local(gen, func->parameters[j].name);

    /* Scan body for all local declarations */
    prescan_locals(gen, func->body);

    /* Emit label + prologue with correct frame size */
    emit_asm(gen, "\n__casprix_%s:\n", func->name);
    emit_asm(gen, "    push rbp\n");
    emit_asm(gen, "    mov rbp, rsp\n");
    emit_prologue_stack(gen);

    /* Store register parameters into stack slots */
    {
        int reg_n = func->param_count < ABI_I_REG_COUNT ? func->param_count : ABI_I_REG_COUNT;
        for (int j = 0; j < reg_n; j++) {
            DataType pt = func->parameters[j].type;
            int off = find_local(gen, func->parameters[j].name);
            if (pt == TYPE_FLOAT || pt == TYPE_F32 || pt == TYPE_F64)
                emit_asm(gen, "    movq [rbp - %d], xmm%d\n", off, j);
            else
                emit_asm(gen, "    mov [rbp - %d], %s\n", off, ABI_I_REGS[j]);
        }
    }

    /* Stack parameters (ABI_I_REG_COUNT+) */
    for (int i = ABI_I_REG_COUNT; i < func->param_count; i++) {
        int stack_base = ABI_SHADOW_SPACE > 0 ? 48 : 16;
        int stack_offset = stack_base + (i - ABI_I_REG_COUNT) * 8;
        int off = find_local(gen, func->parameters[i].name);
        emit_asm(gen, "    mov rax, [rbp + %d]\n", stack_offset);
        if (off)
            emit_asm(gen, "    mov [rbp - %d], rax\n", off);
    }

    /* ── ARC: Enter function scope and register ARC parameters ── */
    drop_planner_enter_scope(&gen->drop_ctx);
    for (int j = 0; j < func->param_count; j++) {
        DataType pt = func->parameters[j].type;
        if (pt == TYPE_CLASS || pt == TYPE_STRING || pt == TYPE_STRBUF) {
            int off = find_local(gen, func->parameters[j].name);
            drop_planner_register(&gen->drop_ctx, func->parameters[j].name,
                                   off, DROP_ARC, NULL, true);
        }
    }

    generate_asm_stmt(gen, func->body, symbols);

    /* ── ARC: Drop all remaining function-scope variables at function exit ── */
    {
        int drop_count = 0;
        const DropEntry* drops = drop_planner_get_scope_drops(&gen->drop_ctx, &drop_count);
        emit_scope_drops(gen, drops, drop_count);
    }
    drop_planner_exit_scope(&gen->drop_ctx);

    if (func->return_type == TYPE_VOID) {
        emit_asm(gen, "    mov rax, 0\n");
    }

    emit_asm(gen, "    leave\n");
    emit_asm(gen, "    ret\n");
}

static void generate_asm_return(AssemblyGenerator* gen, Stmt* stmt, SymbolTable* symbols) {
    ReturnStmt* ret = &stmt->as.return_stmt;

    if (ret->value) {
        generate_asm_expr(gen, ret->value, "rax", symbols);
        /* Save return value while we emit drops */
        emit_asm(gen, "    push rax  ; save return value across drops\n");
    } else {
        emit_asm(gen, "    mov rax, 0\n");
    }

    /* ── ARC: flush ALL pending drops from every scope to function root ── */
    {
        int drop_count = 0;
        const DropEntry* drops = drop_planner_get_drops_to_scope(
            &gen->drop_ctx, 0, &drop_count);
        emit_scope_drops(gen, drops, drop_count);
    }

    if (ret->value) {
        emit_asm(gen, "    pop rax   ; restore return value\n");
    }
    emit_asm(gen, "    leave\n");
    emit_asm(gen, "    ret\n");
}

static void generate_asm_class(AssemblyGenerator* gen, Stmt* stmt, SymbolTable* symbols) {
    ClassStmt* class_stmt = &stmt->as.class_stmt;

    // Look up the class symbol to get field offsets
    ClassSymbol* class_sym = lookup_class(symbols, class_stmt->name);
    if (!class_sym) {
        emit_asm(gen, "    ; ERROR: Class '%s' not found in symbol table\n", class_stmt->name);
        return;
    }

    // Set current class context
    ClassSymbol* prev_class = gen->current_class;
    gen->current_class = class_sym;

    emit_asm(gen, "\n; Class: %s\n", class_stmt->name);
    emit_asm(gen, "; Instance size: %d bytes\n", class_sym->instance_size);
    emit_asm(gen, "; Fields: %d\n", class_sym->field_count);
    for (int i = 0; i < class_sym->field_count; i++) {
        emit_asm(gen, ";   %s: %s (offset %d)\n",
                class_sym->fields[i].name,
                type_to_string(class_sym->fields[i].type),
                class_sym->fields[i].offset);
    }

    // Generate methods
    for (int i = 0; i < class_stmt->method_count; i++) {
        MethodDecl* method = &class_stmt->methods[i];

        // Mangled method name: __static_ClassName_methodName for static, ClassName_methodName for instance
        char mangled_name[256];
        if (method->is_static) {
            snprintf(mangled_name, sizeof(mangled_name), "__static_%s_%s", class_stmt->name, method->name);
        } else {
            snprintf(mangled_name, sizeof(mangled_name), "%s_%s", class_stmt->name, method->name);
        }

        /* Abstract methods have no body — emit a comment stub and skip code generation */
        if (method->is_abstract || !method->body) {
            emit_asm(gen, "; [abstract] %s::%s — no body generated\n",
                     class_stmt->name, method->name);
            continue;
        }

        /* ── Per-method stack frame ── */
        begin_local_frame(gen);

        for (int j = 0; j < method->param_count; j++)
            alloc_local(gen, method->parameters[j].name);
        prescan_locals(gen, method->body);

        /* Now frame_size is known, emit label + prologue */
        emit_asm(gen, "\n%s:\n", mangled_name);
        emit_asm(gen, "    push rbp\n");
        emit_asm(gen, "    mov rbp, rsp\n");
        emit_prologue_stack(gen);

        if (!method->is_static) {
            /* 'this' pointer comes in the first argument register */
            emit_asm(gen, "    mov [rel this_ptr], %s\n", ABI_I_REGS[0]);
        }

        /* Store parameters into stack slots */
        int first_arg_idx = method->is_static ? 0 : 1;
        int reg_param_count = method->param_count < (ABI_I_REG_COUNT - first_arg_idx) ? 
                              method->param_count : (ABI_I_REG_COUNT - first_arg_idx);

        for (int j = 0; j < reg_param_count; j++) {
            DataType pt = method->parameters[j].type;
            int off = find_local(gen, method->parameters[j].name);
            const char* reg = ABI_I_REGS[j + first_arg_idx];
            if (pt == TYPE_FLOAT || pt == TYPE_F32 || pt == TYPE_F64)
                emit_asm(gen, "    movq [rbp - %d], xmm%d\n", off, j + first_arg_idx);
            else
                emit_asm(gen, "    mov [rbp - %d], %s\n", off, reg);
        }

        for (int j = reg_param_count; j < method->param_count; j++) {
            // Stack arguments start after return address and saved RBP
            // Windows: after 32-byte shadow space (total 48-byte offset)
            // Linux: immediately after RBP (total 16-byte offset)
            int stack_base = ABI_SHADOW_SPACE > 0 ? 48 : 16;
            int stack_offset = stack_base + (j - reg_param_count) * 8;
            int off = find_local(gen, method->parameters[j].name);
            emit_asm(gen, "    mov rax, [rbp + %d]\n", stack_offset);
            emit_asm(gen, "    mov [rbp - %d], rax\n", off);
        }


        /* ── ARC: Enter method scope and register ARC parameters ── */
        drop_planner_enter_scope(&gen->drop_ctx);
        for (int j = 0; j < method->param_count; j++) {
            DataType pt = method->parameters[j].type;
            if (pt == TYPE_CLASS || pt == TYPE_STRING || pt == TYPE_STRBUF) {
                int off = find_local(gen, method->parameters[j].name);
                drop_planner_register(&gen->drop_ctx, method->parameters[j].name,
                                       off, DROP_ARC, NULL, true);
            }
        }

        /* Generate method body */
        generate_asm_stmt(gen, method->body, symbols);

        /* ── ARC: Drop any remaining method-scope variables ── */
        {
            int drop_count = 0;
            const DropEntry* drops = drop_planner_get_scope_drops(&gen->drop_ctx, &drop_count);
            emit_scope_drops(gen, drops, drop_count);
        }
        drop_planner_exit_scope(&gen->drop_ctx);

        /* Epilogue */
        if (method->is_constructor) {
            emit_asm(gen, "    mov rax, [rel this_ptr]  ; Return 'this' from constructor\n");
        } else if (method->return_type == TYPE_VOID) {
            emit_asm(gen, "    mov rax, 0\n");
        }

        emit_asm(gen, "    leave\n");
        emit_asm(gen, "    ret\n");
    }

    // Generate destructor stub for ARC cleanup
    emit_asm(gen, "\n; Destructor for %s (releases ref-counted fields)\n", class_stmt->name);
    emit_asm(gen, "__dtor_%s:\n", class_stmt->name);
    emit_asm(gen, "    push rbp\n");
    emit_asm(gen, "    mov rbp, rsp\n");
    emit_asm(gen, "    ; rcx = pointer to object user data\n");
    // Release any CLASS-typed fields (they hold ARC references)
    for (int i = 0; i < class_sym->field_count; i++) {
        if (class_sym->fields[i].type == TYPE_CLASS ||
            class_sym->fields[i].type == TYPE_STRING ||
            class_sym->fields[i].type == TYPE_STRBUF) {
            emit_asm(gen, "    ; Release field '%s' at offset %d\n",
                    class_sym->fields[i].name, class_sym->fields[i].offset);
            emit_asm(gen, "    push %s  ; Save object ptr\n", ABI_I_REGS[0]);
            emit_asm(gen, "    mov %s, [%s + %d]  ; Load field ref\n",
                    ABI_I_REGS[0], ABI_I_REGS[0], class_sym->fields[i].offset);
            emit_asm(gen, "    test %s, %s  ; Check for NULL\n", ABI_I_REGS[0], ABI_I_REGS[0]);
            emit_asm(gen, "    jz .dtor_%s_skip_%d\n", class_stmt->name, i);
            int dtor_stack_adj = ABI_SHADOW_SPACE > 0 ? ABI_SHADOW_SPACE : 16;
            emit_asm(gen, "    sub rsp, %d\n", dtor_stack_adj);
            emit_asm(gen, "    call arc_release\n");
            emit_asm(gen, "    add rsp, %d\n", dtor_stack_adj);
            emit_asm(gen, ".dtor_%s_skip_%d:\n", class_stmt->name, i);
            emit_asm(gen, "    pop %s  ; Restore object ptr\n", ABI_I_REGS[0]);
        }
    }
    emit_asm(gen, "    pop rbp\n");
    emit_asm(gen, "    ret\n");

    // Restore previous class context
    gen->current_class = prev_class;
}

static void emit_lambda_function(AssemblyGenerator* gen, LambdaExpr* lambda, SymbolTable* symbols) {
    if (!lambda_direct_call_supported(lambda) || (!lambda->expr_body && !lambda->block_body)) {
        return;
    }

    begin_local_frame(gen);

    for (int j = 0; j < lambda->capture_count; j++) {
        if (lambda->captured_vars && lambda->captured_vars[j]) {
            alloc_local(gen, lambda->captured_vars[j]);
        }
    }
    for (int j = 0; j < lambda->param_count; j++) {
        if (lambda->parameters[j].name) {
            alloc_local(gen, lambda->parameters[j].name);
        }
    }
    if (lambda->block_body) {
        prescan_locals(gen, lambda->block_body);
    }

    emit_asm(gen, "\n__lambda_%d:\n", lambda->closure_id);
    emit_asm(gen, "    push rbp\n");
    emit_asm(gen, "    mov rbp, rsp\n");
    emit_prologue_stack(gen);

    {
        int total_params = lambda->capture_count + lambda->param_count;
        int n = total_params < 4 ? total_params : 4;
        const char* regs[] = {"rcx", "rdx", "r8", "r9"};
        for (int j = 0; j < n; j++) {
            const char* param_name = NULL;
            DataType pt = lambda_call_arg_type(lambda, j);
            int off;
            if (j < lambda->capture_count) {
                param_name = lambda->captured_vars[j];
            } else {
                param_name = lambda->parameters[j - lambda->capture_count].name;
            }
            off = find_local(gen, param_name);
            if (j < lambda->capture_count) {
                emit_asm(gen, "    mov rax, [%s]\n", regs[j]);
                emit_asm(gen, "    mov [rbp - %d], rax\n", off);
            } else if (pt == TYPE_FLOAT || pt == TYPE_F32 || pt == TYPE_F64) {
                emit_asm(gen, "    movq [rbp - %d], xmm%d\n", off, j);
            } else {
                emit_asm(gen, "    mov [rbp - %d], %s\n", off, regs[j]);
            }
        }
    }

    for (int j = 4; j < lambda->capture_count + lambda->param_count; j++) {
        int stack_offset = 48 + (j - 4) * 8;
        const char* param_name = (j < lambda->capture_count)
            ? lambda->captured_vars[j]
            : lambda->parameters[j - lambda->capture_count].name;
        int off = find_local(gen, param_name);
        emit_asm(gen, "    mov rax, [rbp + %d]\n", stack_offset);
        if (j < lambda->capture_count) {
            emit_asm(gen, "    mov rax, [rax]\n");
        }
        emit_asm(gen, "    mov [rbp - %d], rax\n", off);
    }

    if (lambda->is_expression && lambda->expr_body) {
        generate_asm_expr(gen, lambda->expr_body, "rax", symbols);
    } else if (lambda->block_body) {
        generate_asm_stmt(gen, lambda->block_body, symbols);
        emit_asm(gen, "    mov rax, 0\n");
    }
    emit_asm(gen, "    leave\n");
    emit_asm(gen, "    ret\n");
}

static void emit_lambda_functions_from_expr(AssemblyGenerator* gen, Expr* expr, SymbolTable* symbols) {
    if (!expr) return;

    switch (expr->type) {
        case EXPR_BINARY:
            emit_lambda_functions_from_expr(gen, expr->as.binary.left, symbols);
            emit_lambda_functions_from_expr(gen, expr->as.binary.right, symbols);
            break;
        case EXPR_UNARY:
            emit_lambda_functions_from_expr(gen, expr->as.unary.operand, symbols);
            break;
        case EXPR_CALL:
            emit_lambda_functions_from_expr(gen, expr->as.call.callee, symbols);
            for (int i = 0; i < expr->as.call.arg_count; i++) {
                emit_lambda_functions_from_expr(gen, expr->as.call.arguments[i], symbols);
            }
            break;
        case EXPR_MEMBER_ACCESS:
            emit_lambda_functions_from_expr(gen, expr->as.member.object, symbols);
            for (int i = 0; i < expr->as.member.arg_count; i++) {
                emit_lambda_functions_from_expr(gen, expr->as.member.arguments[i], symbols);
            }
            break;
        case EXPR_INDEX:
            emit_lambda_functions_from_expr(gen, expr->as.index.array, symbols);
            emit_lambda_functions_from_expr(gen, expr->as.index.index, symbols);
            break;
        case EXPR_NEW:
            for (int i = 0; i < expr->as.new_expr.arg_count; i++) {
                emit_lambda_functions_from_expr(gen, expr->as.new_expr.arguments[i], symbols);
            }
            break;
        case EXPR_STATIC_ACCESS:
            for (int i = 0; i < expr->as.static_access.arg_count; i++) {
                emit_lambda_functions_from_expr(gen, expr->as.static_access.arguments[i], symbols);
            }
            break;
        case EXPR_SUPER:
            for (int i = 0; i < expr->as.super_expr.arg_count; i++) {
                emit_lambda_functions_from_expr(gen, expr->as.super_expr.arguments[i], symbols);
            }
            break;
        case EXPR_LAMBDA:
            emit_lambda_function(gen, &expr->as.lambda, symbols);
            if (expr->as.lambda.expr_body) {
                emit_lambda_functions_from_expr(gen, expr->as.lambda.expr_body, symbols);
            }
            if (expr->as.lambda.block_body) {
                emit_lambda_functions_from_stmt(gen, expr->as.lambda.block_body, symbols);
            }
            break;
        default:
            break;
    }
}

static void emit_lambda_functions_from_stmt(AssemblyGenerator* gen, Stmt* stmt, SymbolTable* symbols) {
    if (!stmt) return;

    switch (stmt->type) {
        case STMT_DECLARATION:
        case STMT_CONST_DECL:
            emit_lambda_functions_from_expr(gen, stmt->as.declaration.initializer, symbols);
            break;
        case STMT_ASSIGNMENT:
            emit_lambda_functions_from_expr(gen, stmt->as.assignment.target, symbols);
            emit_lambda_functions_from_expr(gen, stmt->as.assignment.value, symbols);
            break;
        case STMT_PRINT:
            emit_lambda_functions_from_expr(gen, stmt->as.print.expression, symbols);
            break;
        case STMT_EXPR:
            emit_lambda_functions_from_expr(gen, stmt->as.expr_stmt.expression, symbols);
            break;
        case STMT_RETURN:
            emit_lambda_functions_from_expr(gen, stmt->as.return_stmt.value, symbols);
            break;
        case STMT_IF:
            emit_lambda_functions_from_expr(gen, stmt->as.if_stmt.condition, symbols);
            emit_lambda_functions_from_stmt(gen, stmt->as.if_stmt.then_branch, symbols);
            emit_lambda_functions_from_stmt(gen, stmt->as.if_stmt.else_branch, symbols);
            break;
        case STMT_WHILE:
            emit_lambda_functions_from_expr(gen, stmt->as.while_stmt.condition, symbols);
            emit_lambda_functions_from_stmt(gen, stmt->as.while_stmt.body, symbols);
            break;
        case STMT_FOR:
            emit_lambda_functions_from_expr(gen, stmt->as.for_stmt.initializer, symbols);
            emit_lambda_functions_from_expr(gen, stmt->as.for_stmt.condition, symbols);
            emit_lambda_functions_from_stmt(gen, stmt->as.for_stmt.increment, symbols);
            emit_lambda_functions_from_stmt(gen, stmt->as.for_stmt.body, symbols);
            break;
        case STMT_FOR_IN:
            emit_lambda_functions_from_expr(gen, stmt->as.for_in_stmt.iterable, symbols);
            emit_lambda_functions_from_stmt(gen, stmt->as.for_in_stmt.body, symbols);
            break;
        case STMT_BLOCK:
            for (int i = 0; i < stmt->as.block.stmt_count; i++) {
                emit_lambda_functions_from_stmt(gen, stmt->as.block.statements[i], symbols);
            }
            break;
        case STMT_FUNCTION:
            emit_lambda_functions_from_stmt(gen, stmt->as.function.body, symbols);
            break;
        case STMT_CLASS:
            for (int i = 0; i < stmt->as.class_stmt.method_count; i++) {
                emit_lambda_functions_from_stmt(gen, stmt->as.class_stmt.methods[i].body, symbols);
            }
            break;
        case STMT_TRY:
            emit_lambda_functions_from_stmt(gen, stmt->as.try_stmt.try_body, symbols);
            for (int i = 0; i < stmt->as.try_stmt.catch_count; i++) {
                emit_lambda_functions_from_stmt(gen, stmt->as.try_stmt.catches[i].body, symbols);
            }
            emit_lambda_functions_from_stmt(gen, stmt->as.try_stmt.finally_body, symbols);
            break;
        case STMT_MATCH:
            emit_lambda_functions_from_expr(gen, stmt->as.match_stmt.subject, symbols);
            for (int i = 0; i < stmt->as.match_stmt.arm_count; i++) {
                emit_lambda_functions_from_stmt(gen, stmt->as.match_stmt.arms[i].body, symbols);
            }
            break;
        default:
            break;
    }
}

static void generate_asm_block(AssemblyGenerator* gen, Stmt* stmt, SymbolTable* symbols) {
    BlockStmt* block = &stmt->as.block;
    drop_planner_enter_scope(&gen->drop_ctx);
    
    for (int i = 0; i < block->stmt_count; i++) {
        generate_asm_stmt(gen, block->statements[i], symbols);
    }
    
    int drop_count = 0;
    const DropEntry* drops = drop_planner_get_scope_drops(&gen->drop_ctx, &drop_count);
    emit_scope_drops(gen, drops, drop_count);
    
    drop_planner_exit_scope(&gen->drop_ctx);
}

static void generate_asm_stmt(AssemblyGenerator* gen, Stmt* stmt, SymbolTable* symbols) {
    if (!stmt) return;
    
    switch (stmt->type) {
        case STMT_DECLARATION:
            generate_asm_declaration(gen, stmt, symbols);
            break;
        case STMT_ASSIGNMENT:
            generate_asm_assignment(gen, stmt, symbols);
            break;
        case STMT_PRINT:
            generate_asm_print(gen, stmt, symbols);
            break;
        case STMT_IF:
            generate_asm_if(gen, stmt, symbols);
            break;
        case STMT_WHILE:
            generate_asm_while(gen, stmt, symbols);
            break;
        case STMT_FOR:
            generate_asm_for(gen, stmt, symbols);
            break;
        case STMT_FUNCTION:
            generate_asm_function(gen, stmt, symbols);
            break;
        case STMT_RETURN:
            generate_asm_return(gen, stmt, symbols);
            break;
        case STMT_BLOCK:
            generate_asm_block(gen, stmt, symbols);
            break;
        case STMT_EXPR:
            generate_asm_expr(gen, stmt->as.expr_stmt.expression, "rax", symbols);
            break;
        case STMT_INCLUDE:
            // Include statements are processed during module resolution
            break;
        case STMT_BREAK:
            /* ── ARC: flush current scope's drops before jumping out ── */
            {
                int drop_count = 0;
                const DropEntry* drops = drop_planner_get_scope_drops(
                    &gen->drop_ctx, &drop_count);
                emit_scope_drops(gen, drops, drop_count);
            }
            if (gen->loop_depth > 0) {
                emit_asm(gen, "    jmp %s  ; break\n", gen->loop_end_labels[gen->loop_depth - 1]);
            } else {
                emit_asm(gen, "    ; ERROR: break outside of loop\n");
            }
            break;
        case STMT_CONTINUE:
            /* ── ARC: flush current scope's drops before continuing ── */
            {
                int drop_count = 0;
                const DropEntry* drops = drop_planner_get_scope_drops(
                    &gen->drop_ctx, &drop_count);
                emit_scope_drops(gen, drops, drop_count);
            }
            if (gen->loop_depth > 0) {
                emit_asm(gen, "    jmp %s  ; continue\n", gen->loop_start_labels[gen->loop_depth - 1]);
            } else {
                emit_asm(gen, "    ; ERROR: continue outside of loop\n");
            }
            break;
        case STMT_CLASS:
            generate_asm_class(gen, stmt, symbols);
            break;
        case STMT_EXTERN:
            // Extern declarations are handled in generate_assembly (extern emission)
            break;
        case STMT_STRUCT:
        case STMT_ENUM:
        case STMT_UNION:
            // Type declarations are compile-time only, no runtime code generated
            break;

        /* ── New statement types ── */

        case STMT_FOR_IN: {
            ForInStmt* fi = &stmt->as.for_in_stmt;
            /*
             * Lowering: for var in arr  →
             *   __cpx_for_in_arr_len = arr.length
             *   __cpx_for_in_idx     = 0
             *   loop: if idx >= len goto end
             *     var = arr[idx]   (via arc_retain + load)
             *     body
             *     idx++
             *     goto loop
             *   end:
             */
            int lb = gen->label_count++;
            char loop_lbl[32], end_lbl[32];
            snprintf(loop_lbl, sizeof(loop_lbl), "L%d", lb);
            snprintf(end_lbl,  sizeof(end_lbl),  "L%d", gen->label_count++);

            /* Store the iterable and its length in frame slots so they survive
               calls inside the loop body without clobbering callee-saved regs. */
            int fi_iter_off = alloc_local(gen, "__cpx_fi_iter");
            int fi_len_off  = alloc_local(gen, "__cpx_fi_len");
            emit_asm(gen, "    ; for-in loop over '%s'\n", fi->var_name);
            generate_asm_expr(gen, fi->iterable, "rax", symbols);
            emit_asm(gen, "    mov [rbp - %d], rax  ; save iterable ptr\n", fi_iter_off);
            emit_asm(gen, "    mov rax, [rax + 0]   ; load length\n");
            emit_asm(gen, "    mov [rbp - %d], rax  ; save length\n", fi_len_off);
            /* Allocate index on stack */
            int idx_off = alloc_local(gen, "__cpx_for_idx");
            int var_off = alloc_local(gen, fi->var_name);
            emit_asm(gen, "    mov qword [rbp - %d], 0  ; idx = 0\n", idx_off);

            /* Push loop labels for break/continue */
            if (gen->loop_depth < 32) {
                snprintf(gen->loop_start_labels[gen->loop_depth], 32, "%s", loop_lbl);
                snprintf(gen->loop_end_labels[gen->loop_depth], 32, "%s", end_lbl);
            }
            gen->loop_depth++;

            emit_asm(gen, "%s:\n", loop_lbl);
            emit_asm(gen, "    mov rax, [rbp - %d]  ; load idx\n", idx_off);
            emit_asm(gen, "    cmp rax, [rbp - %d]  ; idx < length\n", fi_len_off);
            emit_asm(gen, "    jge %s  ; idx >= length → end\n", end_lbl);
            /* Load arr[idx]: data starts at offset 8 (after length) */
            emit_asm(gen, "    mov rcx, [rbp - %d]  ; load iterable ptr\n", fi_iter_off);
            emit_asm(gen, "    lea rbx, [rcx + 8]   ; base of data array\n");
            emit_asm(gen, "    mov rcx, [rbx + rax*8]  ; load element\n");
            emit_asm(gen, "    mov [rbp - %d], rcx  ; store into loop var\n", var_off);

            generate_asm_stmt(gen, fi->body, symbols);

            emit_asm(gen, "    add qword [rbp - %d], 1  ; idx++\n", idx_off);
            emit_asm(gen, "    jmp %s\n", loop_lbl);
            emit_asm(gen, "%s:\n", end_lbl);
            gen->loop_depth--;
            break;
        }

        case STMT_MATCH: {
            MatchStmt* ms = &stmt->as.match_stmt;
            /* Store subject in a frame slot — avoids callee-saved register clobbering. */
            int ms_subj_off = alloc_local(gen, "__cpx_match_subj");
            generate_asm_expr(gen, ms->subject, "rax", symbols);
            emit_asm(gen, "    mov [rbp - %d], rax  ; save match subject\n", ms_subj_off);

            int end_id = gen->label_count++;
            char end_lbl[32];
            snprintf(end_lbl, sizeof(end_lbl), "L%d", end_id);

            char arm_lbl[32];
            for (int i = 0; i < ms->arm_count; i++) {
                MatchArm* arm = &ms->arms[i];
                snprintf(arm_lbl, sizeof(arm_lbl), "L%d", gen->label_count++);

                if (arm->pattern == NULL) {
                    /* Wildcard _ — always matches, emit body directly */
                    emit_asm(gen, "    ; match arm: wildcard\n");
                    generate_asm_stmt(gen, arm->body, symbols);
                    emit_asm(gen, "    jmp %s  ; end match\n", end_lbl);
                } else {
                    /* Compare subject to pattern */
                    generate_asm_expr(gen, arm->pattern, "rax", symbols);
                    emit_asm(gen, "    mov rcx, [rbp - %d]  ; load match subject\n", ms_subj_off);
                    emit_asm(gen, "    cmp rcx, rax\n");
                    emit_asm(gen, "    jne %s  ; pattern mismatch\n", arm_lbl);
                    generate_asm_stmt(gen, arm->body, symbols);
                    emit_asm(gen, "    jmp %s  ; end match\n", end_lbl);
                    emit_asm(gen, "%s:\n", arm_lbl);
                }
            }
            emit_asm(gen, "%s:\n", end_lbl);
            break;
        }

        case STMT_THROW: {
            ThrowStmt* th = &stmt->as.throw_stmt;
            /* Evaluate the thrown value */
            generate_asm_expr(gen, th->value, "rax", symbols);
            /* Store as current exception */
            emit_asm(gen, "    mov [rel cpx_current_exception], rax  ; store thrown value\n");
            /* longjmp to nearest try handler */
            emit_asm(gen, "    mov %s, [rel cpx_jmp_buf_ptr]  ; load jmp_buf ptr\n", ABI_I_REGS[0]);
            emit_asm(gen, "    test %s, %s\n", ABI_I_REGS[0], ABI_I_REGS[0]);
            emit_asm(gen, "    jz .no_handler_%d  ; if no handler, abort\n", gen->label_count);
            emit_asm(gen, "    mov %s, 1  ; setjmp return value\n", ABI_I_REGS[1]);
            int jump_stack_adj = ABI_SHADOW_SPACE > 0 ? ABI_SHADOW_SPACE : 16;
            emit_asm(gen, "    sub rsp, %d\n", jump_stack_adj);
            emit_asm(gen, "    call longjmp\n");
            emit_asm(gen, "    add rsp, %d\n", jump_stack_adj);
            emit_asm(gen, ".no_handler_%d:\n", gen->label_count++);
            emit_asm(gen, "    ; Unhandled exception — abort\n");
            emit_asm(gen, "    xor %s, %s\n", ABI_I_REGS[0], ABI_I_REGS[0]);
            emit_asm(gen, "    sub rsp, %d\n", jump_stack_adj);
            emit_asm(gen, "    call abort\n");
            emit_asm(gen, "    add rsp, %d\n", jump_stack_adj);
            break;
        }

        case STMT_TRY: {
            TryStmt* tr = &stmt->as.try_stmt;
            /*
             * try/catch using setjmp/longjmp:
             *   1. Save previous jmp_buf ptr
             *   2. Call setjmp into local buf; jump addr stored in cpx_jmp_buf_ptr
             *   3. If setjmp returns 0 → run try body
             *   4. If setjmp returns nonzero → exception thrown, run catch
             *   5. Restore previous jmp_buf ptr
             *   6. Run finally (always)
             */
            int try_id = gen->label_count++;
            char catch_lbl[32], finally_lbl[32], done_lbl[32];
            snprintf(catch_lbl,   sizeof(catch_lbl),   "L_catch_%d",   try_id);
            snprintf(finally_lbl, sizeof(finally_lbl), "L_finally_%d", try_id);
            snprintf(done_lbl,    sizeof(done_lbl),    "L_done_%d",    try_id);

            /* Allocate local jmp_buf (216 bytes on x64 Windows) */
            int jbuf_off = alloc_local(gen, "__cpx_jbuf");
            /* Grow frame by 216 bytes for jmp_buf (already allocated 8 bytes by alloc_local)
               — in a real impl we'd reserve 216; for simplicity reference as [rbp-jbuf_off] */

            emit_asm(gen, "    ; try block (id=%d)\n", try_id);
            /* Save old jmp_buf ptr */
            emit_asm(gen, "    mov rax, [rel cpx_jmp_buf_ptr]\n");
            emit_asm(gen, "    push rax  ; save old jmp_buf ptr\n");
            /* Set new jmp_buf */
            emit_asm(gen, "    lea rcx, [rbp - %d]  ; address of local jmp_buf\n", jbuf_off);
            emit_asm(gen, "    mov [rel cpx_jmp_buf_ptr], rcx\n");
            int jmp_stack_adj = ABI_SHADOW_SPACE > 0 ? ABI_SHADOW_SPACE : 16;
            emit_asm(gen, "    sub rsp, %d\n", jmp_stack_adj);
            emit_asm(gen, "    call setjmp\n");
            emit_asm(gen, "    add rsp, %d\n", jmp_stack_adj);
            emit_asm(gen, "    test rax, rax\n");
            emit_asm(gen, "    jnz %s  ; exception thrown — go to catch\n", catch_lbl);

            /* Try body */
            generate_asm_stmt(gen, tr->try_body, symbols);
            emit_asm(gen, "    jmp %s  ; no exception — skip catch\n", finally_lbl);

            /* Catch blocks */
            emit_asm(gen, "%s:\n", catch_lbl);
            emit_asm(gen, "    ; load thrown exception object\n");
            emit_asm(gen, "    mov rax, [rel cpx_current_exception]\n");
            for (int ci = 0; ci < tr->catch_count; ci++) {
                CatchClause* clause = &tr->catches[ci];
                if (clause->exception_var) {
                    int evar_off = alloc_local(gen, clause->exception_var);
                    emit_asm(gen, "    mov [rbp - %d], rax  ; bind '%s'\n",
                             evar_off, clause->exception_var);
                }
                generate_asm_stmt(gen, clause->body, symbols);
            }

            /* Finally block */
            emit_asm(gen, "%s:\n", finally_lbl);
            /* Restore previous jmp_buf ptr */
            emit_asm(gen, "    pop rax  ; restore old jmp_buf ptr\n");
            emit_asm(gen, "    mov [rel cpx_jmp_buf_ptr], rax\n");
            if (tr->finally_body) {
                generate_asm_stmt(gen, tr->finally_body, symbols);
            }
            emit_asm(gen, "%s:\n", done_lbl);
            break;
        }

        case STMT_TRAIT:
            /* Traits are purely compile-time — no runtime code */
            break;

        case STMT_CONST_DECL:
            /* Const declarations generate same code as regular declarations */
            generate_asm_declaration(gen, stmt, symbols);
            break;

        case STMT_IMPL:
            /* Impl blocks: generate each method as a standalone function */
            for (int i = 0; i < stmt->as.impl_stmt.method_count; i++) {
                generate_asm_stmt(gen, stmt->as.impl_stmt.methods[i], symbols);
            }
            break;
    }
}

void generate_assembly(AssemblyGenerator* gen, Stmt** statements, int count, SymbolTable* symbols) {
    CPX_LOG(CPX_LOG_DEBUG, CPX_LOG_CAT_CODEGEN, "Generating x86-64 assembly code");
    
    // Initialize variable list
    var_list.names = NULL;
    var_list.count = 0;
    var_list.capacity = 0;
    
    // First pass: collect all string literals and variables
    for (int i = 0; i < count; i++) {
        collect_strings_from_stmt(gen, statements[i]);
        collect_variables_from_stmt(statements[i]);
    }
    
    // Assembly header
    emit_asm(gen, "; Generated by Casprix Compiler\n");
    emit_asm(gen, "; Assemble with: nasm -f win64 (Windows) or nasm -f elf64 (Linux)\n");
    emit_asm(gen, "bits 64\n");
#ifdef _WIN32
    emit_asm(gen, "default rel\n\n");
#else
    emit_asm(gen, "default rel\n\n");
#endif
    
    // External functions
    emit_asm(gen, "extern printf\n");
    emit_asm(gen, "extern malloc\n");
    emit_asm(gen, "extern free\n");
    emit_asm(gen, "extern strcpy\n");
    emit_asm(gen, "extern setjmp\n");
    emit_asm(gen, "extern longjmp\n");
    emit_asm(gen, "extern abort\n");
    emit_asm(gen, "extern arc_retain\n");
    emit_asm(gen, "extern arc_release\n");
    emit_asm(gen, "extern obj_alloc\n");
    emit_asm(gen, "extern strcat\n");
    emit_asm(gen, "extern strlen\n");
    emit_asm(gen, "; ARC memory management\n");
    emit_asm(gen, "extern arc_alloc_full\n");
    emit_asm(gen, "extern arc_retain\n");
    emit_asm(gen, "extern arc_release\n");
    emit_asm(gen, "; Rc<T> / scope guard (drop planner)\n");
    emit_asm(gen, "extern rc_release\n");
    emit_asm(gen, "extern scope_guard_drop_extern\n\n");
    
    // Emit extern declarations for user-defined external functions
    for (int i = 0; i < count; i++) {
        if (statements[i]->type == STMT_EXTERN) {
            emit_asm(gen, "extern %s\n", statements[i]->as.extern_stmt.name);
        }
    }
    emit_asm(gen, "\n");
    
    // Data section
    emit_asm(gen, "section .data\n");
    
    // Format strings
    emit_asm(gen, "    fmt_int db \"%%lld\", 10, 0\n");
    emit_asm(gen, "    fmt_float db \"%%.6f\", 10, 0\n");
    emit_asm(gen, "    fmt_str db \"%%s\", 10, 0\n");
    emit_asm(gen, "    fmt_true db \"True\", 10, 0\n");
    emit_asm(gen, "    fmt_false db \"False\", 10, 0\n");
    
    // String literals
    for (int i = 0; i < gen->string_size; i++) {
        emit_data_string_literal(gen, i, gen->string_literals[i]);
    }
    emit_asm(gen, "\n");

    /* VTable data for classes.
     * Rules:
     *  1. Abstract methods have no label — skip them (leave slot as NULL for safety).
     *  2. For a method that this class does NOT define (inherited), walk up the
     *     parent chain in the symbol table to find the ancestor that owns the
     *     concrete body and emit that class's label.
     */
    for (int i = 0; i < count; i++) {
        if (statements[i]->type == STMT_CLASS) {
            ClassStmt* class_stmt = &statements[i]->as.class_stmt;
            ClassSymbol* class_sym = lookup_class(symbols, class_stmt->name);
            if (!class_sym) continue;

            emit_asm(gen, "; VTable for %s\n", class_stmt->name);
            emit_asm(gen, "__vtable_%s:\n", class_stmt->name);

            /* Emit one entry per virtual method in the class's method table */
            int virtual_count = 0;
            for (int j = 0; j < class_sym->method_count; j++) {
                MethodSymbol* method = &class_sym->methods[j];
                if (method->vtable_index < 0 || method->is_static || method->is_constructor) {
                    continue;
                }

                /* Skip if this method is abstract — there is no code label */
                if (method->is_abstract) {
                    emit_asm(gen, "    dq 0  ; abstract %s::%s — no implementation\n",
                             class_stmt->name, method->name);
                    virtual_count++;
                    continue;
                }

                /* Find which class in the hierarchy actually owns a concrete body.
                 * Walk up the parent chain: if class_sym itself declares this method
                 * in the AST body list, use class_stmt->name; otherwise check parents. */
                bool found_in_self = false;
                for (int k = 0; k < class_stmt->method_count; k++) {
                    if (strcmp(class_stmt->methods[k].name, method->name) == 0 &&
                        !class_stmt->methods[k].is_abstract) {
                        found_in_self = true;
                        break;
                    }
                }

                if (found_in_self) {
                    /* Overridden (or originally defined) in this class */
                    emit_asm(gen, "    dq %s_%s  ; vtable[%d]\n",
                             class_stmt->name, method->name, method->vtable_index);
                } else {
                    /* Inherited — find the nearest ancestor that has a concrete body */
                    const char* owner_name = NULL;
                    ClassSymbol* ancestor = class_sym->parent;
                    while (ancestor) {
                        MethodSymbol* am = find_method(ancestor, method->name);
                        if (am && !am->is_abstract) {
                            owner_name = ancestor->name;
                            break;
                        }
                        ancestor = ancestor->parent;
                    }
                    if (owner_name) {
                        emit_asm(gen, "    dq %s_%s  ; vtable[%d] inherited from %s\n",
                                 owner_name, method->name, method->vtable_index, owner_name);
                    } else {
                        emit_asm(gen, "    dq 0  ; vtable[%d] %s — no concrete impl found\n",
                                 method->vtable_index, method->name);
                    }
                }
                virtual_count++;
            }
            /* Placeholder if no virtual methods */
            if (virtual_count == 0) {
                emit_asm(gen, "    dq 0  ; No virtual methods\n");
            }
            emit_asm(gen, "\n");
        }
    }


    // BSS section for variables
    emit_asm(gen, "section .bss\n");
    // Special variable for 'this' pointer in class methods
    emit_asm(gen, "    this_ptr resq 1\n");
    // Exception handling globals (used by STMT_TRY / STMT_THROW)
    emit_asm(gen, "    cpx_current_exception resq 1  ; current thrown exception object\n");
    emit_asm(gen, "    cpx_jmp_buf_ptr       resq 1  ; pointer to active setjmp buffer\n");


    // Declare static fields from all classes
    for (int i = 0; i < count; i++) {
        if (statements[i]->type == STMT_CLASS) {
            ClassStmt* class_stmt = &statements[i]->as.class_stmt;
            for (int j = 0; j < class_stmt->field_count; j++) {
                if (class_stmt->fields[j].is_static) {
                    emit_asm(gen, "    __static_%s_%s resq 1  ; Static field\n",
                            class_stmt->name, class_stmt->fields[j].name);
                }
            }
        }
    }

    // Declare all collected variables
    for (int i = 0; i < var_list.count; i++) {
        {
            char global_name[256];
            format_global_var_symbol(var_list.names[i], global_name, sizeof(global_name));
            emit_asm(gen, "    %s resq 1\n", global_name);
        }
    }
    emit_asm(gen, "\n");
    
    // Clean up variable list
    for (int i = 0; i < var_list.count; i++) {
        free(var_list.names[i]);
    }
    if (var_list.names) {
        free(var_list.names);
    }
    
    // Code section
    emit_asm(gen, "section .text\n");
#ifdef _WIN32
    emit_asm(gen, "global main\n\n");
    // Main function (Windows uses main as entry point)
    emit_asm(gen, "main:\n");
#else
    emit_asm(gen, "global main\n\n");
    // Main function (using main for GCC linking)
    emit_asm(gen, "main:\n");
#endif
    emit_asm(gen, "    push rbp\n");
    emit_asm(gen, "    mov rbp, rsp\n");

    /* Set up stack frame for top-level local variables */
    begin_local_frame(gen);
    for (int i = 0; i < count; i++) {
        if (statements[i]->type != STMT_FUNCTION && statements[i]->type != STMT_CLASS) {
            prescan_locals(gen, statements[i]);
        }
    }
    emit_prologue_stack(gen);
    emit_asm(gen, "\n");

    // Initialize static fields with default values
    emit_asm(gen, "    ; Initialize static fields\n");
    for (int i = 0; i < count; i++) {
        if (statements[i]->type == STMT_CLASS) {
            ClassStmt* class_stmt = &statements[i]->as.class_stmt;
            for (int j = 0; j < class_stmt->field_count; j++) {
                if (class_stmt->fields[j].is_static && class_stmt->fields[j].default_value) {
                    generate_asm_expr(gen, class_stmt->fields[j].default_value, "rax", symbols);
                    emit_asm(gen, "    mov [rel __static_%s_%s], rax\n",
                            class_stmt->name, class_stmt->fields[j].name);
                }
            }
        }
    }
    emit_asm(gen, "\n");

    // Generate top-level statements (exclude functions and classes)
    for (int i = 0; i < count; i++) {
        if (statements[i]->type != STMT_FUNCTION && statements[i]->type != STMT_CLASS) {
            generate_asm_stmt(gen, statements[i], symbols);
        }
    }
    
    // Call the user's main function if it exists
    Symbol* main_symbol = lookup_symbol(symbols, "main");
    if (main_symbol && main_symbol->kind == SYMBOL_FUNCTION) {
        int main_stack_adj = ABI_SHADOW_SPACE > 0 ? ABI_SHADOW_SPACE : 16;
        emit_asm(gen, "    sub rsp, %d\n", main_stack_adj);
        emit_asm(gen, "    call __casprix_main\n");
        emit_asm(gen, "    add rsp, %d\n", main_stack_adj);
    }
    
    // Exit
    emit_asm(gen, "    mov rax, 0\n"); // return 0
    emit_asm(gen, "    leave\n");
    emit_asm(gen, "    ret\n\n");
    
    // Generate functions and class methods
    for (int i = 0; i < count; i++) {
        if (statements[i]->type == STMT_FUNCTION || statements[i]->type == STMT_CLASS) {
            generate_asm_stmt(gen, statements[i], symbols);
        }
    }

    for (int i = 0; i < count; i++) {
        emit_lambda_functions_from_stmt(gen, statements[i], symbols);
    }

    CPX_LOG(CPX_LOG_DEBUG, CPX_LOG_CAT_CODEGEN, "Assembly code generation complete");
}
