/**
 * Casperix Compiler - Escape Analysis Implementation
 *
 * Walks function-level AST to classify each local variable as
 * escaping or non-escaping.  Non-escaping values can be promoted
 * to stack allocation, eliminating ARC overhead.
 */

#include "escape_analysis.h"
#include "support/log.h"
#include "support/error.h"
#include <string.h>
#include <stdio.h>

/* ─── Lifecycle ─── */

void escape_analyzer_init(EscapeAnalyzer* ea) {
    memset(ea, 0, sizeof(EscapeAnalyzer));
    ea->current_scope = 0;
    ea->in_closure    = false;
}

void escape_analyzer_reset(EscapeAnalyzer* ea) {
    ea->count         = 0;
    ea->current_scope = 0;
    ea->in_closure    = false;
}

/* ─── Scope tracking ─── */

void escape_enter_scope(EscapeAnalyzer* ea) {
    ea->current_scope++;
}

void escape_exit_scope(EscapeAnalyzer* ea) {
    if (ea->current_scope > 0) ea->current_scope--;
}

/* ─── Registration & Lookup ─── */

EscapeInfo* escape_register_var(EscapeAnalyzer* ea, const char* name,
                                 bool is_parameter) {
    if (ea->count >= MAX_ESCAPE_ENTRIES) return NULL;

    EscapeInfo* info = &ea->entries[ea->count++];
    info->var_name      = name;
    info->scope_level   = ea->current_scope;
    info->flags         = ESCAPE_NONE;
    info->is_parameter  = is_parameter;
    info->analyzed      = false;
    info->is_string_view = false;
    info->parent_name    = NULL;
    return info;
}

EscapeInfo* escape_register_string_view(EscapeAnalyzer* ea,
                                         const char* name,
                                         const char* parent_name) {
    EscapeInfo* info = escape_register_var(ea, name, false);
    if (!info) return NULL;
    info->is_string_view = true;
    info->parent_name    = parent_name;
    return info;
}

EscapeInfo* escape_lookup(EscapeAnalyzer* ea, const char* var_name) {
    if (!var_name) return NULL;
    /* Walk backwards so inner-scope shadows win */
    for (int i = ea->count - 1; i >= 0; i--) {
        if (ea->entries[i].var_name &&
            strcmp(ea->entries[i].var_name, var_name) == 0) {
            return &ea->entries[i];
        }
    }
    return NULL;
}

/* ─── Mark helpers ─── */

static void mark_flag(EscapeAnalyzer* ea, const char* var_name,
                       EscapeFlags flag) {
    EscapeInfo* info = escape_lookup(ea, var_name);
    if (info) {
        info->flags |= flag;
    }
}

void escape_mark_return(EscapeAnalyzer* ea, const char* var_name) {
    mark_flag(ea, var_name, ESCAPE_RETURN);
}

void escape_mark_closure_capture(EscapeAnalyzer* ea, const char* var_name) {
    mark_flag(ea, var_name, ESCAPE_CLOSURE);
}

void escape_mark_global_store(EscapeAnalyzer* ea, const char* var_name) {
    mark_flag(ea, var_name, ESCAPE_GLOBAL);
}

void escape_mark_field_store(EscapeAnalyzer* ea, const char* var_name) {
    mark_flag(ea, var_name, ESCAPE_FIELD);
}

void escape_mark_call_arg(EscapeAnalyzer* ea, const char* var_name,
                           bool is_borrow) {
    /* Borrows do not cause escape — the callee never takes ownership */
    if (!is_borrow) {
        mark_flag(ea, var_name, ESCAPE_CALL_ARG);
    }
}

void escape_mark_collection_insert(EscapeAnalyzer* ea, const char* var_name) {
    mark_flag(ea, var_name, ESCAPE_COLLECTION);
}

/* ─── Queries ─── */

