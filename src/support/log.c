/*
 * log.c -- Casprix Compiler Logging & Diagnostics Implementation
 *
 * Copyright (c) 2026 Casprix Project
 * SPDX-License-Identifier: MIT
 */

/* Enable POSIX-2008 APIs: clock_gettime, fileno, strcasecmp */
#ifndef _WIN32
#  define _POSIX_C_SOURCE 200809L
#  include <strings.h>
#endif

#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#ifdef _WIN32
#  include <windows.h>
#  define strcasecmp _stricmp
/* High-resolution timer on Windows */
static double s_qpc_freq = 0.0;
static void   init_timer(void) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    s_qpc_freq = (double)f.QuadPart / 1000.0; /* ticks -> ms */
}
static double now_ms(void) {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / s_qpc_freq;
}
#else
#  include <time.h>
#  include <unistd.h>
static void init_timer(void) {}
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif

/* ============================================================
 * ANSI color codes
 * ============================================================ */

#define A_RESET   "\x1b[0m"
#define A_BOLD    "\x1b[1m"
#define A_DIM     "\x1b[2m"
#define A_BLACK   "\x1b[30m"
#define A_RED     "\x1b[31m"
#define A_GREEN   "\x1b[32m"
#define A_YELLOW  "\x1b[33m"
#define A_BLUE    "\x1b[34m"
#define A_MAGENTA "\x1b[35m"
#define A_CYAN    "\x1b[36m"
#define A_WHITE   "\x1b[37m"
#define A_GRAY    "\x1b[90m"
#define A_BRED    "\x1b[1;31m"
#define A_BYELLOW "\x1b[1;33m"
#define A_BBLUE   "\x1b[1;34m"
#define A_BCYAN   "\x1b[1;36m"

/* Professional indicators */
#define INDICATOR_INFO   "i"
#define INDICATOR_OK     "ok"
#define INDICATOR_ERR    "!!"
#define INDICATOR_STEP   "->"


/* ============================================================
 * Static data tables
 * ============================================================ */

static const char *s_level_name[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"
};

/* Color applied to the level badge */
static const char *s_level_color[] = {
    A_GRAY,    /* TRACE */
    A_CYAN,    /* DEBUG */
    A_GREEN,   /* INFO  */
    A_YELLOW,  /* WARN  */
    A_BRED,    /* ERROR */
    A_MAGENTA, /* FATAL */
    ""         /* OFF   */
};

/* 4-char padded badge text */
static const char *s_level_badge[] = {
    "TRC ", "DBG ", "INF ", "WRN ", "ERR ", "FTL ", "OFF "
};

static const char *s_cat_name[] = {
    "GENERAL", "DRIVER", "LEXER",   "PARSER",  "SEMA",
    "MIR",     "OPT",    "CODEGEN", "REGALLOC","LINKER",
    "RUNTIME", "MEMORY", "IO",      "PKGMGR"
};

static const char *s_diag_name[] = {
    "error", "warning", "note", "hint"
};

static const char *s_diag_color[] = {
    A_BRED,    /* error   */
    A_BYELLOW, /* warning */
    A_BCYAN,   /* note    */
    A_BBLUE    /* hint    */
};

/* ============================================================
 * Global config instance
 * ============================================================ */

CpxLogConfig g_cpx_log = {
    .min_level      = CPX_LOG_INFO,
    .cat_level      = { 0 },           /* all zeros = CPX_LOG_TRACE */
    .enabled_cats   = 0xFFFFFFFFu,
    .stream         = NULL,            /* late-initialised to stderr */
    .format         = CPX_LOG_FMT_TEXT,
    .color          = true,
    .timestamp      = true,
    .show_location  = false,
    .show_category  = true,
    .werror         = false,
    .show_phase_times = true,
    .file           = { NULL },
    .file_path      = { {0} },
    .file_count     = 0
};

/* ============================================================
 * Diagnostic counters
 * ============================================================ */

static int s_error_count = 0;
static int s_warn_count  = 0;

/* ============================================================
 * Phase timing state
 * ============================================================ */

