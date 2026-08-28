/*
 * lsp_server.c — LSP method handlers over the decoded JSON-RPC message.
 */
#define _POSIX_C_SOURCE 200809L
#include "lsp.h"

#include <stdlib.h>
#include <string.h>

bool g_lsp_initialized        = false;
bool g_lsp_shutdown_requested = false;

/* ── helpers ─────────────────────────────────────────────────────────────── */

/* Serialise a JSON value node back to compact text (malloc'd). Used to echo an
 * incoming `id` verbatim into our response. */
static char *json_raw(const cJSON *v) {
    if (!v) return strdup("null");
    char *s = cJSON_PrintUnformatted(v);
    return s ? s : strdup("null");
}

static const char *json_str(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (v && cJSON_IsString(v)) ? v->valuestring : NULL;
}
static int json_int(const cJSON *obj, const char *key, int dflt) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (v && cJSON_IsNumber(v)) ? v->valueint : dflt;
}

/* ── initialize / shutdown ──────────────────────────────────────────────── */

static void handle_initialize(const char *id_json, FILE *out) {
    /* Capabilities: Full text sync, push diagnostics, documentSymbol,
     * definition. */
    const char *result =
        "{"
          "\"capabilities\":{"
            "\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
            "\"documentSymbolProvider\":true,"
            "\"definitionProvider\":true,"
            "\"diagnosticProvider\":{\"interFileDependencies\":false,"
                                    "\"workspaceDiagnostics\":false}"
          "},"
          "\"serverInfo\":{\"name\":\"casprix-lsp\",\"version\":\"0.1.0\"}"
        "}";
    jsonrpc_send_response_raw(out, id_json, result);
    g_lsp_initialized = true;
}

/* ── diagnostics publishing ─────────────────────────────────────────────── */

static void publish_diagnostics(Document *doc, FILE *out) {
    StrBuf sb; sb_init(&sb);
    sb_puts(&sb, "{\"uri\":");
    sb_json_string(&sb, doc->uri);
    sb_printf(&sb, ",\"version\":%d,\"diagnostics\":[", doc->version);
    for (int i = 0; i < doc->diag_count; ++i) {
        LspDiag *d = &doc->diags[i];
        if (i) sb_putc(&sb, ',');
        sb_printf(&sb,
            "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                       "\"end\":{\"line\":%d,\"character\":%d}},"
             "\"severity\":%d,\"source\":\"casprix\",\"message\":",
            d->line_start, d->col_start, d->line_end, d->col_end, d->severity);
        sb_json_string(&sb, d->message);
        sb_putc(&sb, '}');
    }
    sb_puts(&sb, "]}");
    jsonrpc_send_notification(out, "textDocument/publishDiagnostics", sb.data);
    sb_free(&sb);
}

/* ── didOpen / didChange / didClose ────────────────────────────────────── */

static void handle_did_open(const cJSON *params, FILE *out) {
    const cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    if (!td) return;
    const char *uri  = json_str(td, "uri");
    const char *text = json_str(td, "text");
    int version      = json_int(td, "version", 0);
    if (!uri) return;

    Document *doc = doc_put(uri, text ? text : "", version);
    analysis_run(doc);
    publish_diagnostics(doc, out);
}

static void handle_did_change(const cJSON *params, FILE *out) {
    const cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    if (!td) return;
    const char *uri = json_str(td, "uri");
    int version     = json_int(td, "version", 0);
    if (!uri) return;

    /* Full sync: take the last content change's full text. */
    const cJSON *changes = cJSON_GetObjectItemCaseSensitive(params, "contentChanges");
    const char *text = NULL;
    if (cJSON_IsArray(changes)) {
        int n = cJSON_GetArraySize(changes);
        if (n > 0) {
            const cJSON *last = cJSON_GetArrayItem(changes, n - 1);
            text = json_str(last, "text");
        }
    }
    if (!text) return;

    Document *doc = doc_put(uri, text, version);
    analysis_run(doc);
    publish_diagnostics(doc, out);
}

