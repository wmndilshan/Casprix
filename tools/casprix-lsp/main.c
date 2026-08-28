/*
 * main.c — Casprix LSP server entry point.
 *
 *   casprix-lsp            reads framed JSON-RPC from stdin, writes to stdout.
 *
 * STDOUT is the JSON-RPC channel and must carry nothing else. All compiler
 * output paths are pinned to stderr (see below); DiagEngine runs in buffered
 * mode so it never renders to any stream during an LSP analysis run.
 */
#define _POSIX_C_SOURCE 200809L
#include "lsp.h"

#include <stdlib.h>
#include <string.h>

#include "support/diagnostic.h"
#include "support/log.h"
#include "support/debug.h"

int main(void) {
    /* ── Pin every compiler output stream away from stdout ─────────────────
     * Step-0 audit: the lex→parse→sema path writes nothing to stdout today.
     * These calls are defensive so a future addition can't corrupt the
     * JSON-RPC channel. */
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);

    cpx_log_init(CPX_LOG_OFF);           /* logging off; sink is stderr anyway */
    cpx_log_set_stream(stderr);

    g_debug_config.output = stderr;      /* debug dumps → stderr if ever hit  */

    diag_engine_init(&g_diag);
    diag_engine_set_output(&g_diag, stderr);
    diag_engine_set_colors(&g_diag, false);
    diag_engine_set_buffered(&g_diag, true);   /* buffer, never render        */
    g_diag.min_severity = DIAG_WARNING;        /* errors + warnings to the IDE */

    docstore_init();

    /* ── Transport loop ─────────────────────────────────────────────────── */
    for (;;) {
        char *body = jsonrpc_read_message(stdin);
        if (!body) break;                       /* EOF / framing error        */

        cJSON *msg = cJSON_Parse(body);
        free(body);
        if (!msg) continue;                     /* skip malformed JSON        */

        bool keep_going = lsp_dispatch(msg, stdout);
        cJSON_Delete(msg);
        if (!keep_going) {
            docstore_shutdown();
            diag_engine_destroy(&g_diag);
            /* Per LSP: exit 0 iff a shutdown request preceded exit.          */
            return g_lsp_shutdown_requested ? 0 : 1;
        }
    }

    /* stdin closed without exit — treat as clean shutdown. */
    docstore_shutdown();
    diag_engine_destroy(&g_diag);
    return g_lsp_shutdown_requested ? 0 : 1;
}
