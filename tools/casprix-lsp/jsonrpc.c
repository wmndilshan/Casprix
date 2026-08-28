/*
 * jsonrpc.c — Content-Length framed JSON-RPC 2.0 transport over stdio,
 * plus tiny hand-formatting helpers for outgoing messages.
 */
#define _POSIX_C_SOURCE 200809L
#include "lsp.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── StrBuf ──────────────────────────────────────────────────────────────── */

void sb_init(StrBuf *sb) { sb->data = NULL; sb->len = 0; sb->cap = 0; }
void sb_free(StrBuf *sb) { free(sb->data); sb->data = NULL; sb->len = sb->cap = 0; }

static void sb_reserve(StrBuf *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t nc = sb->cap ? sb->cap * 2 : 256;
    while (nc < sb->len + extra + 1) nc *= 2;
    sb->data = (char *)realloc(sb->data, nc);
    sb->cap = nc;
}

void sb_putc(StrBuf *sb, char c) {
    sb_reserve(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

void sb_puts(StrBuf *sb, const char *s) {
    size_t n = strlen(s);
    sb_reserve(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_printf(StrBuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    sb_reserve(sb, (size_t)n);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)n;
}

void sb_json_string(StrBuf *sb, const char *s) {
    sb_putc(sb, '"');
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
            switch (*p) {
                case '"':  sb_puts(sb, "\\\""); break;
                case '\\': sb_puts(sb, "\\\\"); break;
                case '\n': sb_puts(sb, "\\n");  break;
                case '\r': sb_puts(sb, "\\r");  break;
                case '\t': sb_puts(sb, "\\t");  break;
                case '\b': sb_puts(sb, "\\b");  break;
                case '\f': sb_puts(sb, "\\f");  break;
                default:
                    if (*p < 0x20) sb_printf(sb, "\\u%04x", (unsigned)*p);
                    else           sb_putc(sb, (char)*p);
            }
        }
    }
    sb_putc(sb, '"');
}

/* ── Transport ───────────────────────────────────────────────────────────── */

char *jsonrpc_read_message(FILE *in) {
    long content_length = -1;
    char line[8192];

    /* Read headers until blank line. */
    for (;;) {
        if (!fgets(line, sizeof(line), in)) return NULL; /* EOF */
        /* strip trailing CR/LF */
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0) break; /* end of headers */

        /* case-insensitive match on "Content-Length:" */
        const char *cl = "content-length:";
        size_t cln = strlen(cl);
        if (n > cln) {
            int match = 1;
            for (size_t i = 0; i < cln; ++i)
                if (tolower((unsigned char)line[i]) != cl[i]) { match = 0; break; }
            if (match) {
                const char *v = line + cln;
                while (*v == ' ' || *v == '\t') ++v;
                content_length = strtol(v, NULL, 10);
            }
        }
        /* Content-Type / other headers ignored. */
    }

    if (content_length < 0) return NULL;

    char *body = (char *)malloc((size_t)content_length + 1);
    if (!body) return NULL;
    size_t got = fread(body, 1, (size_t)content_length, in);
    body[got] = '\0';
    if (got != (size_t)content_length) { free(body); return NULL; }
    return body;
}

void jsonrpc_write_message(FILE *out, const char *body) {
    size_t n = strlen(body);
    fprintf(out, "Content-Length: %zu\r\n\r\n", n);
    fwrite(body, 1, n, out);
    fflush(out);
}

void jsonrpc_send_response_raw(FILE *out, const char *id_json, const char *result_json) {
    StrBuf sb; sb_init(&sb);
    sb_puts(&sb, "{\"jsonrpc\":\"2.0\",\"id\":");
    sb_puts(&sb, id_json ? id_json : "null");
    sb_puts(&sb, ",\"result\":");
    sb_puts(&sb, result_json ? result_json : "null");
    sb_putc(&sb, '}');
    jsonrpc_write_message(out, sb.data);
    sb_free(&sb);
}

void jsonrpc_send_error(FILE *out, const char *id_json, int code, const char *message) {
    StrBuf sb; sb_init(&sb);
    sb_puts(&sb, "{\"jsonrpc\":\"2.0\",\"id\":");
    sb_puts(&sb, id_json ? id_json : "null");
    sb_printf(&sb, ",\"error\":{\"code\":%d,\"message\":", code);
    sb_json_string(&sb, message ? message : "error");
    sb_puts(&sb, "}}");
    jsonrpc_write_message(out, sb.data);
    sb_free(&sb);
}

void jsonrpc_send_notification(FILE *out, const char *method, const char *params_json) {
    StrBuf sb; sb_init(&sb);
    sb_puts(&sb, "{\"jsonrpc\":\"2.0\",\"method\":");
    sb_json_string(&sb, method);
    sb_puts(&sb, ",\"params\":");
    sb_puts(&sb, params_json ? params_json : "null");
    sb_putc(&sb, '}');
    jsonrpc_write_message(out, sb.data);
    sb_free(&sb);
}