typedef struct {
    char   name[CPX_PHASE_NAME_MAX];
    double start_ms;
} PhaseEntry;

static PhaseEntry s_phase_stack[CPX_PHASE_MAX_DEPTH];
static int        s_phase_depth = 0;

/* Accumulated records (one per unique name, first-seen order) */
#define CPX_MAX_PHASES 64
typedef struct { char name[CPX_PHASE_NAME_MAX]; double total_ms; int calls; } PhaseRecord;
static PhaseRecord s_phase_records[CPX_MAX_PHASES];
static int         s_phase_record_count = 0;

/* ============================================================
 * Progress state
 * ============================================================ */

static bool s_progress_active = false;  /* did we emit a \r line? */

/* ============================================================
 * Platform helpers
 * ============================================================ */

#ifdef _WIN32
static HANDLE   s_con_handle  = NULL;
static DWORD    s_con_orig_mode = 0;
static bool     s_con_init    = false;

static void enable_vt_on_windows(void) {
    if (s_con_init) return;
    s_con_init = true;
    s_con_handle = GetStdHandle(STD_ERROR_HANDLE);
    if (s_con_handle != INVALID_HANDLE_VALUE) {
        GetConsoleMode(s_con_handle, &s_con_orig_mode);
        SetConsoleMode(s_con_handle,
            s_con_orig_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

static void restore_vt_on_windows(void) {
    if (s_con_init && s_con_handle != INVALID_HANDLE_VALUE)
        SetConsoleMode(s_con_handle, s_con_orig_mode);
}

/* true when the handle is a real console (not redirected to file/pipe) */
static bool stream_is_tty(FILE *f) {
    HANDLE h = (f == stderr) ? GetStdHandle(STD_ERROR_HANDLE)
                              : GetStdHandle(STD_OUTPUT_HANDLE);
    return GetFileType(h) == FILE_TYPE_CHAR;
}
#else
static void enable_vt_on_windows(void) {}
static void restore_vt_on_windows(void) {}
static bool stream_is_tty(FILE *f) { return isatty(fileno(f)) != 0; }
#endif

/* ============================================================
 * Internal helpers
 * ============================================================ */

/* Effective minimum level for a category */
static CpxLogLevel effective_level(CpxLogCategory cat) {
    if (cat < CPX_CAT_COUNT && g_cpx_log.cat_level[cat] != CPX_LOG_TRACE)
        return g_cpx_log.cat_level[cat];
    return g_cpx_log.min_level;
}

/* Strip leading path components, keep only basename */
static const char *basename_of(const char *path) {
    if (!path) return "";
    const char *p = strrchr(path, '/');
    const char *q = strrchr(path, '\\');
    const char *sep = p > q ? p : q;
    return sep ? sep + 1 : path;
}

/* ============================================================
 * TEXT formatter
 * ============================================================ */

static void write_text(FILE *out, bool color,
                       CpxLogLevel level, CpxLogCategory cat,
                       const char *src_file, int src_line, const char *src_func,
                       const char *msg)
{
    /* Timestamp HH:MM:SS.mmm */
    if (g_cpx_log.timestamp) {
        time_t     t  = time(NULL);
        struct tm *tm = localtime(&t);
        if (color)
            fprintf(out, "%s%02d:%02d:%02d%s ",
                    A_DIM, tm->tm_hour, tm->tm_min, tm->tm_sec, A_RESET);
        else
            fprintf(out, "%02d:%02d:%02d ",
                    tm->tm_hour, tm->tm_min, tm->tm_sec);
    }

    /* Level badge */
    if (color)
        fprintf(out, "%s%s%s", s_level_color[level], s_level_badge[level], A_RESET);
    else
        fprintf(out, "%s", s_level_badge[level]);

    /* Category badge */
    if (g_cpx_log.show_category && cat < CPX_CAT_COUNT) {
        if (color)
            fprintf(out, "%s%-8s%s %s ", A_DIM, s_cat_name[cat], A_RESET, INDICATOR_STEP);
        else
            fprintf(out, "%-8s > ", s_cat_name[cat]);
    }

    /* Message */
    if (color && level >= CPX_LOG_ERROR) fprintf(out, A_BOLD);
    fputs(msg, out);
    if (color) fprintf(out, A_RESET);
    fputc('\n', out);
    fflush(out);
}

/* ============================================================
 * JSON formatter  (one object per line)
 * ============================================================ */

/* Escape a string for JSON: only handles the printable-ASCII subset
   that appears in compiler messages. */
static void json_escape(FILE *out, const char *s) {
    if (!s) { fputs("null", out); return; }
    fputc('"', out);
    for (; *s; s++) {
        switch (*s) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n",  out); break;
        case '\r': fputs("\\r",  out); break;
        case '\t': fputs("\\t",  out); break;
        default:
            if ((unsigned char)*s < 0x20)
                fprintf(out, "\\u%04x", (unsigned char)*s);
            else
                fputc(*s, out);
        }
    }
    fputc('"', out);
}

static void write_json(FILE *out,
                       CpxLogLevel level, CpxLogCategory cat,
                       const char *src_file, int src_line,
                       const char *msg)
{
    time_t     t  = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", tm);

    fprintf(out,
        "{\"ts\":\"%s\",\"level\":\"%s\",\"cat\":\"%s\","
        "\"file\":",
        ts,
        s_level_name[level],
        (cat < CPX_CAT_COUNT) ? s_cat_name[cat] : "UNKNOWN");
    json_escape(out, basename_of(src_file));
    fprintf(out, ",\"line\":%d,\"msg\":", src_line);
    json_escape(out, msg);
    fputs("}\n", out);
    fflush(out);
}

/* ============================================================
 * Dispatch to all sinks
 * ============================================================ */

static void dispatch(CpxLogLevel level, CpxLogCategory cat,
                     const char *src_file, int src_line, const char *src_func,
                     const char *msg)
{
    /* If a progress line is showing, clear it first */
    if (s_progress_active) {
        cpx_log_progress_clear();
    }

    FILE *stream = g_cpx_log.stream ? g_cpx_log.stream : stderr;

    if (g_cpx_log.format == CPX_LOG_FMT_JSON) {
        write_json(stream, level, cat, src_file, src_line, msg);
    } else {
        bool color = g_cpx_log.color && stream_is_tty(stream);
        write_text(stream, color, level, cat, src_file, src_line, src_func, msg);
    }

    /* File sinks — always plain text, no color */
    for (int i = 0; i < g_cpx_log.file_count; i++) {
        if (g_cpx_log.file[i])
            write_text(g_cpx_log.file[i], false,
                       level, cat, src_file, src_line, src_func, msg);
    }
}

/* ============================================================
 * Public: init & teardown
 * ============================================================ */

void cpx_log_init(CpxLogLevel min_level) {
    bool color = stream_is_tty(stderr);
    cpx_log_init_full(min_level, stderr, color, CPX_LOG_FMT_TEXT);
}

void cpx_log_init_full(CpxLogLevel min_level, FILE *stream,
                       bool color, CpxLogFormat format) {
    g_cpx_log.min_level    = min_level;
    g_cpx_log.stream       = stream ? stream : stderr;
    g_cpx_log.color        = color;
    g_cpx_log.format       = format;
    g_cpx_log.timestamp    = true;
    g_cpx_log.show_location= false;
    g_cpx_log.show_category= true;
    g_cpx_log.werror       = false;
    g_cpx_log.show_phase_times = true;
    g_cpx_log.enabled_cats = 0xFFFFFFFFu;
    memset(g_cpx_log.cat_level, 0, sizeof g_cpx_log.cat_level);
    s_error_count = 0;
    s_warn_count  = 0;
    s_phase_depth = 0;
    s_phase_record_count = 0;

    if (color) enable_vt_on_windows();
    init_timer();
}

void cpx_log_shutdown(void) {
    cpx_log_close_files();
    restore_vt_on_windows();
}

/* ============================================================
 * Public: runtime config setters
 * ============================================================ */

void cpx_log_set_level           (CpxLogLevel l)    { g_cpx_log.min_level       = l; }
void cpx_log_set_stream          (FILE *s)           { g_cpx_log.stream          = s ? s : stderr; }
void cpx_log_set_color           (bool on)           { g_cpx_log.color           = on; if (on) enable_vt_on_windows(); }
void cpx_log_set_format          (CpxLogFormat f)    { g_cpx_log.format          = f; }
void cpx_log_set_timestamp       (bool on)           { g_cpx_log.timestamp       = on; }
void cpx_log_set_location        (bool on)           { g_cpx_log.show_location   = on; }
void cpx_log_set_show_category   (bool on)           { g_cpx_log.show_category   = on; }
void cpx_log_set_werror          (bool on)           { g_cpx_log.werror          = on; }
void cpx_log_set_show_phase_times(bool on)           { g_cpx_log.show_phase_times= on; }

void cpx_log_set_cat_level(CpxLogCategory cat, CpxLogLevel level) {
    if (cat < CPX_CAT_COUNT) g_cpx_log.cat_level[cat] = level;
}

void cpx_log_enable_cat (CpxLogCategory cat) { if (cat<CPX_CAT_COUNT) g_cpx_log.enabled_cats |=  (1u<<cat); }
void cpx_log_disable_cat(CpxLogCategory cat) { if (cat<CPX_CAT_COUNT) g_cpx_log.enabled_cats &= ~(1u<<cat); }
void cpx_log_enable_all_cats (void) { g_cpx_log.enabled_cats = 0xFFFFFFFFu; }
void cpx_log_disable_all_cats(void) { g_cpx_log.enabled_cats = 0; }
bool cpx_log_cat_enabled(CpxLogCategory cat) {
    return cat < CPX_CAT_COUNT && (g_cpx_log.enabled_cats & (1u << cat)) != 0;
}

bool cpx_log_add_file(const char *path) {
    if (!path || g_cpx_log.file_count >= CPX_LOG_MAX_FILES) return false;
    FILE *f = fopen(path, "a");
    if (!f) return false;
    int i = g_cpx_log.file_count++;
    g_cpx_log.file[i] = f;
    strncpy(g_cpx_log.file_path[i], path, 255);
    g_cpx_log.file_path[i][255] = '\0';
    return true;
}

void cpx_log_remove_file(const char *path) {
    for (int i = 0; i < g_cpx_log.file_count; i++) {
        if (strcmp(g_cpx_log.file_path[i], path) == 0) {
            if (g_cpx_log.file[i]) fclose(g_cpx_log.file[i]);
            /* Compact */
            for (int j = i; j < g_cpx_log.file_count - 1; j++) {
                g_cpx_log.file[j] = g_cpx_log.file[j+1];
                memcpy(g_cpx_log.file_path[j], g_cpx_log.file_path[j+1], 256);
            }
            g_cpx_log.file_count--;
            return;
        }
    }
}

void cpx_log_close_files(void) {
    for (int i = 0; i < g_cpx_log.file_count; i++) {
        if (g_cpx_log.file[i]) { fclose(g_cpx_log.file[i]); g_cpx_log.file[i] = NULL; }
        g_cpx_log.file_path[i][0] = '\0';
    }
    g_cpx_log.file_count = 0;
}

/* ============================================================
 * Public: string <-> enum helpers
 * ============================================================ */

const char *cpx_log_level_name(CpxLogLevel l) {
    return (l >= 0 && l <= CPX_LOG_OFF) ? s_level_name[l] : "UNKNOWN";
}
const char *cpx_log_cat_name(CpxLogCategory cat) {
    return (cat >= 0 && cat < CPX_CAT_COUNT) ? s_cat_name[cat] : "UNKNOWN";
}

CpxLogLevel cpx_log_level_from_str(const char *s) {
    if (!s) return CPX_LOG_INFO;
    for (int i = 0; i <= CPX_LOG_OFF; i++)
        if (strcasecmp(s, s_level_name[i]) == 0) return (CpxLogLevel)i;
    if (strcasecmp(s, "warning") == 0) return CPX_LOG_WARN;
    if (strcasecmp(s, "verbose") == 0) return CPX_LOG_DEBUG;
    if (strcasecmp(s, "quiet")   == 0) return CPX_LOG_ERROR;
    if (strcasecmp(s, "silent")  == 0) return CPX_LOG_OFF;
    return CPX_LOG_INFO;
}

CpxLogCategory cpx_log_cat_from_str(const char *s) {
    if (!s) return CPX_CAT_GENERAL;
    for (int i = 0; i < CPX_CAT_COUNT; i++)
        if (strcasecmp(s, s_cat_name[i]) == 0) return (CpxLogCategory)i;
    return CPX_CAT_GENERAL;
}

/* ============================================================
 * Public: core logging
 * ============================================================ */

void cpx_log_writev(CpxLogLevel level, CpxLogCategory cat,
                    const char *src_file, int src_line, const char *src_func,
                    const char *fmt, va_list ap)
{
    /* Lazy init */
    if (!g_cpx_log.stream) {
        g_cpx_log.stream = stderr;
        init_timer();
    }

    /* Level / category gates */
    if (level < effective_level(cat)) return;
    if (cat < CPX_CAT_COUNT && !cpx_log_cat_enabled(cat)) return;

    /* Format the message into a local buffer (avoids double va_list use) */
    char msg[4096];
    vsnprintf(msg, sizeof msg, fmt, ap);

    dispatch(level, cat, src_file, src_line, src_func, msg);

    if (level == CPX_LOG_FATAL) {
        cpx_log_shutdown();
        abort();
    }
}

void cpx_log_write(CpxLogLevel level, CpxLogCategory cat,
                   const char *src_file, int src_line, const char *src_func,
                   const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    cpx_log_writev(level, cat, src_file, src_line, src_func, fmt, ap);
    va_end(ap);
}

/* ============================================================
 * Public: source diagnostics
 * ============================================================ */

void cpx_diagv(CpxDiagSeverity sev, const CpxSourceSpan *span,
               const char *fmt, va_list ap)
{
    /* Respect -Werror */
    if (sev == CPX_DIAG_WARN && g_cpx_log.werror) sev = CPX_DIAG_ERROR;

    if (sev == CPX_DIAG_ERROR) s_error_count++;
    else if (sev == CPX_DIAG_WARN) s_warn_count++;

    if (s_progress_active) cpx_log_progress_clear();

    FILE *out = g_cpx_log.stream ? g_cpx_log.stream : stderr;
    bool  color = g_cpx_log.color && stream_is_tty(out);

    /* Format the user message */
    char msg[4096];
    vsnprintf(msg, sizeof msg, fmt, ap);

    /* --- Header line: file.cpx:12:8: error: message --- */
    if (span && span->file) {
        if (color)
            fprintf(out, A_BOLD "%s:%d:%d: %s%s" A_RESET ": %s\n",
                    span->file, span->line, span->start_col,
                    s_diag_color[sev], s_diag_name[sev],
                    msg);
        else
            fprintf(out, "%s:%d:%d: %s: %s\n",
                    span->file, span->line, span->start_col,
                    s_diag_name[sev], msg);
    } else {
        if (color)
            fprintf(out, "%s%s" A_RESET ": %s\n",
                    s_diag_color[sev], s_diag_name[sev], msg);
        else
            fprintf(out, "%s: %s\n", s_diag_name[sev], msg);
    }

    /* --- Source excerpt + caret annotation --- */
    if (span && span->source_line) {
        /* Indent the source line by 2 spaces */
        fprintf(out, "  %s\n", span->source_line);

        /* Build caret line */
        int sc = span->start_col > 0 ? span->start_col - 1 : 0;
        int ec = span->end_col   > 0 ? span->end_col   - 1 : sc + 1;
        if (ec <= sc) ec = sc + 1;

        /* Cap to a sane width */
        if (ec - sc > 120) ec = sc + 120;

        /* Leading spaces to align with the source text */
        char caret_buf[256];
        int  buf_pos = 0;
        /* account for the 2-space indent */
        for (int i = 0; i < sc + 2 && buf_pos < 250; i++)
            caret_buf[buf_pos++] = ' ';
        for (int i = sc; i < ec && buf_pos < 250; i++)
            caret_buf[buf_pos++] = '^';
        caret_buf[buf_pos] = '\0';

        if (color)
            fprintf(out, "%s%s%s\n", s_diag_color[sev], caret_buf, A_RESET);
        else
            fprintf(out, "%s\n", caret_buf);
    }

    fflush(out);

    /* Mirror to file sinks (no color) */
    for (int i = 0; i < g_cpx_log.file_count; i++) {
        FILE *f = g_cpx_log.file[i];
        if (!f) continue;
        if (span && span->file)
            fprintf(f, "%s:%d:%d: %s: %s\n",
                    span->file, span->line, span->start_col, s_diag_name[sev], msg);
        else
            fprintf(f, "%s: %s\n", s_diag_name[sev], msg);
        if (span && span->source_line) {
            fprintf(f, "  %s\n", span->source_line);
            /* Plain caret without color */
            int sc = span->start_col > 0 ? span->start_col - 1 : 0;
            int ec = span->end_col   > 0 ? span->end_col   - 1 : sc + 1;
            if (ec <= sc) ec = sc + 1;
            if (ec - sc > 120) ec = sc + 120;
            fprintf(f, "  ");
            for (int c = 0; c < sc; c++) fputc(' ', f);
            for (int c = sc; c < ec; c++) fputc('^', f);
            fputc('\n', f);
        }
    }
}

void cpx_diag(CpxDiagSeverity sev, const CpxSourceSpan *span,
              const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    cpx_diagv(sev, span, fmt, ap);
    va_end(ap);
}

/* ============================================================
 * Public: counters & summary
 * ============================================================ */

int  cpx_diag_error_count (void) { return s_error_count; }
int  cpx_diag_warn_count  (void) { return s_warn_count;  }
void cpx_diag_reset_counts(void) { s_error_count = 0; s_warn_count = 0; }

bool cpx_diag_summary(void) {
    FILE *out = g_cpx_log.stream ? g_cpx_log.stream : stderr;
    bool  color = g_cpx_log.color && stream_is_tty(out);

    if (s_error_count == 0 && s_warn_count == 0) return false;

    if (color) {
        if (s_error_count > 0)
            fprintf(out, A_BRED "%d error%s" A_RESET, s_error_count,
                    s_error_count == 1 ? "" : "s");
        if (s_error_count > 0 && s_warn_count > 0) fputs(", ", out);
        if (s_warn_count > 0)
            fprintf(out, A_BYELLOW "%d warning%s" A_RESET, s_warn_count,
                    s_warn_count == 1 ? "" : "s");
    } else {
        if (s_error_count > 0)
            fprintf(out, "%d error%s", s_error_count, s_error_count==1?"":"s");
        if (s_error_count > 0 && s_warn_count > 0) fputs(", ", out);
        if (s_warn_count > 0)
            fprintf(out, "%d warning%s", s_warn_count, s_warn_count==1?"":"s");
    }
    fputs(" generated.\n", out);
    fflush(out);
    return s_error_count > 0;
}

/* ============================================================
 * Public: phase timing
 * ============================================================ */

void cpx_phase_begin(const char *name) {
    if (!name || s_phase_depth >= CPX_PHASE_MAX_DEPTH) return;
    init_timer();
    PhaseEntry *e = &s_phase_stack[s_phase_depth++];
    strncpy(e->name, name, CPX_PHASE_NAME_MAX - 1);
    e->name[CPX_PHASE_NAME_MAX - 1] = '\0';
    e->start_ms = now_ms();
}

void cpx_phase_end(const char *name) {
    if (!name || s_phase_depth <= 0) return;

    /* Find matching entry (allows out-of-order ends for nested phases) */
    int idx = -1;
    for (int i = s_phase_depth - 1; i >= 0; i--) {
        if (strcmp(s_phase_stack[i].name, name) == 0) { idx = i; break; }
    }
    if (idx < 0) return;

    double elapsed = now_ms() - s_phase_stack[idx].start_ms;

    /* Remove from stack */
    for (int i = idx; i < s_phase_depth - 1; i++)
        s_phase_stack[i] = s_phase_stack[i+1];
    s_phase_depth--;

    /* Accumulate into records */
    int rec = -1;
    for (int i = 0; i < s_phase_record_count; i++) {
        if (strcmp(s_phase_records[i].name, name) == 0) { rec = i; break; }
    }
    if (rec < 0 && s_phase_record_count < CPX_MAX_PHASES) {
        rec = s_phase_record_count++;
        strncpy(s_phase_records[rec].name, name, CPX_PHASE_NAME_MAX - 1);
        s_phase_records[rec].name[CPX_PHASE_NAME_MAX - 1] = '\0';
        s_phase_records[rec].total_ms = 0.0;
        s_phase_records[rec].calls    = 0;
    }
    if (rec >= 0) {
        s_phase_records[rec].total_ms += elapsed;
        s_phase_records[rec].calls++;
    }

    /* Inline log if enabled */
    if (g_cpx_log.show_phase_times) {
        FILE *out = g_cpx_log.stream ? g_cpx_log.stream : stderr;
        bool  color = g_cpx_log.color && stream_is_tty(out);
        if (color)
            fprintf(out, A_DIM "[TIMING] %-20s %.3f ms" A_RESET "\n", name, elapsed);
        else
            fprintf(out, "[TIMING] %-20s %.3f ms\n", name, elapsed);
        fflush(out);
    }
}

void cpx_phase_report(void) {
    if (s_phase_record_count == 0) return;
    FILE *out = g_cpx_log.stream ? g_cpx_log.stream : stderr;
    bool  color = g_cpx_log.color && stream_is_tty(out);

    if (color) fprintf(out, A_BOLD);
    fprintf(out, "\n%-22s %10s %8s %12s\n",
            "Phase", "Total(ms)", "Calls", "Avg(ms)");
    fprintf(out, "%-22s %10s %8s %12s\n",
            "-----", "---------", "-----", "-------");
    if (color) fprintf(out, A_RESET);

    double grand = 0.0;
    for (int i = 0; i < s_phase_record_count; i++) {
        PhaseRecord *r = &s_phase_records[i];
        double avg = r->calls > 0 ? r->total_ms / r->calls : 0.0;
        fprintf(out, "%-22s %10.3f %8d %12.3f\n",
                r->name, r->total_ms, r->calls, avg);
        grand += r->total_ms;
    }
    fprintf(out, "%-22s %10.3f\n", "TOTAL", grand);
    fputc('\n', out);
    fflush(out);
}

void cpx_phase_reset(void) {
    s_phase_depth        = 0;
    s_phase_record_count = 0;
}

/* ============================================================
 * Public: progress output
 * ============================================================ */

void cpx_log_progress(int step, int total, const char *label) {
    FILE *out = g_cpx_log.stream ? g_cpx_log.stream : stderr;
    if (!stream_is_tty(out)) return;  /* don't emit \r to files/pipes */

    bool color = g_cpx_log.color;
    if (color)
        fprintf(out, "\r" A_DIM "[%d/%d]" A_RESET " %s ...",
                step, total, label ? label : "");
    else
        fprintf(out, "\r[%d/%d] %s ...", step, total, label ? label : "");

    fflush(out);
    s_progress_active = true;
}

void cpx_log_progress_clear(void) {
    if (!s_progress_active) return;
    FILE *out = g_cpx_log.stream ? g_cpx_log.stream : stderr;
    if (stream_is_tty(out)) {
        /* Overwrite with spaces then return to start of line */
        fprintf(out, "\r%80s\r", "");
        fflush(out);
    }
    s_progress_active = false;
}
