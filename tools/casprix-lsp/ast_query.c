/*
 * ast_query.c — position → AST-node lookup.
 *
 * AST nodes carry only a start line/col (no end span), so:
 *  - "word under cursor" is found by scanning the raw text line for the
 *    identifier the (line,col) sits inside;
 *  - "declaration of NAME" is a straight walk of the AST comparing names and
 *    reading the node's line/col.
 */
#define _POSIX_C_SOURCE 200809L
#include "lsp.h"

#include <ctype.h>
#include <string.h>
#include <stdlib.h>

/* ── word under cursor ───────────────────────────────────────────────────── */

static bool is_ident_char(int c) { return isalnum(c) || c == '_'; }

bool astq_word_at(const char *text, int line0, int col0, CursorWord *out) {
    if (!text || !out) return false;
    memset(out, 0, sizeof(*out));

    /* advance to line `line0` (0-based) */
    const char *p = text;
    int line = 0;
    while (line < line0 && *p) {
        if (*p == '\n') line++;
        p++;
    }
    if (line != line0) return false;

    const char *line_start = p;
    const char *line_end = p;
    while (*line_end && *line_end != '\n') line_end++;

    int line_len = (int)(line_end - line_start);
    if (col0 < 0) col0 = 0;
    if (col0 > line_len) col0 = line_len;

    int c = col0;
    if (c >= line_len) c = line_len - 1;
    if (c < 0) return false;
    /* If not on an identifier char, try stepping right first (cursor sitting
     * in the whitespace just before a name), then left (just past a name). */
    if (!is_ident_char((unsigned char)line_start[c])) {
        if (c + 1 < line_len && is_ident_char((unsigned char)line_start[c + 1]))
            c++;
        else if (c > 0 && is_ident_char((unsigned char)line_start[c - 1]))
            c--;
    }
    if (c < 0 || c >= line_len || !is_ident_char((unsigned char)line_start[c]))
        return false;

    int s = c, e = c;
    while (s > 0 && is_ident_char((unsigned char)line_start[s - 1])) s--;
    while (e + 1 < line_len && is_ident_char((unsigned char)line_start[e + 1])) e++;

    int wlen = e - s + 1;
    if (wlen <= 0 || wlen >= (int)sizeof(out->name)) return false;
    memcpy(out->name, line_start + s, (size_t)wlen);
    out->name[wlen] = '\0';
    out->line = line0 + 1;      /* 1-based */
    out->col  = s + 1;          /* 1-based */
    out->kind = isupper((unsigned char)out->name[0]) ? 't' : 'v';
    return true;
}

/* ── top-level declaration lookup ────────────────────────────────────────── */

static const char *stmt_decl_name(const Stmt *s) {
    switch (s->type) {
        case STMT_FUNCTION: return s->as.function.name;
        case STMT_CLASS:    return s->as.class_stmt.name;
        case STMT_STRUCT:   return s->as.struct_stmt.name;
        case STMT_ENUM:     return s->as.enum_stmt.name;
        case STMT_UNION:    return s->as.union_stmt.name;
        case STMT_TRAIT:    return s->as.trait_stmt.name;
        default:            return NULL;
    }
}

Stmt *astq_toplevel_decl(Stmt **stmts, int n, const char *name,
                         int *line1, int *col1) {
    if (!stmts || !name) return NULL;
    for (int i = 0; i < n; ++i) {
        Stmt *s = stmts[i];
        if (!s) continue;
        const char *dn = stmt_decl_name(s);
        if (dn && strcmp(dn, name) == 0) {
            if (line1) *line1 = s->line;
            if (col1)  *col1  = s->column;
            return s;
        }
    }
    return NULL;
}

/* ── local declaration fallback ─────────────────────────────────────────── */

typedef struct {
    const char *name;
    int use_line, use_col;
    int best_line, best_col;
    bool found;
} LocalScan;

/* Record a candidate if it lexically precedes the use and beats the current
 * best (closest-preceding wins). */
static void local_candidate(LocalScan *sc, const char *nm, int line, int col) {
    if (!nm || strcmp(nm, sc->name) != 0) return;
    if (line > sc->use_line || (line == sc->use_line && col > sc->use_col)) return;
    if (!sc->found || line > sc->best_line ||
        (line == sc->best_line && col > sc->best_col)) {
        sc->best_line = line;
        sc->best_col  = col;
        sc->found = true;
    }
}

static void local_walk_stmt(const Stmt *s, LocalScan *sc);

static void local_walk_block(Stmt **stmts, int n, LocalScan *sc) {
    for (int i = 0; i < n; ++i) local_walk_stmt(stmts[i], sc);
}

static void local_walk_stmt(const Stmt *s, LocalScan *sc) {
    if (!s) return;
    switch (s->type) {
        case STMT_DECLARATION:
        case STMT_CONST_DECL:
            local_candidate(sc, s->as.declaration.name, s->line, s->column);
            break;
        case STMT_BLOCK:
            local_walk_block(s->as.block.statements, s->as.block.stmt_count, sc);
            break;
        case STMT_IF:
            local_walk_stmt(s->as.if_stmt.then_branch, sc);
            local_walk_stmt(s->as.if_stmt.else_branch, sc);
            break;
        case STMT_WHILE:
            local_walk_stmt(s->as.while_stmt.body, sc);
            break;
        case STMT_FOR:
            /* the loop variable is declared at the for-stmt position */
            local_candidate(sc, s->as.for_stmt.variable, s->line, s->column);
            local_walk_stmt(s->as.for_stmt.increment, sc);
            local_walk_stmt(s->as.for_stmt.body, sc);
            break;
        case STMT_FOR_IN:
            local_candidate(sc, s->as.for_in_stmt.var_name, s->line, s->column);
            local_walk_stmt(s->as.for_in_stmt.body, sc);
            break;
        default:
            break;
    }
}

/* A statement (line,col) is "inside" a function body block if it falls between
 * the function's line and the next top-level statement's line. Since we only
 * have start positions we approximate by function ordering. */
bool astq_local_decl(Stmt **stmts, int n, const char *name,
                     int use_line1, int use_col1,
                     int *def_line1, int *def_col1) {
    if (!stmts || !name) return false;

    /* Find the enclosing function: the last STMT_FUNCTION whose line <= use
     * line and whose successor top-level stmt (if any) starts after the use. */
    const Stmt *fn = NULL;
    for (int i = 0; i < n; ++i) {
        const Stmt *s = stmts[i];
        if (!s || s->type != STMT_FUNCTION) continue;
        if (s->line > use_line1) continue;
        int next_line = 1 << 30;
        for (int j = i + 1; j < n; ++j)
            if (stmts[j]) { next_line = stmts[j]->line; break; }
        if (use_line1 < next_line) fn = s;
    }
    if (!fn) return false;

    LocalScan sc = {0};
    sc.name = name;
    sc.use_line = use_line1;
    sc.use_col  = use_col1;

    /* function parameters — declared conceptually at the function position */
    for (int i = 0; i < fn->as.function.param_count; ++i)
        local_candidate(&sc, fn->as.function.parameters[i].name, fn->line, fn->column);

    local_walk_stmt(fn->as.function.body, &sc);

    if (!sc.found) return false;
    if (def_line1) *def_line1 = sc.best_line;
    if (def_col1)  *def_col1  = sc.best_col;
    return true;
}
