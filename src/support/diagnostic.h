/*
 * Casprix Compiler — Production Diagnostic & Logging Engine
 *
 * Unified diagnostic system for all compiler stages.  Replaces the
 * ad-hoc fprintf-to-stderr pattern with structured, rich diagnostics
 * that support:
 *
 *   • Multi-span source annotations (primary + secondary labels)
 *   • Code snippet rendering with carets and underlines
 *   • Severity levels: error / warning / note / help / info / debug
 *   • Compiler stage tags: LEX / PARSE / AST / MIR / OPT / BORROW / …
 *   • Stable error codes (E1001 … E9999)
 *   • Four output modes: Human (colored), JSON, Minimal (CI), Verbose
 *   • Hierarchical performance tracing with timing + memory deltas
 *   • Error budget (max-errors) and deduplication
 *   • Thread-safe emission (single mutex for the output sink)
 *   • "Did-you-mean?" suggestion support
 *
 * Integration points:
 *   Lexer, Parser → diag_emit()/diag_error_at()
 *   Semantic       → diag_emit()
 *   MIR lowering   → SourceSpan propagated from AST nodes
 *   MIR opt        → diag_note() for optimisation remarks
 *   Borrow checker → diag_emit() with secondary spans
 *   Const-eval     → diag_emit()
 *   Backend        → diag_emit()
 *   VM / JIT       → perf timing hooks
 *
 * Design constraints:
 *   • All allocations use a bump arena (DiagArena) for zero-overhead
 *     bulk deallocation — diagnostics are short-lived.
 *   • SourceMap is built once from the source buffer and shared
 *     read-only by all stages.
 *   • Emission is immediate (streaming) by default; can be switched
 *     to buffered mode for IDE/LSP integration.
 */

#ifndef CASPRIX_DIAGNOSTIC_H
#define CASPRIX_DIAGNOSTIC_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════════
 * 1. SOURCE SPAN MODEL
 * ════════════════════════════════════════════════════════════════════ */

/* Compact file identifier (index into DiagEngine's file table). */
typedef uint16_t FileId;
#define FILE_ID_NONE ((FileId)0xFFFF)

/* A contiguous region of source text.
 * All offsets are byte offsets from the start of the source buffer.
 * line/column are 1-based and eagerly resolved for formatting.     */
typedef struct {
    FileId      file;           /* which source file                 */
    uint32_t    byte_start;     /* byte offset of first char         */
    uint32_t    byte_end;       /* byte offset past last char        */
    uint32_t    line_start;     /* 1-based line of span start        */
    uint32_t    col_start;      /* 1-based column of span start      */
    uint32_t    line_end;       /* 1-based line of span end          */
    uint32_t    col_end;        /* 1-based column of span end        */
} SourceSpan;

/* Sentinel for "no span available". */
#define SPAN_NONE ((SourceSpan){FILE_ID_NONE,0,0,0,0,0,0})

/* Construct a span from a single (line, col) with no end info. */
static inline SourceSpan span_from_pos(FileId f, uint32_t line, uint32_t col) {
    return (SourceSpan){ f, 0, 0, line, col, line, col };
}

/* Construct a span from a start line/col and an end line/col. */
static inline SourceSpan span_from_range(FileId f,
        uint32_t l1, uint32_t c1, uint32_t l2, uint32_t c2) {
    return (SourceSpan){ f, 0, 0, l1, c1, l2, c2 };
}

/* ════════════════════════════════════════════════════════════════════
 * 2. SOURCE MAP — line index for snippet extraction
 * ════════════════════════════════════════════════════════════════════ */

/* Pre-computed line→offset table for a single source file.
 * Built once from the source buffer, then shared read-only.        */
typedef struct {
    const char*     source;         /* raw source text (borrowed)    */
    uint32_t        source_len;     /* byte length                   */
    const char*     filename;       /* display name                  */
    FileId          id;
    uint32_t*       line_offsets;   /* offset of first byte per line */
    uint32_t        line_count;     /* total lines                   */
} SourceFile;

typedef struct {
    SourceFile*     files;
    int             file_count;
    int             file_capacity;
} SourceMap;

void       source_map_init(SourceMap* sm);
void       source_map_destroy(SourceMap* sm);
FileId     source_map_add_file(SourceMap* sm, const char* filename,
                                const char* source, uint32_t len);
SourceFile* source_map_get_file(SourceMap* sm, FileId id);

/* Extract a single source line (0-terminated, arena-allocated).
 * Returns NULL if out of range.  `out_len` receives line length.   */
const char* source_map_get_line(SourceMap* sm, FileId file,
                                 uint32_t line_1based, uint32_t* out_len);

