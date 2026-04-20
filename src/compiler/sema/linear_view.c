/**
 * Casprix Compiler — Linear Type System for `StringView` (helpers)
 *
 * Pure helpers shared by ownership_check.c, escape_analysis.c, and
 * drop_planner.c.  See linear_view.h for the design contract.
 */

#define _POSIX_C_SOURCE 200809L
#include "compiler/sema/linear_view.h"
#include "compiler/sema/symtable.h"
#include "compiler/sema/ownership_check.h"

#include <string.h>

/* ─────────────────────────────────────────────────────────────────────────
 * Type-name probes
 * ───────────────────────────────────────────────────────────────────────── */

static bool name_equals(const char* candidate, const char* expected) {
    return candidate && expected && strcmp(candidate, expected) == 0;
}

bool linear_view_decl_is_view(const DeclarationStmt* decl) {
    if (!decl) return false;

    /* Spelled as `let v: StringView = ...` — class_name carries the spelling. */
    if (name_equals(decl->class_name, LINEAR_VIEW_TYPE_NAME)) {
        return true;
    }

    /* Spelled via a parameterised TypeInfo — type_info->type_name carries it. */
    if (decl->type_info &&
        name_equals(decl->type_info->type_name, LINEAR_VIEW_TYPE_NAME)) {
        return true;
    }

    return false;
}

bool linear_view_expr_is_view(const Expr* expr) {
    if (!expr) return false;
    if (name_equals(expr->class_name, LINEAR_VIEW_TYPE_NAME)) return true;
    if (expr->type_info &&
        name_equals(expr->type_info->type_name, LINEAR_VIEW_TYPE_NAME)) {
        return true;
    }
    return false;
}

bool linear_view_decl_is_string(const DeclarationStmt* decl) {
    if (!decl) return false;
    /* Strings are intrinsic: identified by the DataType enum, not a name. */
    return decl->type == TYPE_STRING;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Parent inference
 *
 * We perform a small depth-first walk over the initialiser expression,
 * stopping at the first EXPR_VARIABLE whose resolved symbol is TYPE_STRING.
 * That symbol becomes the parent.  This handles the common cases:
 *
 *     let v: StringView = make_view(s)              // parent = s
 *     let v: StringView = StringView { data: s.bytes, len: s.length }
 *     let v: StringView = s.slice(0, 4)             // parent = s
 *
 * It deliberately does NOT walk through fields of class/struct values that
 * might transitively contain a String: such cases require interprocedural
 * tracking and are conservatively rejected by the drop planner with an
 * "unknown parent" diagnostic.
 * ───────────────────────────────────────────────────────────────────────── */

static const char* walk_for_string_parent(SymbolTable* table, Expr* e) {
    if (!e || !table) return NULL;

    switch (e->type) {
    case EXPR_VARIABLE: {
        const char* name = e->as.variable.name;
        if (!name) return NULL;
        Symbol* sym = lookup_symbol(table, name);
        if (sym && sym->type == TYPE_STRING) {
            return name;        /* borrowed pointer; lives with AST */
        }
        return NULL;
    }

    case EXPR_CALL: {
        for (int i = 0; i < e->as.call.arg_count; i++) {
            const char* p = walk_for_string_parent(table, e->as.call.arguments[i]);
            if (p) return p;
        }
        const char* p = walk_for_string_parent(table, e->as.call.callee);
        if (p) return p;
        return NULL;
    }

    case EXPR_NEW: {
        for (int i = 0; i < e->as.new_expr.arg_count; i++) {
            const char* p = walk_for_string_parent(table, e->as.new_expr.arguments[i]);
            if (p) return p;
        }
        return NULL;
    }

    case EXPR_MEMBER_ACCESS: {
        /* `s.slice(...)` or `s.bytes` — `s` is the parent. */
        const char* p = walk_for_string_parent(table, e->as.member.object);
        if (p) return p;
        for (int i = 0; i < e->as.member.arg_count; i++) {
            p = walk_for_string_parent(table, e->as.member.arguments[i]);
            if (p) return p;
        }
        return NULL;
    }

    case EXPR_BINARY: {
        const char* p = walk_for_string_parent(table, e->as.binary.left);
        if (p) return p;
        return walk_for_string_parent(table, e->as.binary.right);
    }

    case EXPR_UNARY:
        return walk_for_string_parent(table, e->as.unary.operand);

    case EXPR_INDEX: {
        const char* p = walk_for_string_parent(table, e->as.index.array);
        if (p) return p;
        return walk_for_string_parent(table, e->as.index.index);
    }

    default:
        return NULL;
    }
}

const char* linear_view_infer_parent(SemanticAnalyzer* analyzer,
                                     Expr* init_expr) {
    if (!analyzer || !analyzer->symbols || !init_expr) return NULL;
    return walk_for_string_parent(analyzer->symbols, init_expr);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Linear-view log
 *
 * The ownership checker's per-symbol state disappears the moment the
 * declaring scope exits.  The escape analyser, however, runs after every
 * local scope inside the function has already been torn down.  To bridge
 * the lifetime gap without leaking scope-local memory into long-lived
 * structures, we copy the minimal `(view, parent, line)` triples into a
 * function-scoped log that is reset at each new function entry and
 * consulted during escape promotion.
 * ───────────────────────────────────────────────────────────────────────── */

#include <stdlib.h>

static char* sv_dup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (!p) return NULL;
    memcpy(p, s, n);
    return p;
}

void linear_view_log_reset(LinearViewLog* log) {
    if (!log) return;
    for (int i = 0; i < log->count; i++) {
        free(log->entries[i].view_name);
        free(log->entries[i].parent_name);
    }
    free(log->entries);
    log->entries  = NULL;
    log->count    = 0;
    log->capacity = 0;
}

void linear_view_log_add(LinearViewLog* log,
                         const char* view_name,
                         const char* parent_name,
                         int line) {
    if (!log || !view_name) return;

    if (log->count >= log->capacity) {
        int new_cap = log->capacity ? log->capacity * 2 : 8;
        LinearViewLogEntry* ne = (LinearViewLogEntry*)realloc(
            log->entries, (size_t)new_cap * sizeof(LinearViewLogEntry));
        if (!ne) return;
        log->entries  = ne;
        log->capacity = new_cap;
    }

    LinearViewLogEntry* e = &log->entries[log->count++];
    e->view_name   = sv_dup(view_name);
    e->parent_name = sv_dup(parent_name);          /* NULL-safe */
    e->line        = line;
}

void linear_view_promote_from_log(EscapeAnalyzer* ea,
                                  const LinearViewLog* log) {
    if (!ea || !log) return;

    for (int li = 0; li < log->count; li++) {
        const LinearViewLogEntry* le = &log->entries[li];
        if (!le->view_name) continue;

        for (int i = 0; i < ea->count; i++) {
            EscapeInfo* info = &ea->entries[i];
            if (!info->var_name) continue;
            if (strcmp(info->var_name, le->view_name) != 0) continue;
            info->is_string_view = true;
            info->parent_name    = le->parent_name; /* log-owned copy */
            break;
        }
    }
}
