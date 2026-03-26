/*
 * log.h — Casprix Compiler Logging & Diagnostics
 *
 * Three output channels:
 *
 *   1. Internal trace/debug/info/warn/error/fatal — compiler pass messages.
 *
 *   2. Source diagnostics — clang/rustc-style messages anchored to a
 *      location in the user's source file (file.cpx:3:12: error: …)
 *      with optional source-line excerpt and caret annotation.
 *
 *   3. Phase timing — per-pass stopwatch for profiling compilation.
 *
 * Copyright (c) 2026 Casprix Project
 * SPDX-License-Identifier: MIT
 */

#ifndef CPX_LOG_H
#define CPX_LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 1. LOG LEVELS
 * ============================================================ */

typedef enum CpxLogLevel {
    CPX_LOG_TRACE = 0,   /* Token-by-token tracing, IR dumps        */
    CPX_LOG_DEBUG,       /* Internal state, pass details            */
    CPX_LOG_INFO,        /* High-level progress messages            */
    CPX_LOG_WARN,        /* Suspicious internal condition           */
    CPX_LOG_ERROR,       /* Internal compiler error (non-fatal)     */
    CPX_LOG_FATAL,       /* Unrecoverable — logs then abort()       */
    CPX_LOG_OFF          /* Disable logging entirely                */
} CpxLogLevel;

/* ============================================================
 * 2. LOG CATEGORIES
 * ============================================================ */

typedef enum CpxLogCategory {
    CPX_CAT_GENERAL  = 0,   /* Catch-all / driver messages          */
    CPX_CAT_DRIVER,         /* CLI argument handling, pipeline      */
    CPX_CAT_LEXER,          /* Tokenisation                         */
    CPX_CAT_PARSER,         /* Parsing / AST construction           */
    CPX_CAT_SEMA,           /* Semantic analysis & type checking    */
    CPX_CAT_MIR,            /* Mid-level IR construction & passes   */
    CPX_CAT_OPT,            /* Optimisation passes                  */
    CPX_CAT_CODEGEN,        /* x86-64 assembly emission             */
    CPX_CAT_REGALLOC,       /* Register allocation                  */
    CPX_CAT_LINKER,         /* Linking & symbol resolution          */
    CPX_CAT_RUNTIME,        /* Runtime library internals            */
    CPX_CAT_MEMORY,         /* ARC / GC / arena allocator           */
    CPX_CAT_IO,             /* File I/O, module loading             */
    CPX_CAT_PKGMGR,         /* Package manager                      */
    CPX_CAT_COUNT           /* Sentinel                             */
} CpxLogCategory;

/* ============================================================
 * 3. SOURCE SPAN  (for user-facing diagnostics)
 * ============================================================ */

/*
 * Half-open byte range [start_col, end_col) on one line.
 * end_col = 0  =>  single-character caret at start_col.
 * source_line = NULL  =>  suppress excerpt/caret rendering.
 */
typedef struct CpxSourceSpan {
    const char *file;
    int         line;        /* 1-based                   */
    int         start_col;   /* 1-based                   */
    int         end_col;     /* 1-based; 0 = start_col+1  */
    const char *source_line; /* Full text of the line     */
} CpxSourceSpan;

/* Convenience initialiser */
#define CPX_SPAN(f, ln, col) \
    ((CpxSourceSpan){ .file=(f), .line=(ln), .start_col=(col), \
                      .end_col=0, .source_line=NULL })

/* ============================================================
 * 4. DIAGNOSTIC SEVERITY  (user-facing)
 * ============================================================ */

typedef enum CpxDiagSeverity {
    CPX_DIAG_ERROR = 0,   /* error:   compilation will fail        */
    CPX_DIAG_WARN,        /* warning: compilation may continue     */
    CPX_DIAG_NOTE,        /* note:    supplemental context         */
    CPX_DIAG_HINT         /* hint:    actionable suggestion        */
} CpxDiagSeverity;

/* ============================================================
 * 5. LOG FORMAT
 * ============================================================ */

typedef enum CpxLogFormat {
    CPX_LOG_FMT_TEXT = 0,  /* Human-readable, ANSI color on TTY */
    CPX_LOG_FMT_JSON       /* One JSON object per line           */
} CpxLogFormat;

/* ============================================================
 * 6. LOGGER CONFIG
 * ============================================================ */

#define CPX_LOG_MAX_FILES 4