static void handle_did_close(const cJSON *params, FILE *out) {
    const cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    const char *uri = td ? json_str(td, "uri") : NULL;
    if (!uri) return;
    /* Clear diagnostics for the closed file, then drop it. */
    StrBuf sb; sb_init(&sb);
    sb_puts(&sb, "{\"uri\":");
    sb_json_string(&sb, uri);
    sb_puts(&sb, ",\"diagnostics\":[]}");
    jsonrpc_send_notification(out, "textDocument/publishDiagnostics", sb.data);
    sb_free(&sb);
    doc_remove(uri);
}

/* ── documentSymbol ────────────────────────────────────────────────────── */

/* LSP SymbolKind: File=1 Module=2 Namespace=3 Package=4 Class=5 Method=6
 * Property=7 Field=8 Constructor=9 Enum=10 Interface=11 Function=12 ...
 * Struct=23. */
static int stmt_symbol_kind(const Stmt *s) {
    switch (s->type) {
        case STMT_CLASS:    return 5;   /* Class     */
        case STMT_STRUCT:   return 23;  /* Struct    */
        case STMT_ENUM:     return 10;  /* Enum      */
        case STMT_UNION:    return 23;  /* Struct-ish */
        case STMT_TRAIT:    return 11;  /* Interface */
        case STMT_FUNCTION: return 12;  /* Function  */
        default:            return 0;
    }
}
static const char *stmt_symbol_name(const Stmt *s) {
    switch (s->type) {
        case STMT_CLASS:    return s->as.class_stmt.name;
        case STMT_STRUCT:   return s->as.struct_stmt.name;
        case STMT_ENUM:     return s->as.enum_stmt.name;
        case STMT_UNION:    return s->as.union_stmt.name;
        case STMT_TRAIT:    return s->as.trait_stmt.name;
        case STMT_FUNCTION: return s->as.function.name;
        default:            return NULL;
    }
}

static void emit_symbol(StrBuf *sb, const char *name, int kind,
                        int line1, int col1, bool *first) {
    if (!*first) sb_putc(sb, ',');
    *first = false;
    int l = line1 > 0 ? line1 - 1 : 0;
    int c = col1  > 0 ? col1  - 1 : 0;
    int name_len = (int)strlen(name);
    sb_puts(sb, "{\"name\":");
    sb_json_string(sb, name);
    sb_printf(sb, ",\"kind\":%d,", kind);
    sb_printf(sb,
        "\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                  "\"end\":{\"line\":%d,\"character\":%d}},",
        l, c, l, c + name_len);
    sb_printf(sb,
        "\"selectionRange\":{\"start\":{\"line\":%d,\"character\":%d},"
                           "\"end\":{\"line\":%d,\"character\":%d}}}",
        l, c, l, c + name_len);
}

static void handle_document_symbol(const char *id_json, const cJSON *params, FILE *out) {
    const cJSON *td = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    const char *uri = td ? json_str(td, "uri") : NULL;
    Document *doc = uri ? doc_get(uri) : NULL;

    StrBuf sb; sb_init(&sb);
    sb_putc(&sb, '[');
    bool first = true;
    if (doc && doc->analyzed && doc->stmts) {
        for (int i = 0; i < doc->stmt_count; ++i) {
            const Stmt *s = doc->stmts[i];
            if (!s) continue;
            int kind = stmt_symbol_kind(s);
            const char *nm = stmt_symbol_name(s);
            if (kind && nm)
                emit_symbol(&sb, nm, kind, s->line, s->column, &first);
        }
    }
    sb_putc(&sb, ']');
    jsonrpc_send_response_raw(out, id_json, sb.data);
    sb_free(&sb);
}

/* ── definition ────────────────────────────────────────────────────────── */