/* Resolve byte offset → (line, col) using binary search. */
void source_map_resolve(SourceMap* sm, FileId file, uint32_t byte_offset,
                         uint32_t* out_line, uint32_t* out_col);

/* ════════════════════════════════════════════════════════════════════
 * 3. DIAGNOSTIC SEVERITY & STAGE TAGS
 * ════════════════════════════════════════════════════════════════════ */

typedef enum {
    DIAG_ERROR,         /* compilation must stop (or error budget--)  */
    DIAG_WARNING,       /* potential issue, compilation continues     */
    DIAG_NOTE,          /* extra context attached to a prior diag     */
    DIAG_HELP,          /* suggested fix                              */
    DIAG_INFO,          /* stage information (phase start/end)        */
    DIAG_DEBUG,         /* internal compiler trace                    */
} DiagSeverity;

typedef enum {
    STAGE_NONE     = 0,
    STAGE_LEX      = 1,
    STAGE_PARSE    = 2,
    STAGE_AST      = 3,
    STAGE_SEMA     = 4,
    STAGE_MIR      = 5,
    STAGE_OPT      = 6,
    STAGE_BORROW   = 7,
    STAGE_CONST    = 8,
    STAGE_CODEGEN  = 9,
    STAGE_VM       = 10,
    STAGE_JIT      = 11,
    STAGE_LINK     = 12,
    STAGE_COUNT
} DiagStage;

/* ════════════════════════════════════════════════════════════════════
 * 4. STABLE ERROR CODES
 *
 * Convention:  Exyyyy
 *   x = severity class (1=lex, 2=parse, 3=semantic, 4=type,
 *                        5=borrow, 6=mir, 7=codegen, 8=runtime)
 *   yyyy = sequential within class
 * ════════════════════════════════════════════════════════════════════ */

typedef uint16_t DiagCode;
#define DIAG_CODE_NONE          ((DiagCode)0)

/* Lexer errors  E1xxx */
#define E_LEX_UNEXPECTED_CHAR   ((DiagCode)1001)
#define E_LEX_UNTERMINATED_STR  ((DiagCode)1002)
#define E_LEX_INVALID_NUMBER    ((DiagCode)1003)
#define E_LEX_INVALID_ESCAPE    ((DiagCode)1004)

/* Parser errors E2xxx */
#define E_PARSE_UNEXPECTED_TOK  ((DiagCode)2001)
#define E_PARSE_EXPECTED_EXPR   ((DiagCode)2002)
#define E_PARSE_EXPECTED_SEMI   ((DiagCode)2003)
#define E_PARSE_EXPECTED_PAREN  ((DiagCode)2004)
#define E_PARSE_EXPECTED_BRACE  ((DiagCode)2005)
#define E_PARSE_EXPECTED_TYPE   ((DiagCode)2006)
#define E_PARSE_UNMATCHED       ((DiagCode)2007)

/* Semantic errors E3xxx */
#define E_SEMA_UNDEFINED_VAR    ((DiagCode)3001)
#define E_SEMA_REDEFINITION     ((DiagCode)3002)
#define E_SEMA_WRONG_ARG_COUNT  ((DiagCode)3003)
#define E_SEMA_NOT_CALLABLE     ((DiagCode)3004)

/* Type errors E4xxx */
#define E_TYPE_MISMATCH         ((DiagCode)4001)
#define E_TYPE_NO_CONVERSION    ((DiagCode)4002)
#define E_TYPE_INVALID_OP       ((DiagCode)4003)

/* Borrow / ownership errors E5xxx */
#define E_BORROW_USE_AFTER_MOVE ((DiagCode)5001)
#define E_BORROW_DOUBLE_MOVE   ((DiagCode)5002)
#define E_BORROW_CONFLICT       ((DiagCode)5003)
#define E_BORROW_MUT_ALIAS      ((DiagCode)5004)
#define E_BORROW_OUTLIVES       ((DiagCode)5005)
#define E_BORROW_MISSING_DROP   ((DiagCode)5006)
#define E_BORROW_DOUBLE_DROP    ((DiagCode)5007)

/* MIR errors E6xxx */
#define E_MIR_INVALID_TYPE      ((DiagCode)6001)
#define E_MIR_VALIDATION        ((DiagCode)6002)

/* Codegen errors E7xxx */
#define E_CODEGEN_UNSUPPORTED   ((DiagCode)7001)
#define E_CODEGEN_INTERNAL      ((DiagCode)7002)

/* ════════════════════════════════════════════════════════════════════
 * 5. DIAGNOSTIC LABELS — annotated source spans
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    SourceSpan  span;
    const char* message;        /* label text (arena-owned)          */
    bool        is_primary;     /* primary (^^^) vs secondary (---) */
} DiagLabel;