bool escape_can_stack_alloc(EscapeAnalyzer* ea, const char* var_name) {
    EscapeInfo* info = escape_lookup(ea, var_name);
    if (!info) return false;       /* Unknown — be conservative            */
    return info->flags == ESCAPE_NONE;
}

EscapeFlags escape_get_flags(EscapeAnalyzer* ea, const char* var_name) {
    EscapeInfo* info = escape_lookup(ea, var_name);
    return info ? info->flags : ESCAPE_UNKNOWN;
}

/* ─── AST Walkers ─── */

void escape_analyze_expr(EscapeAnalyzer* ea, Expr* expr,
                          const char* enclosing_func,
                          EscapeFlags context_flags) {
    if (!expr) return;

    switch (expr->type) {
    case EXPR_VARIABLE: {
        const char* name = expr->as.variable.name;
        if (!name) break;

        /* If the context says "this value is being returned / stored", propagate */
        if (context_flags != ESCAPE_NONE) {
            mark_flag(ea, name, context_flags);
        }

        /* If we are inside a closure body, any reference to an outer-scope
           variable means it is captured → closure escape */
        if (ea->in_closure) {
            EscapeInfo* info = escape_lookup(ea, name);
            if (info && info->scope_level < ea->current_scope) {
                info->flags |= ESCAPE_CLOSURE;
            }
        }
        break;
    }

    case EXPR_CALL: {
        /* Each argument might escape through the callee */
        for (int i = 0; i < expr->as.call.arg_count; i++) {
            Expr* arg = expr->as.call.arguments[i];
            /* Conservative: assume non-borrow arguments escape into callee.
               A future interprocedural pass could refine this. */
            escape_analyze_expr(ea, arg, enclosing_func, ESCAPE_CALL_ARG);
        }
        break;
    }

    case EXPR_NEW: {
        /* `new Foo(args)` — constructor may capture arguments in heap fields,
           so treat every argument as a potential non-borrow call argument. */
        for (int i = 0; i < expr->as.new_expr.arg_count; i++) {
            escape_analyze_expr(ea, expr->as.new_expr.arguments[i],
                                 enclosing_func, ESCAPE_CALL_ARG);
        }
        break;
    }

    case EXPR_MEMBER_ACCESS: {
        /* obj.field read — object itself does not escape */
        escape_analyze_expr(ea, expr->as.member.object,
                             enclosing_func, ESCAPE_NONE);
        /* obj.method(args) — method may capture arguments */
        if (expr->as.member.is_method_call) {
            for (int i = 0; i < expr->as.member.arg_count; i++) {
                escape_analyze_expr(ea, expr->as.member.arguments[i],
                                     enclosing_func, ESCAPE_CALL_ARG);
            }
        }
        break;
    }

    case EXPR_LAMBDA: {
        /* Entering closure body — everything referenced from outer scope escapes */
        bool prev_in_closure = ea->in_closure;
        ea->in_closure = true;
        escape_enter_scope(ea);

        /* Register closure parameters */
        for (int i = 0; i < expr->as.lambda.param_count; i++) {
            escape_register_var(ea, expr->as.lambda.parameters[i].name, true);
        }

        /* Analyse closure body */
        if (expr->as.lambda.block_body) {
            escape_analyze_stmt(ea, expr->as.lambda.block_body, enclosing_func);
        }
        if (expr->as.lambda.expr_body) {
            escape_analyze_expr(ea, expr->as.lambda.expr_body,
                                 enclosing_func, ESCAPE_NONE);
        }

        escape_exit_scope(ea);
        ea->in_closure = prev_in_closure;
        break;
    }

    case EXPR_BINARY: {
        escape_analyze_expr(ea, expr->as.binary.left,
                             enclosing_func, ESCAPE_NONE);
        escape_analyze_expr(ea, expr->as.binary.right,
                             enclosing_func, ESCAPE_NONE);
        break;
    }

    case EXPR_UNARY: {
        escape_analyze_expr(ea, expr->as.unary.operand,
                             enclosing_func, ESCAPE_NONE);
        break;
    }

    case EXPR_INDEX: {
        escape_analyze_expr(ea, expr->as.index.array,
                             enclosing_func, ESCAPE_NONE);
        escape_analyze_expr(ea, expr->as.index.index,
                             enclosing_func, ESCAPE_NONE);
        break;
    }

    default:
        /* Literals and other leaf expressions — no escape */
        break;
    }
}

