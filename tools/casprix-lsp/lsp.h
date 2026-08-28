/*
 * Casprix LSP server — shared declarations (v1).
 *
 * v1 scope: publishDiagnostics (push), textDocument/documentSymbol,
 * textDocument/definition. stdio JSON-RPC, Content-Length framing,
 * textDocumentSync = Full.
 */
#ifndef CASPRIX_LSP_H
#define CASPRIX_LSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "cJSON.h"

/* ── frontend / sema headers from casprix_compiler ─────────────────────────── */
#include "compiler/frontend/ast.h"
#include "compiler/sema/semantic.h"

/* ════════════════════════════════════════════════════════════════════════════
 *  jsonrpc.c — Content-Length framed transport over stdio
 * ════════════════════════════════════════════════════════════════════════════ */

/* Read one framed JSON-RPC message from `in`. Returns a malloc'd, NUL-terminated
 * body buffer (caller frees) or NULL on EOF / fatal framing error. */
char *jsonrpc_read_message(FILE *in);

/* Write one framed message. `body` is a complete JSON object text. */
void jsonrpc_write_message(FILE *out, const char *body);

/* Hand-format helpers — append JSON fragments to a growable buffer. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} StrBuf;

void sb_init(StrBuf *sb);
void sb_free(StrBuf *sb);
void sb_putc(StrBuf *sb, char c);
void sb_puts(StrBuf *sb, const char *s);
void sb_printf(StrBuf *sb, const char *fmt, ...);
/* Append `s` as a JSON string literal (adds quotes, escapes control chars). */
void sb_json_string(StrBuf *sb, const char *s);

/* Convenience: emit a framed JSON-RPC response `{"jsonrpc":"2.0","id":<id>,
 * "result":<result_json>}`. `id_json` is the raw id token text ("3", "\"x\"",
 * or "null"). */
void jsonrpc_send_response_raw(FILE *out, const char *id_json, const char *result_json);
void jsonrpc_send_error(FILE *out, const char *id_json, int code, const char *message);
void jsonrpc_send_notification(FILE *out, const char *method, const char *params_json);

/* ════════════════════════════════════════════════════════════════════════════
 *  document.c — in-memory document store keyed by URI
 * ════════════════════════════════════════════════════════════════════════════ */

/* One buffered diagnostic, translated from the compiler DiagEngine. */
typedef struct {
    int   severity;      /* LSP: 1=Error 2=Warning 3=Info 4=Hint            */
    int   line_start, col_start;   /* 0-based (LSP coords)                   */
    int   line_end,   col_end;     /* 0-based                               */
    char *message;       /* malloc'd                                        */
} LspDiag;

typedef struct Document {
    char *uri;           /* malloc'd, canonical key                         */
    char *text;          /* malloc'd full contents (Full sync)              */
    int   version;

    /* Cached analysis result. Owned by the Document; rebuilt on change.    */
    bool           analyzed;
    Stmt         **stmts;        /* parser output (partial-ok)              */
    int            stmt_count;
    SemanticAnalyzer analyzer;   /* holds the symbol table post-analyze     */
    bool           analyzer_inited;

    LspDiag       *diags;
    int            diag_count;

    struct Document *next;
} Document;

/* Store lifecycle */
void      docstore_init(void);
void      docstore_shutdown(void);

Document *doc_get(const char *uri);
/* Insert or replace; takes ownership of a *copy* of uri/text (it strdup's). */
Document *doc_put(const char *uri, const char *text, int version);
void      doc_remove(const char *uri);

/* ════════════════════════════════════════════════════════════════════════════
 *  analysis.c — lex → parse → sema coordinator (buffered diagnostics)
 * ════════════════════════════════════════════════════════════════════════════ */

/* Run the frontend + semantic analysis over `doc->text`, filling doc->stmts /
 * doc->analyzer / doc->diags. All compiler diagnostics are collected via the
 * DiagEngine's buffered mode; parser panic-mode recovery keeps a partial AST
 * usable for symbols / definition even when errors are present. Safe to call
 * repeatedly (frees the previous result first). */
void analysis_run(Document *doc);
void analysis_free(Document *doc);

/* ════════════════════════════════════════════════════════════════════════════
 *  ast_query.c — position → AST-node lookup (proximity based)
 * ════════════════════════════════════════════════════════════════════════════ */

/* Names have only a start line/col (no span), so we locate the identifier
 * token the cursor sits on by re-lexing the line and matching the word under
 * the position. */
typedef struct {
    char kind;      /* 'v' variable/ident, 't' type-ish (uppercase), 0 none  */
    char name[256];
    int  line;      /* 1-based, of the token                                 */
    int  col;       /* 1-based, of the token start                           */
} CursorWord;

/* line/col are 0-based LSP coords. */
bool astq_word_at(const char *text, int line0, int col0, CursorWord *out);

/* Find the top-level declaration whose name == `name` (class/struct/enum/
 * union/trait/function). Returns the Stmt pointer or NULL; fills line1/col1
 * (1-based) with the declaration's position. */
Stmt *astq_toplevel_decl(Stmt **stmts, int n, const char *name,
                         int *line1, int *col1);

/* Local fallback: within the function enclosing (line1,col1), search for a
 * DeclarationStmt / for-var / param named `name` that appears before the use.
 * Returns 1-based line/col via out params, or false. */
bool astq_local_decl(Stmt **stmts, int n, const char *name,
                     int use_line1, int use_col1,
                     int *def_line1, int *def_col1);

/* ════════════════════════════════════════════════════════════════════════════
 *  lsp_server.c — LSP method handlers
 * ════════════════════════════════════════════════════════════════════════════ */

/* Process one decoded JSON-RPC message object. Writes any responses /
 * notifications to `out`. Returns false when the server should exit. */
bool lsp_dispatch(cJSON *msg, FILE *out);

/* Whether `initialize` has completed (used to gate exit-code semantics). */
extern bool  g_lsp_initialized;
extern bool  g_lsp_shutdown_requested;

#endif /* CASPRIX_LSP_H */