static void handle_definition(const char *id_json, const cJSON *params, FILE *out) {
    const cJSON *td  = cJSON_GetObjectItemCaseSensitive(params, "textDocument");
    const cJSON *pos = cJSON_GetObjectItemCaseSensitive(params, "position");
    const char *uri  = td ? json_str(td, "uri") : NULL;
    Document *doc = uri ? doc_get(uri) : NULL;

    int line0 = pos ? json_int(pos, "line", 0) : 0;
    int col0  = pos ? json_int(pos, "character", 0) : 0;

    int def_line1 = 0, def_col1 = 0;
    bool resolved = false;

    if (doc && doc->analyzed) {
        CursorWord w;
        if (astq_word_at(doc->text, line0, col0, &w)) {
            /* 1. top-level declaration (class/struct/enum/union/trait/func) */
            if (astq_toplevel_decl(doc->stmts, doc->stmt_count, w.name,
                                   &def_line1, &def_col1)) {
                resolved = true;
            }
            /* 2. symbol table confirms it is a known top-level type/function
             *    but the AST lookup missed (e.g. name mangling) — fall through
             *    only if we still have nothing. */
            if (!resolved && doc->analyzer_inited && doc->analyzer.symbols) {
                Symbol *sym = lookup_symbol(doc->analyzer.symbols, w.name);
                ClassSymbol *cls = lookup_class(doc->analyzer.symbols, w.name);
                TraitSymbol *tr  = lookup_trait(doc->analyzer.symbols, w.name);
                if (sym || cls || tr) {
                    /* re-run the AST scan (covers the common case); if that
                     * still fails we simply return no location. */
                    resolved = astq_toplevel_decl(doc->stmts, doc->stmt_count,
                                                  w.name, &def_line1, &def_col1);
                }
            }
            /* 3. local fallback: locals are freed from the symbol table on
             *    scope exit, so search the enclosing function's AST. */
            if (!resolved) {
                resolved = astq_local_decl(doc->stmts, doc->stmt_count, w.name,
                                           w.line, w.col, &def_line1, &def_col1);
            }
        }
    }

    if (!resolved) {
        jsonrpc_send_response_raw(out, id_json, "null");
        return;
    }

    int l = def_line1 > 0 ? def_line1 - 1 : 0;
    int c = def_col1  > 0 ? def_col1  - 1 : 0;
    StrBuf sb; sb_init(&sb);
    sb_puts(&sb, "{\"uri\":");
    sb_json_string(&sb, doc->uri);
    sb_printf(&sb,
        ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                    "\"end\":{\"line\":%d,\"character\":%d}}}",
        l, c, l, c);
    jsonrpc_send_response_raw(out, id_json, sb.data);
    sb_free(&sb);
}

/* ── dispatch ──────────────────────────────────────────────────────────── */

bool lsp_dispatch(cJSON *msg, FILE *out) {
    const cJSON *m  = cJSON_GetObjectItemCaseSensitive(msg, "method");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const cJSON *pr = cJSON_GetObjectItemCaseSensitive(msg, "params");
    const char *method = (m && cJSON_IsString(m)) ? m->valuestring : NULL;
    bool is_request = (id != NULL);

    if (!method) {
        /* A response to a server->client request; we send none in v1. Ignore. */
        return true;
    }

    char *id_json = is_request ? json_raw(id) : NULL;

    if (strcmp(method, "initialize") == 0) {
        handle_initialize(id_json ? id_json : "null", out);
    } else if (strcmp(method, "initialized") == 0) {
        /* notification, no-op */
    } else if (strcmp(method, "shutdown") == 0) {
        g_lsp_shutdown_requested = true;
        jsonrpc_send_response_raw(out, id_json ? id_json : "null", "null");
    } else if (strcmp(method, "exit") == 0) {
        free(id_json);
        return false; /* caller exits: code 0 if shutdown was requested else 1 */
    } else if (strcmp(method, "textDocument/didOpen") == 0) {
        handle_did_open(pr, out);
    } else if (strcmp(method, "textDocument/didChange") == 0) {
        handle_did_change(pr, out);
    } else if (strcmp(method, "textDocument/didClose") == 0) {
        handle_did_close(pr, out);
    } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
        handle_document_symbol(id_json ? id_json : "null", pr, out);
    } else if (strcmp(method, "textDocument/definition") == 0) {
        handle_definition(id_json ? id_json : "null", pr, out);
    } else if (is_request) {
        /* Unknown request → MethodNotFound so the client doesn't hang. */
        jsonrpc_send_error(out, id_json, -32601, "method not found");
    }
    /* Unknown notification → silently ignored. */

    free(id_json);
    return true;
}
