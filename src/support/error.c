/*
 * Legacy error API — now routes through the DiagEngine.
 *
 * Existing callers (lexer, parser, semantic analyser) continue to call
 * report_error() / report_error_at() / report_type_error() /
 * report_semantic_error() exactly as before, but the output now goes
 * through g_diag so it benefits from source snippets, colour, JSON
 * mode, error budget, etc.
 *
 * The `had_error` / `had_runtime_error` globals remain for backwards
 * compatibility in main.c control flow.
 */

#include "support/error.h"
#include "support/diagnostic.h"

bool had_error = false;
bool had_runtime_error = false;

int get_error_count(void) {
    return diag_engine_error_count(&g_diag);
}

void reset_error_count(void) {
    /* Reset the global stats but keep the engine alive. */
    g_diag.stats.counts[DIAG_ERROR] = 0;
    g_diag.stats.counts[DIAG_WARNING] = 0;
    g_diag.has_errors = false;
    had_error = false;
}

void report_error(int line, int column, const char* message) {
    SourceSpan span = span_from_pos(0, (uint32_t)line, (uint32_t)column);
    diag_error(&g_diag, STAGE_NONE, DIAG_CODE_NONE, span, "%s", message);
    had_error = true;
}

void report_error_at(int line, int column, const char* format, ...) {
    SourceSpan span = span_from_pos(0, (uint32_t)line, (uint32_t)column);

    /* Format the message through the arena. */
    va_list args;
    va_start(args, format);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    diag_error(&g_diag, STAGE_NONE, DIAG_CODE_NONE, span, "%s", buf);
    had_error = true;
}

void report_type_error(int line, int column, const char* message) {
    SourceSpan span = span_from_pos(0, (uint32_t)line, (uint32_t)column);
    diag_error(&g_diag, STAGE_SEMA, E_TYPE_MISMATCH, span,
               "type error: %s", message);
    had_error = true;
}

void report_semantic_error(int line, int column, const char* message) {
    SourceSpan span = span_from_pos(0, (uint32_t)line, (uint32_t)column);
    diag_error(&g_diag, STAGE_SEMA, DIAG_CODE_NONE, span,
               "semantic error: %s", message);
    had_error = true;
}