typedef struct CpxLogConfig {
    CpxLogLevel  min_level;
    CpxLogLevel  cat_level[CPX_CAT_COUNT]; /* CPX_LOG_OFF = use global min */
    uint32_t     enabled_cats;             /* bitmask: bit N = CPX_CAT_N   */

    FILE        *stream;                   /* Console sink (default: stderr) */
    CpxLogFormat format;

    bool  color;
    bool  timestamp;
    bool  show_location;
    bool  show_category;
    bool  werror;             /* Promote diagnostic WARN -> ERROR   */
    bool  show_phase_times;

    FILE *file[CPX_LOG_MAX_FILES];
    char  file_path[CPX_LOG_MAX_FILES][256];
    int   file_count;
} CpxLogConfig;

extern CpxLogConfig g_cpx_log;

/* ============================================================
 * 7. INIT & TEARDOWN
 * ============================================================ */

/* Quick start: level + auto-detect color support */
void cpx_log_init     (CpxLogLevel min_level);

/* Full setup */
void cpx_log_init_full(CpxLogLevel min_level, FILE *stream,
                       bool color, CpxLogFormat format);

/* Flush, close file sinks, reset state */
void cpx_log_shutdown (void);

/* ============================================================
 * 8. RUNTIME CONFIGURATION
 * ============================================================ */

void cpx_log_set_level           (CpxLogLevel level);
void cpx_log_set_stream          (FILE *stream);
void cpx_log_set_color           (bool on);
void cpx_log_set_format          (CpxLogFormat fmt);
void cpx_log_set_timestamp       (bool on);
void cpx_log_set_location        (bool on);
void cpx_log_set_show_category   (bool on);
void cpx_log_set_werror          (bool on);
void cpx_log_set_show_phase_times(bool on);

/* Per-category level override.  Pass CPX_LOG_OFF to clear. */
void cpx_log_set_cat_level(CpxLogCategory cat, CpxLogLevel level);

/* Category gates */
void cpx_log_enable_cat     (CpxLogCategory cat);
void cpx_log_disable_cat    (CpxLogCategory cat);
void cpx_log_enable_all_cats (void);
void cpx_log_disable_all_cats(void);
bool cpx_log_cat_enabled    (CpxLogCategory cat);

/* File sinks — plain text, no color */
bool cpx_log_add_file   (const char *path);   /* returns false on open error */
void cpx_log_remove_file(const char *path);
void cpx_log_close_files(void);

/* String <-> enum helpers (for --log-level=, --log-cat= flags) */
CpxLogLevel    cpx_log_level_from_str(const char *s);
CpxLogCategory cpx_log_cat_from_str  (const char *s);
const char    *cpx_log_level_name    (CpxLogLevel level);
const char    *cpx_log_cat_name      (CpxLogCategory cat);

/* ============================================================
 * 9. CORE LOGGING  (internal compiler messages)
 * ============================================================ */

/*
 * cpx_log_write -- single write path; all macros funnel here.
 * The 'file/line/func' arguments are the *compiler source* location
 * (__FILE__, __LINE__, __func__), not the user's source location.
 */
void cpx_log_write(CpxLogLevel level, CpxLogCategory cat,
                   const char *file, int line, const char *func,
                   const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 6, 7)))
#endif
    ;

void cpx_log_writev(CpxLogLevel level, CpxLogCategory cat,
                    const char *file, int line, const char *func,
                    const char *fmt, va_list ap);