void escape_analyze_stmt(EscapeAnalyzer* ea, Stmt* stmt,
                          const char* enclosing_func) {
    if (!stmt) return;

    switch (stmt->type) {
    case STMT_CONST_DECL:  /* fall through — same layout as DECLARATION */
    case STMT_DECLARATION: {
        const char* name = stmt->as.declaration.name;
        escape_register_var(ea, name, false);

        if (stmt->as.declaration.initializer) {
            escape_analyze_expr(ea, stmt->as.declaration.initializer,
                                 enclosing_func, ESCAPE_NONE);
        }
        break;
    }

    case STMT_ASSIGNMENT: {
        /* x = value — analyse the RHS to determine what escapes into x */
        Expr* lhs = stmt->as.assignment.target;
        Expr* rhs = stmt->as.assignment.value;

        /* If LHS is a field write, the RHS escapes via ESCAPE_FIELD */
        if (lhs && lhs->type == EXPR_MEMBER_ACCESS) {
            escape_analyze_expr(ea, rhs, enclosing_func, ESCAPE_FIELD);
        } else {
            escape_analyze_expr(ea, rhs, enclosing_func, ESCAPE_NONE);
        }
        escape_analyze_expr(ea, lhs, enclosing_func, ESCAPE_NONE);
        break;
    }

    case STMT_RETURN: {
        if (stmt->as.return_stmt.value) {
            /* The returned value escapes the function */
            escape_analyze_expr(ea, stmt->as.return_stmt.value,
                                 enclosing_func, ESCAPE_RETURN);
        }
        break;
    }

    case STMT_EXPR: {
        escape_analyze_expr(ea, stmt->as.expr_stmt.expression,
                             enclosing_func, ESCAPE_NONE);
        break;
    }

    case STMT_BLOCK: {
        escape_enter_scope(ea);
        for (int i = 0; i < stmt->as.block.stmt_count; i++) {
            escape_analyze_stmt(ea, stmt->as.block.statements[i],
                                 enclosing_func);
        }
        escape_exit_scope(ea);
        break;
    }

    case STMT_IF: {
        escape_analyze_expr(ea, stmt->as.if_stmt.condition,
                             enclosing_func, ESCAPE_NONE);
        escape_analyze_stmt(ea, stmt->as.if_stmt.then_branch,
                             enclosing_func);
        if (stmt->as.if_stmt.else_branch) {
            escape_analyze_stmt(ea, stmt->as.if_stmt.else_branch,
                                 enclosing_func);
        }
        break;
    }

    case STMT_WHILE: {
        escape_analyze_expr(ea, stmt->as.while_stmt.condition,
                             enclosing_func, ESCAPE_NONE);
        escape_analyze_stmt(ea, stmt->as.while_stmt.body,
                             enclosing_func);
        break;
    }

    case STMT_FOR: {
        escape_enter_scope(ea);
        escape_register_var(ea, stmt->as.for_stmt.variable, false);
        if (stmt->as.for_stmt.initializer) {
            escape_analyze_expr(ea, stmt->as.for_stmt.initializer,
                                 enclosing_func, ESCAPE_NONE);
        }
        if (stmt->as.for_stmt.condition) {
            escape_analyze_expr(ea, stmt->as.for_stmt.condition,
                                 enclosing_func, ESCAPE_NONE);
        }
        if (stmt->as.for_stmt.increment) {
            escape_analyze_stmt(ea, stmt->as.for_stmt.increment,
                                 enclosing_func);
        }
        escape_analyze_stmt(ea, stmt->as.for_stmt.body,
                             enclosing_func);
        escape_exit_scope(ea);
        break;
    }

    case STMT_FUNCTION: {
        /* Nested function / method — recursively analyse */
        const char* fname = stmt->as.function.name;
        escape_enter_scope(ea);

        for (int i = 0; i < stmt->as.function.param_count; i++) {
            escape_register_var(ea, stmt->as.function.parameters[i].name, true);
        }

        if (stmt->as.function.body) {
            escape_analyze_stmt(ea, stmt->as.function.body, fname);
        }

        escape_exit_scope(ea);
        break;
    }

    case STMT_CLASS: {
        /* Walk method bodies inside the class */
        for (int i = 0; i < stmt->as.class_stmt.method_count; i++) {
            MethodDecl* method = &stmt->as.class_stmt.methods[i];
            if (method->body) {
                escape_enter_scope(ea);
                for (int j = 0; j < method->param_count; j++) {
                    escape_register_var(ea, method->parameters[j].name, true);
                }
                escape_analyze_stmt(ea, method->body, method->name);
                escape_exit_scope(ea);
            }
        }
        break;
    }

    case STMT_IMPL: {
        /* Walk method bodies inside the impl block */
        for (int i = 0; i < stmt->as.impl_stmt.method_count; i++) {
            Stmt* method = stmt->as.impl_stmt.methods[i];
            if (method && method->type == STMT_FUNCTION && method->as.function.body) {
                escape_enter_scope(ea);
                for (int j = 0; j < method->as.function.param_count; j++) {
                    escape_register_var(ea, method->as.function.parameters[j].name, true);
                }
                escape_analyze_stmt(ea, method->as.function.body,
                                     method->as.function.name);
                escape_exit_scope(ea);
            }
        }
        break;
    }

    default:
        break;
    }
}