/* ════════════════════════════════════════════════════════════════════
 * 6. DIAGNOSTIC — one complete message
 * ════════════════════════════════════════════════════════════════════ */

#define DIAG_MAX_LABELS     8
#define DIAG_MAX_NOTES      4
#define DIAG_MAX_HELPS      4
#define DIAG_MAX_SUGGEST     4

/* A "did-you-mean" suggestion. */
typedef struct {
    const char*     suggestion;     /* the corrected text            */
    int             edit_distance;  /* Levenshtein distance          */
} DiagSuggestion;

typedef struct Diagnostic {
    DiagSeverity    severity;
    DiagStage       stage;
    DiagCode        code;           /* E1001 etc., or DIAG_CODE_NONE */
    const char*     message;        /* main message text             */

    DiagLabel       labels[DIAG_MAX_LABELS];
    int             label_count;

    const char*     notes[DIAG_MAX_NOTES];
    int             note_count;

    const char*     helps[DIAG_MAX_HELPS];
    int             help_count;

    DiagSuggestion  suggestions[DIAG_MAX_SUGGEST];
    int             suggestion_count;

    /* Linked list for buffered mode. */
    struct Diagnostic* next;
} Diagnostic;

/* ════════════════════════════════════════════════════════════════════
 * 7. DIAGNOSTIC BUILDER — fluent API
 *
 * Usage:
 *   DiagBuilder b = diag_build_error(&engine, span, "message");
 *   diag_add_note_span(&b, span2, "extra info");
 *   diag_add_help(&b, "try doing X");
 *   diag_emit(&b);
 *
 * All strings are copied into the DiagEngine's arena on emit.
 * The builder itself lives on the stack.
 * ════════════════════════════════════════════════════════════════════ */

typedef struct DiagEngine DiagEngine;

typedef struct {
    DiagEngine*     engine;
    Diagnostic      diag;
} DiagBuilder;

/* Create a builder for a specific severity. */
DiagBuilder diag_build(DiagEngine* eng, DiagSeverity sev, DiagStage stage,
                        DiagCode code, SourceSpan span, const char* fmt, ...);
DiagBuilder diag_build_error(DiagEngine* eng, DiagStage stage,
                              DiagCode code, SourceSpan span,
                              const char* fmt, ...);
DiagBuilder diag_build_warning(DiagEngine* eng, DiagStage stage,
                                DiagCode code, SourceSpan span,
                                const char* fmt, ...);

/* Add secondary labels, notes, helps. */
DiagBuilder* diag_add_label(DiagBuilder* b, SourceSpan span,
                             bool is_primary, const char* fmt, ...);
DiagBuilder* diag_add_note(DiagBuilder* b, const char* fmt, ...);
DiagBuilder* diag_add_note_span(DiagBuilder* b, SourceSpan span,
                                 const char* fmt, ...);
DiagBuilder* diag_add_help(DiagBuilder* b, const char* fmt, ...);
DiagBuilder* diag_add_suggestion(DiagBuilder* b, const char* text,
                                  int distance);

/* Emit the diagnostic (renders + updates counters). */
void diag_emit(DiagBuilder* b);

/* ════════════════════════════════════════════════════════════════════
 * 8. OUTPUT FORMAT MODES
 * ════════════════════════════════════════════════════════════════════ */

typedef enum {
    DIAG_FMT_HUMAN,     /* colored CLI output (rustc-style)          */
    DIAG_FMT_JSON,      /* machine-readable JSON (one obj per diag)  */
    DIAG_FMT_MINIMAL,   /* single-line per diag (CI mode)            */
    DIAG_FMT_VERBOSE,   /* human + compiler internals                */
} DiagFormat;

/* ════════════════════════════════════════════════════════════════════
 * 9. PERFORMANCE TRACER
 *
 * Hierarchical timing system for compiler passes.
 * ════════════════════════════════════════════════════════════════════ */

#define PERF_MAX_DEPTH  16
#define PERF_MAX_ENTRIES 128

typedef struct {
    const char*     name;           /* "MIR Lowering", "mem2reg", …  */
    DiagStage       stage;
    double          elapsed_ms;     /* wall-clock milliseconds       */
    size_t          memory_delta;   /* heap growth during this span  */
    int             depth;          /* nesting level (0 = top)       */
    int             items_processed;/* e.g. functions lowered        */
} PerfEntry;

typedef struct {
    PerfEntry       entries[PERF_MAX_ENTRIES];
    int             entry_count;

    /* Stack for nested start/end pairs. */
    struct {
        const char* name;
        DiagStage   stage;
        double      start_time;
        size_t      start_memory;
        int         items;
    } stack[PERF_MAX_DEPTH];
    int             stack_depth;

    bool            enabled;
} PerfTracer;