/* --- General macros (CPX_CAT_GENERAL) --- */
#define CPX_TRACE(...) \
    cpx_log_write(CPX_LOG_TRACE, CPX_CAT_GENERAL, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define CPX_DEBUG(...) \
    cpx_log_write(CPX_LOG_DEBUG, CPX_CAT_GENERAL, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define CPX_INFO(...) \
    cpx_log_write(CPX_LOG_INFO,  CPX_CAT_GENERAL, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define CPX_WARN(...) \
    cpx_log_write(CPX_LOG_WARN,  CPX_CAT_GENERAL, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define CPX_ERROR(...) \
    cpx_log_write(CPX_LOG_ERROR, CPX_CAT_GENERAL, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define CPX_FATAL(...) \
    cpx_log_write(CPX_LOG_FATAL, CPX_CAT_GENERAL, __FILE__, __LINE__, __func__, __VA_ARGS__)

/* --- Category-aware macro --- */
#define CPX_LOG(level, cat, ...) \
    cpx_log_write((level), (cat), __FILE__, __LINE__, __func__, __VA_ARGS__)

/* --- Pass-specific debug shorthands --- */
#define CPX_LEX_TRACE(...)   CPX_LOG(CPX_LOG_TRACE, CPX_CAT_LEXER,    __VA_ARGS__)
#define CPX_LEX_DEBUG(...)   CPX_LOG(CPX_LOG_DEBUG, CPX_CAT_LEXER,    __VA_ARGS__)
#define CPX_PARSE_TRACE(...) CPX_LOG(CPX_LOG_TRACE, CPX_CAT_PARSER,   __VA_ARGS__)
#define CPX_PARSE_DEBUG(...) CPX_LOG(CPX_LOG_DEBUG, CPX_CAT_PARSER,   __VA_ARGS__)
#define CPX_SEMA_DEBUG(...)  CPX_LOG(CPX_LOG_DEBUG, CPX_CAT_SEMA,     __VA_ARGS__)
#define CPX_MIR_DEBUG(...)   CPX_LOG(CPX_LOG_DEBUG, CPX_CAT_MIR,      __VA_ARGS__)
#define CPX_OPT_DEBUG(...)   CPX_LOG(CPX_LOG_DEBUG, CPX_CAT_OPT,      __VA_ARGS__)
#define CPX_CG_DEBUG(...)    CPX_LOG(CPX_LOG_DEBUG, CPX_CAT_CODEGEN,  __VA_ARGS__)
#define CPX_RA_DEBUG(...)    CPX_LOG(CPX_LOG_DEBUG, CPX_CAT_REGALLOC, __VA_ARGS__)

/* --- Utility macros --- */
#define CPX_LOG_IF(cond, level, cat, ...) \
    do { if (cond) CPX_LOG((level), (cat), __VA_ARGS__); } while (0)

/* Log at most once per call-site */
#define CPX_WARN_ONCE(...) \
    do { \
        static bool _cpx_warned_ = false; \
        if (!_cpx_warned_) { \
            _cpx_warned_ = true; \
            CPX_LOG(CPX_LOG_WARN, CPX_CAT_GENERAL, __VA_ARGS__); \
        } \
    } while (0)

/* Assert -- logs at FATAL (which calls abort()) on failure */
#define CPX_ASSERT(expr) \
    do { \
        if (!(expr)) \
            cpx_log_write(CPX_LOG_FATAL, CPX_CAT_GENERAL, \
                          __FILE__, __LINE__, __func__, \
                          "assertion failed: %s", #expr); \
    } while (0)

#define CPX_ASSERT_MSG(expr, ...) \
    do { \
        if (!(expr)) \
            cpx_log_write(CPX_LOG_FATAL, CPX_CAT_GENERAL, \
                          __FILE__, __LINE__, __func__, __VA_ARGS__); \
    } while (0)

/* Unreachable branch */
#define CPX_UNREACHABLE(...) \
    cpx_log_write(CPX_LOG_FATAL, CPX_CAT_GENERAL, \
                  __FILE__, __LINE__, __func__, "unreachable: " __VA_ARGS__)

/* ============================================================
 * 10. SOURCE DIAGNOSTICS  (user-facing compiler messages)
 *
 *  Emits output like:
 *
 *    src/main.cpx:12:8: error: undefined variable 'foo'
 *      let x = foo + 1
 *              ^^^
 *    src/main.cpx:5:3: note: 'foo' was declared here
 *
 * ============================================================ */

void cpx_diag (CpxDiagSeverity sev, const CpxSourceSpan *span,
               const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

void cpx_diagv(CpxDiagSeverity sev, const CpxSourceSpan *span,
               const char *fmt, va_list ap);

/* Convenience wrappers */
#define cpx_diag_error(span, ...) cpx_diag(CPX_DIAG_ERROR, (span), __VA_ARGS__)
#define cpx_diag_warn(span,  ...) cpx_diag(CPX_DIAG_WARN,  (span), __VA_ARGS__)
#define cpx_diag_note(span,  ...) cpx_diag(CPX_DIAG_NOTE,  (span), __VA_ARGS__)
#define cpx_diag_hint(span,  ...) cpx_diag(CPX_DIAG_HINT,  (span), __VA_ARGS__)

/* Span-less variants (no source excerpt) */
#define cpx_diag_error_noloc(...) cpx_diag(CPX_DIAG_ERROR, NULL, __VA_ARGS__)
#define cpx_diag_warn_noloc(...)  cpx_diag(CPX_DIAG_WARN,  NULL, __VA_ARGS__)
#define cpx_diag_note_noloc(...)  cpx_diag(CPX_DIAG_NOTE,  NULL, __VA_ARGS__)

/* ============================================================
 * 11. ERROR / WARNING COUNTERS
 * ============================================================ */

int  cpx_diag_error_count (void);
int  cpx_diag_warn_count  (void);
void cpx_diag_reset_counts(void);

/*
 * Prints "N error(s), M warning(s) generated." to the log stream.
 * Returns true if error_count > 0  (caller can: if (cpx_diag_summary()) exit(1)).
 */
bool cpx_diag_summary(void);

/* ============================================================
 * 12. PHASE TIMING
 *
 *   cpx_phase_begin("parser");
 *   ... parse ...
 *   cpx_phase_end("parser");
 *   // [TIMING] parser           1.234 ms
 *
 *   cpx_phase_report();   // table of all accumulated phases
 *
 * ============================================================ */

#define CPX_PHASE_MAX_DEPTH 16
#define CPX_PHASE_NAME_MAX  32

void cpx_phase_begin (const char *name);
void cpx_phase_end   (const char *name);  /* name must match paired begin */
void cpx_phase_report(void);
void cpx_phase_reset (void);

/* RAII-style scoped phase (GCC/Clang cleanup attribute) */
#if defined(__GNUC__) || defined(__clang__)
static inline void cpx_phase_end_cleanup_(const char **name) {
    if (*name) cpx_phase_end(*name);
}
#define CPX_PHASE_SCOPE(name_str) \
    const char *_cpx_phase_scope_ __attribute__((cleanup(cpx_phase_end_cleanup_))) \
        = (cpx_phase_begin(name_str), (name_str))
#endif

/* ============================================================
 * 13. PROGRESS OUTPUT
 *
 *   [2/5] Compiling src/parser.cpx ...
 *
 *   Uses \r on TTY so progress lines don't clutter scrollback.
 * ============================================================ */

void cpx_log_progress      (int step, int total, const char *label);
void cpx_log_progress_clear(void);

/* ============================================================
 * 14. BACKWARD-COMPAT ALIASES  (remove once all callers migrated)
 * ============================================================ */

/* Old nd_log / nd_logv function names */
#define nd_log(level, cat, file, line, func, ...) \
    cpx_log_write((level), (cat), (file), (line), (func), __VA_ARGS__)
#define nd_logv(level, cat, file, line, func, fmt, ap) \
    cpx_log_writev((level), (cat), (file), (line), (func), (fmt), (ap))

/* Old single-category macros */
#define CPX_LOG_TRACE(...) CPX_TRACE(__VA_ARGS__)
#define CPX_LOG_DEBUG(...) CPX_DEBUG(__VA_ARGS__)
#define CPX_LOG_INFO(...)  CPX_INFO(__VA_ARGS__)
#define CPX_LOG_WARN(...)  CPX_WARN(__VA_ARGS__)
#define CPX_LOG_ERROR(...) CPX_ERROR(__VA_ARGS__)
#define CPX_LOG_FATAL(...) CPX_FATAL(__VA_ARGS__)

/* Old category enum names */
#define CPX_LOG_CAT_GENERAL   CPX_CAT_GENERAL
#define CPX_LOG_CAT_LEXER     CPX_CAT_LEXER
#define CPX_LOG_CAT_PARSER    CPX_CAT_PARSER
#define CPX_LOG_CAT_SEMANTIC  CPX_CAT_SEMA
#define CPX_LOG_CAT_CODEGEN   CPX_CAT_CODEGEN
#define CPX_LOG_CAT_OPTIMIZER CPX_CAT_OPT
#define CPX_LOG_CAT_RUNTIME   CPX_CAT_RUNTIME
#define CPX_LOG_CAT_MEMORY    CPX_CAT_MEMORY
#define CPX_LOG_CAT_IO        CPX_CAT_IO
#define CPX_LOG_CAT_COUNT     CPX_CAT_COUNT

/* Old config type / instance names */
#define CpxLogger    CpxLogConfig
#define g_cpx_logger g_cpx_log

#ifdef __cplusplus
}
#endif

#endif /* CPX_LOG_H */