void escape_analyze_function(EscapeAnalyzer* ea, Stmt* func_stmt) {
    if (!func_stmt || func_stmt->type != STMT_FUNCTION) return;

    escape_analyzer_reset(ea);
    const char* fname = func_stmt->as.function.name;

    /* Register parameters */
    for (int i = 0; i < func_stmt->as.function.param_count; i++) {
        escape_register_var(ea, func_stmt->as.function.parameters[i].name, true);
    }

    /* Walk body */
    if (func_stmt->as.function.body) {
        escape_analyze_stmt(ea, func_stmt->as.function.body, fname);
    }

    /* Mark all entries as analyzed */
    for (int i = 0; i < ea->count; i++) {
        ea->entries[i].analyzed = true;
    }

    /* NOTE: `escape_propagate_view_links` is NOT called here.  The escape
     * analyser does not know which entries correspond to `StringView`
     * bindings until the linear-view module promotes them from the
     * ownership-checker state (see `linear_view_promote_escape_entries`
     * and the driver sequence in sema/semantic.c).  Calling the propagator
     * before promotion would silently no-op. */
}

/* ─── Linear-view escape propagation ─────────────────────────────────────
 *
 * Run as a fixpoint after the AST walk completes.  Two propagation
 * directions:
 *
 *   1. parent → view : any escape path the parent has becomes legal for
 *                      the view (the view can be returned wherever the
 *                      parent is).  This is the "lifting" rule.
 *
 *   2. view → parent : if the view escapes via RETURN/CLOSURE/GLOBAL/FIELD
 *                      and the parent does NOT, that is a borrow that
 *                      outlives its referent — emit a diagnostic.
 *
 * The function returns the number of diagnostics emitted.
 * ──────────────────────────────────────────────────────────────────────── */
#define LINEAR_VIEW_OUTLIVING_FLAGS \
    (ESCAPE_RETURN | ESCAPE_CLOSURE | ESCAPE_GLOBAL | ESCAPE_FIELD)