void perf_init(PerfTracer* pt);
void perf_start(PerfTracer* pt, const char* name, DiagStage stage);
void perf_set_items(PerfTracer* pt, int count);
void perf_end(PerfTracer* pt);
void perf_print_report(PerfTracer* pt, FILE* out, bool use_colors);
void perf_print_json(PerfTracer* pt, FILE* out);
void perf_print_compact(PerfTracer* pt, FILE* out, const char* filename, bool use_colors);

/* ════════════════════════════════════════════════════════════════════
 * 10. DIAGNOSTIC ENGINE — central coordinator
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    /* Counters per severity. */
    int counts[6]; /* indexed by DiagSeverity */
    /* Counters per stage. */
    int stage_counts[STAGE_COUNT];
} DiagStats;

struct DiagEngine {
    /* Output configuration */
    FILE*           output;         /* output stream (stderr default)*/
    DiagFormat      format;         /* rendering mode                */
    bool            use_colors;     /* ANSI color codes              */

    /* Source information */
    SourceMap       source_map;

    /* Filtering */
    DiagSeverity    min_severity;   /* DIAG_DEBUG shows everything   */
    uint32_t        stage_mask;     /* bitmask: 1<<STAGE_* to enable */
    int             max_errors;     /* stop after N errors (0=∞)     */

    /* Statistics */
    DiagStats       stats;
    bool            has_errors;     /* any DIAG_ERROR emitted?       */

    /* Buffered mode (for IDE/LSP) */
    bool            buffered;
    Diagnostic*     buffer_head;
    Diagnostic*     buffer_tail;

    /* Arena for diagnostic string copies. */
    char*           arena;
    size_t          arena_used;
    size_t          arena_capacity;

    /* Performance tracer */
    PerfTracer      perf;
};

/* Lifecycle */
void  diag_engine_init(DiagEngine* eng);
void  diag_engine_destroy(DiagEngine* eng);

/* Configuration */
void  diag_engine_set_format(DiagEngine* eng, DiagFormat fmt);
void  diag_engine_set_output(DiagEngine* eng, FILE* out);
void  diag_engine_set_colors(DiagEngine* eng, bool colors);
void  diag_engine_set_max_errors(DiagEngine* eng, int max);
void  diag_engine_set_buffered(DiagEngine* eng, bool buffered);
void  diag_engine_enable_stage(DiagEngine* eng, DiagStage stage);
void  diag_engine_disable_stage(DiagEngine* eng, DiagStage stage);
void  diag_engine_enable_all_stages(DiagEngine* eng);

/* Queries */
bool  diag_engine_has_errors(DiagEngine* eng);
int   diag_engine_error_count(DiagEngine* eng);
int   diag_engine_warning_count(DiagEngine* eng);
void  diag_engine_print_summary(DiagEngine* eng, FILE* out);
const DiagStats* diag_engine_stats(DiagEngine* eng);

/* Flush buffered diagnostics. */
void  diag_engine_flush(DiagEngine* eng);

/* Convenience: emit a simple error/warning without builder. */
void  diag_error(DiagEngine* eng, DiagStage stage, DiagCode code,
                  SourceSpan span, const char* fmt, ...);
void  diag_warning(DiagEngine* eng, DiagStage stage, DiagCode code,
                    SourceSpan span, const char* fmt, ...);
void  diag_note(DiagEngine* eng, DiagStage stage,
                 SourceSpan span, const char* fmt, ...);
void  diag_info(DiagEngine* eng, DiagStage stage, const char* fmt, ...);
void  diag_debug(DiagEngine* eng, DiagStage stage, const char* fmt, ...);

/* ════════════════════════════════════════════════════════════════════
 * 11. STAGE TAG UTILITIES
 * ════════════════════════════════════════════════════════════════════ */

const char* diag_stage_name(DiagStage stage);     /* "LEX", "PARSE" …    */
const char* diag_severity_name(DiagSeverity sev);  /* "error", "warning" … */

/* Colour codes for terminals. */
const char* diag_severity_color(DiagSeverity sev, bool colors);
const char* diag_color_reset(bool colors);
const char* diag_color_bold(bool colors);
const char* diag_color_dim(bool colors);
const char* diag_color_blue(bool colors);
const char* diag_color_cyan(bool colors);

/* ════════════════════════════════════════════════════════════════════
 * 12. GLOBAL DIAGNOSTIC ENGINE
 *
 * For convenience (most compiler passes use a single engine).
 * Initialised in main() before any compilation.
 * ════════════════════════════════════════════════════════════════════ */

extern DiagEngine g_diag;

#ifdef __cplusplus
}
#endif

#endif /* CASPRIX_DIAGNOSTIC_H */
