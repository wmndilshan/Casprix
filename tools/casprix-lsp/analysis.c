/*
 * analysis.c — lightweight lex → parse → sema coordinator for the LSP.
 *
 * Unlike src/driver/pipeline.c (fail-fast, prints to stdout/stderr), this runs
 * the passes directly with the shared DiagEngine `g_diag` in *buffered* mode:
 *   - no diagnostic is ever rendered to a stream (buffered mode accumulates
 *     them in g_diag.buffer_head),
 *   - parser panic-mode recovery still produces a partial AST,
 *   - semantic analysis runs over whatever the parser produced.
 * Afterwards the buffered diagnostics are translated into LspDiag[] and the
 * buffer is drained.
 */
#define _POSIX_C_SOURCE 200809L
#include "lsp.h"

#include <stdlib.h>
#include <string.h>

#include "support/diagnostic.h"
#include "support/error.h"
#include "compiler/frontend/lexer.h"
#include "compiler/frontend/parser.h"

/* g_diag is the process-global DiagEngine (declared in diagnostic.h). It is
 * init'd once from main.c (lsp_main). */

static int diag_sev_to_lsp(DiagSeverity s) {
    switch (s) {
        case DIAG_ERROR:   return 1;
        case DIAG_WARNING: return 2;
        case DIAG_INFO:    return 3;
        case DIAG_NOTE:    return 3;
        case DIAG_HELP:    return 4;
        default:           return 3;
    }
}

void analysis_free(Document *doc) {
    if (!doc) return;
    if (doc->stmts) {
        for (int i = 0; i < doc->stmt_count; ++i)
            if (doc->stmts[i]) free_stmt(doc->stmts[i]);
        free(doc->stmts);
        doc->stmts = NULL;
    }
    doc->stmt_count = 0;
    if (doc->analyzer_inited) {
        free_semantic_analyzer(&doc->analyzer);
        doc->analyzer_inited = false;
    }
    for (int i = 0; i < doc->diag_count; ++i)
        free(doc->diags[i].message);
    free(doc->diags);
    doc->diags = NULL;
    doc->diag_count = 0;
    doc->analyzed = false;
}

void analysis_run(Document *doc) {
    if (!doc) return;
    analysis_free(doc);

    /* Reset shared diagnostic state for a fresh analysis. Buffered mode means
     * nothing is written to any stream. */
    /* Drain any leftover buffer from a prior run. */
    Diagnostic *leftover = g_diag.buffer_head;
    while (leftover) { Diagnostic *n = leftover->next; free(leftover); leftover = n; }
    g_diag.buffer_head = g_diag.buffer_tail = NULL;
    memset(&g_diag.stats, 0, sizeof(g_diag.stats));
    g_diag.has_errors = false;
    reset_error_count();

    /* ── Lex + parse ──────────────────────────────────────────────────────── */
    Lexer lexer;
    init_lexer(&lexer, doc->text);
    Parser parser;
    init_parser(&parser, &lexer);

    int n = 0;
    Stmt **stmts = parse(&parser, &n);   /* panic-mode recovery inside */
    doc->stmts = stmts;
    doc->stmt_count = n;

    /* ── Semantic analysis (best effort over the partial AST) ─────────────── */
    if (stmts) {
        init_semantic_analyzer(&doc->analyzer);
        doc->analyzer_inited = true;
        /* return value ignored — we want diagnostics, not a go/no-go */
        (void)analyze_program(&doc->analyzer, stmts, n);
    }

    /* ── Translate buffered diagnostics → LspDiag[] ──────────────────────── */
    int count = 0;
    for (Diagnostic *d = g_diag.buffer_head; d; d = d->next) count++;

    doc->diags = count ? (LspDiag *)calloc((size_t)count, sizeof(LspDiag)) : NULL;
    doc->diag_count = 0;

    for (Diagnostic *d = g_diag.buffer_head; d; d = d->next) {
        LspDiag *out = &doc->diags[doc->diag_count++];
        out->severity = diag_sev_to_lsp(d->severity);
        out->message  = strdup(d->message ? d->message : "");

        /* Primary label span, or the first label. All compiler spans from the
         * legacy report_* path are single-point (line==line, col==col), 1-based. */
        SourceSpan sp;
        memset(&sp, 0, sizeof(sp));
        if (d->label_count > 0) {
            sp = d->labels[0].span;
            for (int i = 0; i < d->label_count; ++i)
                if (d->labels[i].is_primary) { sp = d->labels[i].span; break; }
        }
        int ls = sp.line_start > 0 ? (int)sp.line_start : 1;
        int cs = sp.col_start  > 0 ? (int)sp.col_start  : 1;
        int le = sp.line_end   > 0 ? (int)sp.line_end   : ls;
        int ce = sp.col_end    > 0 ? (int)sp.col_end    : cs;
        /* to 0-based LSP, and make a >=1-char range so editors can render it */
        out->line_start = ls - 1;
        out->col_start  = cs - 1;
        out->line_end   = le - 1;
        out->col_end    = (le == ls && ce <= cs) ? cs : ce - 1;
        if (out->col_start < 0) out->col_start = 0;
        if (out->line_start < 0) out->line_start = 0;
        if (out->col_end <= out->col_start && out->line_end == out->line_start)
            out->col_end = out->col_start + 1;
    }

    /* Drain the buffer now that we've copied everything out. */
    Diagnostic *d = g_diag.buffer_head;
    while (d) { Diagnostic *nx = d->next; free(d); d = nx; }
    g_diag.buffer_head = g_diag.buffer_tail = NULL;

    doc->analyzed = true;
}