int escape_propagate_view_links(EscapeAnalyzer* ea, int line_for_diag) {
    if (!ea) return 0;
    int diagnostics = 0;

    /* Iterate to fixpoint — at most ea->count rounds since each round
     * monotonically grows at least one EscapeInfo's flag set.            */
    bool changed = true;
    int  rounds  = 0;
    while (changed && rounds++ < ea->count + 1) {
        changed = false;
        for (int i = 0; i < ea->count; i++) {
            EscapeInfo* v = &ea->entries[i];
            if (!v->is_string_view || !v->parent_name) continue;

            EscapeInfo* p = escape_lookup(ea, v->parent_name);
            if (!p) continue;

            EscapeFlags new_v = (EscapeFlags)(v->flags | p->flags);
            if (new_v != v->flags) {
                v->flags = new_v;
                changed  = true;
            }
        }
    }

    /* Now check: any view escape path NOT covered by parent is illegal. */
    for (int i = 0; i < ea->count; i++) {
        EscapeInfo* v = &ea->entries[i];
        if (!v->is_string_view) continue;

        if (!v->parent_name) {
            /* Already diagnosed by the linear-view registrar; skip here  */
            continue;
        }

        EscapeInfo* p = escape_lookup(ea, v->parent_name);
        if (!p) continue;

        EscapeFlags view_only = (EscapeFlags)(v->flags &
                                              LINEAR_VIEW_OUTLIVING_FLAGS &
                                              ~p->flags);
        if (view_only != 0) {
            char msg[320];
            const char* via =
                (view_only & ESCAPE_RETURN)  ? "return"             :
                (view_only & ESCAPE_CLOSURE) ? "closure capture"    :
                (view_only & ESCAPE_GLOBAL)  ? "global store"       :
                (view_only & ESCAPE_FIELD)   ? "heap field store"   :
                                                "unknown escape";
            snprintf(msg, sizeof(msg),
                     "Linear StringView '%s' escapes via %s, but its parent "
                     "String '%s' does not -- the borrowed pointer would "
                     "outlive its storage",
                     v->var_name, via, v->parent_name);
            report_semantic_error(line_for_diag, 0, msg);
            diagnostics++;
        }
    }

    return diagnostics;
}

/* ─── Debug ─── */

static const char* flags_to_str(EscapeFlags f, char* buf, size_t size) {
    buf[0] = '\0';
    if (f == ESCAPE_NONE) return "NONE (stack-eligible)";
    if (f & ESCAPE_RETURN)      strcat(buf, "RETURN ");
    if (f & ESCAPE_CLOSURE)     strcat(buf, "CLOSURE ");
    if (f & ESCAPE_GLOBAL)      strcat(buf, "GLOBAL ");
    if (f & ESCAPE_FIELD)       strcat(buf, "FIELD ");
    if (f & ESCAPE_CALL_ARG)    strcat(buf, "CALL_ARG ");
    if (f & ESCAPE_COLLECTION)  strcat(buf, "COLLECTION ");
    if (f & ESCAPE_UNKNOWN)     strcat(buf, "UNKNOWN ");
    return buf;
}

void escape_print_report(EscapeAnalyzer* ea) {
    printf("=== Escape Analysis Report ===\n");
    printf("%-20s %-6s %-8s %s\n", "Variable", "Scope", "Param?", "Escape Flags");
    printf("--------------------------------------------------------------\n");

    int stack_eligible = 0;
    for (int i = 0; i < ea->count; i++) {
        EscapeInfo* info = &ea->entries[i];
        char flag_buf[128];
        printf("%-20s %-6d %-8s %s\n",
               info->var_name ? info->var_name : "<anon>",
               info->scope_level,
               info->is_parameter ? "yes" : "no",
               flags_to_str(info->flags, flag_buf, sizeof(flag_buf)));
        if (info->flags == ESCAPE_NONE) stack_eligible++;
    }

    printf("--------------------------------------------------------------\n");
    printf("Total variables: %d  |  Stack-eligible: %d  |  Must-heap: %d\n",
           ea->count, stack_eligible, ea->count - stack_eligible);
    printf("==============================================================\n");
}
