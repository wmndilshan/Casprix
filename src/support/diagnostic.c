/*
 * Casprix Compiler — Diagnostic Engine Implementation
 *
 * Central diagnostic collection, formatting, and emission.
 * See diagnostic.h for the API contract.
 */

#define _POSIX_C_SOURCE 200809L
#include "diagnostic.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
/* QueryPerformanceCounter for sub-millisecond timing. */
static double perf_freq_inv = 0.0;
static void perf_init_clock(void) {
    if (perf_freq_inv == 0.0) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        perf_freq_inv = 1000.0 / (double)freq.QuadPart;
    }
}
static double perf_now_ms(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * perf_freq_inv;
}
#else
static void perf_init_clock(void) {}
static double perf_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif

/* ════════════════════════════════════════════════════════════════════
 * GLOBAL INSTANCE
 * ════════════════════════════════════════════════════════════════════ */
DiagEngine g_diag;

/* ════════════════════════════════════════════════════════════════════
 * ARENA — bump allocator for diagnostic strings
 * ════════════════════════════════════════════════════════════════════ */

#define DIAG_ARENA_INITIAL (64 * 1024)

static void arena_init(DiagEngine* eng) {
    eng->arena = (char*)malloc(DIAG_ARENA_INITIAL);
    eng->arena_used = 0;
    eng->arena_capacity = DIAG_ARENA_INITIAL;
}

static char* arena_alloc(DiagEngine* eng, size_t n) {
    /* Grow if needed (double until big enough). */
    while (eng->arena_used + n > eng->arena_capacity) {
        eng->arena_capacity *= 2;
        eng->arena = (char*)realloc(eng->arena, eng->arena_capacity);
    }
    char* ptr = eng->arena + eng->arena_used;
    eng->arena_used += n;
    return ptr;
}

static const char* arena_strdup(DiagEngine* eng, const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* copy = arena_alloc(eng, len);
    memcpy(copy, s, len);
    return copy;
}

static const char* arena_vsprintf(DiagEngine* eng, const char* fmt,
                                   va_list ap) {
    /* Two-pass: measure then copy. */
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) return "";
    char* buf = arena_alloc(eng, (size_t)(n + 1));
    vsnprintf(buf, (size_t)(n + 1), fmt, ap);
    return buf;
}

static const char* arena_sprintf(DiagEngine* eng, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const char* r = arena_vsprintf(eng, fmt, ap);
    va_end(ap);
    return r;
}
/* Suppress unused-function warning — arena_sprintf is available for future use. */
static inline void arena_sprintf_suppress_unused(void) {
    (void)arena_sprintf;
}

/* ════════════════════════════════════════════════════════════════════
 * SOURCE MAP
 * ════════════════════════════════════════════════════════════════════ */

void source_map_init(SourceMap* sm) {
    memset(sm, 0, sizeof(*sm));
    sm->file_capacity = 4;
    sm->files = (SourceFile*)calloc((size_t)sm->file_capacity,
                                     sizeof(SourceFile));
}

void source_map_destroy(SourceMap* sm) {
    for (int i = 0; i < sm->file_count; i++) {
        free(sm->files[i].line_offsets);
    }
    free(sm->files);
    memset(sm, 0, sizeof(*sm));
}

FileId source_map_add_file(SourceMap* sm, const char* filename,
                            const char* source, uint32_t len) {
    if (sm->file_count >= sm->file_capacity) {
        sm->file_capacity *= 2;
        sm->files = (SourceFile*)realloc(sm->files,
                        (size_t)sm->file_capacity * sizeof(SourceFile));
    }

    FileId id = (FileId)sm->file_count;
    SourceFile* f = &sm->files[sm->file_count++];
    f->source = source;
    f->source_len = len;
    f->filename = filename;
    f->id = id;

    /* Build line offset table.  First pass: count lines. */
    uint32_t lines = 1;
    for (uint32_t i = 0; i < len; i++) {
        if (source[i] == '\n') lines++;
    }
    f->line_offsets = (uint32_t*)malloc(lines * sizeof(uint32_t));
    f->line_count = lines;

    /* Second pass: record offsets (1-indexed: line_offsets[0] = line 1). */
    uint32_t li = 0;
    f->line_offsets[li++] = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (source[i] == '\n' && li < lines) {
            f->line_offsets[li++] = i + 1;
        }
    }
    return id;
}

SourceFile* source_map_get_file(SourceMap* sm, FileId id) {
    if (id >= (FileId)sm->file_count) return NULL;
    return &sm->files[id];
}

const char* source_map_get_line(SourceMap* sm, FileId file_id,
                                 uint32_t line_1based, uint32_t* out_len) {
    SourceFile* f = source_map_get_file(sm, file_id);
    if (!f || line_1based == 0 || line_1based > f->line_count) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    uint32_t start = f->line_offsets[line_1based - 1];
    uint32_t end;
    if (line_1based < f->line_count)
        end = f->line_offsets[line_1based] - 1; /* skip '\n' */
    else
        end = f->source_len;

    /* Trim trailing \r */
    while (end > start && (f->source[end - 1] == '\r' ||
                           f->source[end - 1] == '\n'))
        end--;

    if (out_len) *out_len = end - start;
    return f->source + start;
}

void source_map_resolve(SourceMap* sm, FileId file_id,
                         uint32_t byte_offset,
                         uint32_t* out_line, uint32_t* out_col) {
    SourceFile* f = source_map_get_file(sm, file_id);
    if (!f || f->line_count == 0) {
        *out_line = 0; *out_col = 0;
        return;
    }
    /* Binary search for the line containing byte_offset. */
    uint32_t lo = 0, hi = f->line_count - 1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo + 1) / 2;
        if (f->line_offsets[mid] <= byte_offset)
            lo = mid;
        else
            hi = mid - 1;
    }
    *out_line = lo + 1; /* 1-based */
    *out_col = byte_offset - f->line_offsets[lo] + 1; /* 1-based */
}

/* ════════════════════════════════════════════════════════════════════
 * STAGE / SEVERITY NAME TABLES
 * ════════════════════════════════════════════════════════════════════ */

static const char* stage_names[] = {
    [STAGE_NONE]    = "",
    [STAGE_LEX]     = "LEX",
    [STAGE_PARSE]   = "PARSE",
    [STAGE_AST]     = "AST",
    [STAGE_SEMA]    = "SEMA",
    [STAGE_MIR]     = "MIR",
    [STAGE_OPT]     = "OPT",
    [STAGE_BORROW]  = "BORROW",
    [STAGE_CONST]   = "CONST",
    [STAGE_CODEGEN] = "CODEGEN",
    [STAGE_VM]      = "VM",
    [STAGE_JIT]     = "JIT",
    [STAGE_LINK]    = "LINK",
};

static const char* severity_names[] = {
    [DIAG_ERROR]    = "error",
    [DIAG_WARNING]  = "warning",
    [DIAG_NOTE]     = "note",
    [DIAG_HELP]     = "help",
    [DIAG_INFO]     = "info",
    [DIAG_DEBUG]    = "debug",
};

const char* diag_stage_name(DiagStage stage) {
    if (stage >= STAGE_COUNT) return "???";
    return stage_names[stage];
}

const char* diag_severity_name(DiagSeverity sev) {
    if (sev > DIAG_DEBUG) return "???";
    return severity_names[sev];
}

/* ════════════════════════════════════════════════════════════════════
 * ANSI COLOR HELPERS
 * ════════════════════════════════════════════════════════════════════ */

const char* diag_severity_color(DiagSeverity sev, bool colors) {
    if (!colors) return "";
    switch (sev) {
        case DIAG_ERROR:   return "\x1b[1;31m";  /* bold red      */
        case DIAG_WARNING: return "\x1b[1;33m";  /* bold yellow   */
        case DIAG_NOTE:    return "\x1b[1;36m";  /* bold cyan     */
        case DIAG_HELP:    return "\x1b[1;32m";  /* bold green    */
        case DIAG_INFO:    return "\x1b[1;34m";  /* bold blue     */
        case DIAG_DEBUG:   return "\x1b[2;37m";  /* dim white     */
        default:           return "";
    }
}

const char* diag_color_reset(bool colors) {
    return colors ? "\x1b[0m" : "";
}
const char* diag_color_bold(bool colors) {
    return colors ? "\x1b[1m" : "";
}
const char* diag_color_dim(bool colors) {
    return colors ? "\x1b[2m" : "";
}
const char* diag_color_blue(bool colors) {
    return colors ? "\x1b[1;34m" : "";
}
const char* diag_color_cyan(bool colors) {
    return colors ? "\x1b[1;36m" : "";
}

/* ════════════════════════════════════════════════════════════════════
 * DIAGNOSTIC ENGINE — LIFECYCLE
 * ════════════════════════════════════════════════════════════════════ */

void diag_engine_init(DiagEngine* eng) {
    memset(eng, 0, sizeof(*eng));
    eng->output = stderr;
    eng->format = DIAG_FMT_HUMAN;
    eng->use_colors = true;
    eng->min_severity = DIAG_ERROR; /* default: errors only */
    eng->stage_mask = ~(uint32_t)0; /* all stages enabled */
    eng->max_errors = 0; /* unlimited */
    source_map_init(&eng->source_map);
    arena_init(eng);
    perf_init(&eng->perf);

#ifdef _WIN32
    /* Enable ANSI escape codes on Windows 10+. */
    HANDLE hOut = GetStdHandle(STD_ERROR_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
#endif
}

void diag_engine_destroy(DiagEngine* eng) {
    source_map_destroy(&eng->source_map);
    /* Free buffered diagnostics. */
    Diagnostic* d = eng->buffer_head;
    while (d) {
        Diagnostic* next = d->next;
        free(d);
        d = next;
    }
    free(eng->arena);
    memset(eng, 0, sizeof(*eng));
}

/* ════════════════════════════════════════════════════════════════════
 * DIAGNOSTIC ENGINE — CONFIGURATION
 * ════════════════════════════════════════════════════════════════════ */

void diag_engine_set_format(DiagEngine* eng, DiagFormat fmt) {
    eng->format = fmt;
}
void diag_engine_set_output(DiagEngine* eng, FILE* out) {
    eng->output = out;
}
void diag_engine_set_colors(DiagEngine* eng, bool colors) {
    eng->use_colors = colors;
}
void diag_engine_set_max_errors(DiagEngine* eng, int max) {
    eng->max_errors = max;
}
void diag_engine_set_buffered(DiagEngine* eng, bool buffered) {
    eng->buffered = buffered;
}
void diag_engine_enable_stage(DiagEngine* eng, DiagStage stage) {
    eng->stage_mask |= (1u << stage);
}
void diag_engine_disable_stage(DiagEngine* eng, DiagStage stage) {
    eng->stage_mask &= ~(1u << stage);
}
void diag_engine_enable_all_stages(DiagEngine* eng) {
    eng->stage_mask = ~(uint32_t)0;
}

/* ════════════════════════════════════════════════════════════════════
 * DIAGNOSTIC ENGINE — QUERIES
 * ════════════════════════════════════════════════════════════════════ */

bool diag_engine_has_errors(DiagEngine* eng) {
    return eng->has_errors;
}
int diag_engine_error_count(DiagEngine* eng) {
    return eng->stats.counts[DIAG_ERROR];
}
int diag_engine_warning_count(DiagEngine* eng) {
    return eng->stats.counts[DIAG_WARNING];
}
const DiagStats* diag_engine_stats(DiagEngine* eng) {
    return &eng->stats;
}

/* ════════════════════════════════════════════════════════════════════
 * RENDERING — HUMAN FORMAT (rustc-style)
 *
 * Example output:
 *
 *   error[E3001]: undefined variable 'foo'
 *     --> example.cpx:12:5
 *      |
 *   12 |     let x = foo + 1;
 *      |             ^^^ not found in this scope
 *      |
 *      = help: did you mean 'fob'?
 *
 * ════════════════════════════════════════════════════════════════════ */

/* Width of the line-number gutter (auto-sized per diagnostic). */
static int gutter_width(uint32_t max_line) {
    int w = 1;
    while (max_line >= 10) { w++; max_line /= 10; }
    return w < 3 ? 3 : w;
}

static void render_human(DiagEngine* eng, const Diagnostic* d) {
    FILE* out = eng->output;
    bool col = eng->use_colors;

    const char* sev_color = diag_severity_color(d->severity, col);
    const char* reset = diag_color_reset(col);
    const char* bold = diag_color_bold(col);
    const char* dim = diag_color_dim(col);
    const char* blue = diag_color_blue(col);

    /* ── Header line: severity[code]: message ─────────────────── */
    fprintf(out, "%s%s", sev_color, diag_severity_name(d->severity));
    if (d->code != DIAG_CODE_NONE)
        fprintf(out, "[E%04u]", (unsigned)d->code);
    if (d->stage != STAGE_NONE)
        fprintf(out, "(%s)", diag_stage_name(d->stage));
    fprintf(out, "%s: %s%s%s\n", reset, bold, d->message, reset);

    /* ── Primary span location: --> file:line:col ─────────────── */
    SourceSpan primary = SPAN_NONE;
    const char* primary_label = NULL;
    for (int i = 0; i < d->label_count; i++) {
        if (d->labels[i].is_primary) {
            primary = d->labels[i].span;
            primary_label = d->labels[i].message;
            break;
        }
    }
    /* Fall back: use first label. */
    if (primary.file == FILE_ID_NONE && d->label_count > 0) {
        primary = d->labels[0].span;
        primary_label = d->labels[0].message;
    }

    SourceFile* sf = source_map_get_file(&eng->source_map, primary.file);

    if (primary.line_start > 0 && sf) {
        int gw = gutter_width(primary.line_end > 0 ?
                              primary.line_end : primary.line_start);

        fprintf(out, " %s%*s--> %s%s:%u:%u%s\n",
                blue, gw, "", reset,
                sf->filename ? sf->filename : "<input>",
                primary.line_start, primary.col_start, reset);

        /* ── Source snippet + caret underlines ──────────────────── */
        uint32_t first_line = primary.line_start;
        uint32_t last_line  = primary.line_end > 0 ?
                              primary.line_end : primary.line_start;

        /* Empty gutter separator. */
        fprintf(out, " %s%*s |%s\n", blue, gw, "", reset);

        for (uint32_t ln = first_line; ln <= last_line; ln++) {
            uint32_t line_len = 0;
            const char* line = source_map_get_line(&eng->source_map,
                                                    primary.file,
                                                    ln, &line_len);
            if (!line) continue;

            /* Print line with gutter. */
            fprintf(out, " %s%*u |%s ", blue, gw, ln, reset);
            fwrite(line, 1, line_len, out);
            fputc('\n', out);

            /* Print caret/underline row. */
            uint32_t caret_start = 1, caret_end = line_len + 1;
            if (ln == first_line)
                caret_start = primary.col_start > 0 ? primary.col_start : 1;
            if (ln == last_line && primary.col_end > 0)
                caret_end = primary.col_end;
            else if (ln == first_line && primary.col_start > 0 &&
                     primary.col_end == primary.col_start)
                caret_end = caret_start + 1; /* at least one caret */

            if (caret_end < caret_start) caret_end = caret_start + 1;

            fprintf(out, " %s%*s |%s ", blue, gw, "", reset);
            /* Spaces up to caret start. */
            for (uint32_t c = 1; c < caret_start; c++) {
                /* Match tab width in source. */
                if (c <= line_len && line[c - 1] == '\t')
                    fputc('\t', out);
                else
                    fputc(' ', out);
            }
            /* Carets. */
            fprintf(out, "%s", sev_color);
            for (uint32_t c = caret_start; c < caret_end; c++)
                fputc('^', out);

            /* Primary label after carets. */
            if (ln == last_line && primary_label && primary_label[0])
                fprintf(out, " %s", primary_label);

            fprintf(out, "%s\n", reset);
        }

        /* ── Secondary labels ──────────────────────────────────── */
        for (int i = 0; i < d->label_count; i++) {
            if (d->labels[i].is_primary) continue;
            const DiagLabel* lab = &d->labels[i];
            SourceFile* lf = source_map_get_file(&eng->source_map,
                                                  lab->span.file);
            if (!lf || lab->span.line_start == 0) continue;

            const char* note_color = diag_severity_color(DIAG_NOTE, col);
            fprintf(out, " %s%*s--> %s%s:%u:%u%s\n",
                    blue, gw, "", reset,
                    lf->filename ? lf->filename : "<input>",
                    lab->span.line_start, lab->span.col_start, reset);

            uint32_t ll = 0;
            const char* ltext = source_map_get_line(&eng->source_map,
                                    lab->span.file,
                                    lab->span.line_start, &ll);
            if (ltext) {
                fprintf(out, " %s%*u |%s ", blue, lab->span.line_start > 999 ? 4 : gw,
                        lab->span.line_start, reset);
                fwrite(ltext, 1, ll, out);
                fputc('\n', out);

                uint32_t cs = lab->span.col_start > 0 ? lab->span.col_start : 1;
                uint32_t ce = lab->span.col_end > cs ? lab->span.col_end : cs + 1;

                fprintf(out, " %s%*s |%s ", blue, gw, "", reset);
                for (uint32_t c = 1; c < cs; c++) {
                    if (c <= ll && ltext[c - 1] == '\t')
                        fputc('\t', out);
                    else
                        fputc(' ', out);
                }
                fprintf(out, "%s", note_color);
                for (uint32_t c = cs; c < ce; c++)
                    fputc('-', out);
                if (lab->message && lab->message[0])
                    fprintf(out, " %s", lab->message);
                fprintf(out, "%s\n", reset);
            }
        }

        /* Trailing gutter separator. */
        fprintf(out, " %s%*s |%s\n", blue, gw, "", reset);

        /* ── Notes ─────────────────────────────────────────────── */
        for (int i = 0; i < d->note_count; i++) {
            const char* nc = diag_severity_color(DIAG_NOTE, col);
            fprintf(out, " %s%*s = %snote%s: %s\n",
                    blue, gw, "", nc, reset, d->notes[i]);
        }

        /* ── Helps ─────────────────────────────────────────────── */
        for (int i = 0; i < d->help_count; i++) {
            const char* hc = diag_severity_color(DIAG_HELP, col);
            fprintf(out, " %s%*s = %shelp%s: %s\n",
                    blue, gw, "", hc, reset, d->helps[i]);
        }

        /* ── Suggestions ───────────────────────────────────────── */
        for (int i = 0; i < d->suggestion_count; i++) {
            const char* hc = diag_severity_color(DIAG_HELP, col);
            fprintf(out, " %s%*s = %shelp%s: did you mean '%s'?\n",
                    blue, gw, "", hc, reset, d->suggestions[i].suggestion);
        }
    } else if (d->label_count == 0 && primary.file == FILE_ID_NONE) {
        /* No span at all — just print notes/helps inline. */
        for (int i = 0; i < d->note_count; i++) {
            const char* nc = diag_severity_color(DIAG_NOTE, col);
            fprintf(out, " %s= %snote%s: %s\n", dim, nc, reset, d->notes[i]);
        }
        for (int i = 0; i < d->help_count; i++) {
            const char* hc = diag_severity_color(DIAG_HELP, col);
            fprintf(out, " %s= %shelp%s: %s\n", dim, hc, reset, d->helps[i]);
        }
    }

    fprintf(out, "\n");
}

/* ════════════════════════════════════════════════════════════════════
 * RENDERING — JSON FORMAT
 * ════════════════════════════════════════════════════════════════════ */

static void json_escape(FILE* out, const char* s) {
    if (!s) { fprintf(out, "null"); return; }
    fputc('"', out);
    for (; *s; s++) {
        switch (*s) {
            case '"':  fprintf(out, "\\\""); break;
            case '\\': fprintf(out, "\\\\"); break;
            case '\n': fprintf(out, "\\n"); break;
            case '\r': fprintf(out, "\\r"); break;
            case '\t': fprintf(out, "\\t"); break;
            default:   fputc(*s, out);
        }
    }
    fputc('"', out);
}

static void render_json(DiagEngine* eng, const Diagnostic* d) {
    FILE* out = eng->output;
    fprintf(out, "{\"severity\":");
    json_escape(out, diag_severity_name(d->severity));

    if (d->code != DIAG_CODE_NONE)
        fprintf(out, ",\"code\":\"E%04u\"", (unsigned)d->code);
    if (d->stage != STAGE_NONE) {
        fprintf(out, ",\"stage\":");
        json_escape(out, diag_stage_name(d->stage));
    }
    fprintf(out, ",\"message\":");
    json_escape(out, d->message);

    /* Labels. */
    fprintf(out, ",\"labels\":[");
    for (int i = 0; i < d->label_count; i++) {
        if (i) fputc(',', out);
        const DiagLabel* lab = &d->labels[i];
        SourceFile* sf = source_map_get_file(&eng->source_map, lab->span.file);
        fprintf(out, "{\"file\":");
        json_escape(out, sf ? sf->filename : NULL);
        fprintf(out, ",\"line_start\":%u,\"col_start\":%u",
                lab->span.line_start, lab->span.col_start);
        fprintf(out, ",\"line_end\":%u,\"col_end\":%u",
                lab->span.line_end, lab->span.col_end);
        fprintf(out, ",\"primary\":%s", lab->is_primary ? "true" : "false");
        fprintf(out, ",\"message\":");
        json_escape(out, lab->message);
        fputc('}', out);
    }
    fputc(']', out);

    /* Notes. */
    if (d->note_count > 0) {
        fprintf(out, ",\"notes\":[");
        for (int i = 0; i < d->note_count; i++) {
            if (i) fputc(',', out);
            json_escape(out, d->notes[i]);
        }
        fputc(']', out);
    }

    /* Helps. */
    if (d->help_count > 0) {
        fprintf(out, ",\"helps\":[");
        for (int i = 0; i < d->help_count; i++) {
            if (i) fputc(',', out);
            json_escape(out, d->helps[i]);
        }
        fputc(']', out);
    }

    /* Suggestions. */
    if (d->suggestion_count > 0) {
        fprintf(out, ",\"suggestions\":[");
        for (int i = 0; i < d->suggestion_count; i++) {
            if (i) fputc(',', out);
            fprintf(out, "{\"text\":");
            json_escape(out, d->suggestions[i].suggestion);
            fprintf(out, ",\"distance\":%d}", d->suggestions[i].edit_distance);
        }
        fputc(']', out);
    }

    fprintf(out, "}\n");
}

/* ════════════════════════════════════════════════════════════════════
 * RENDERING — MINIMAL FORMAT (CI)
 *
 * file:line:col: severity[code]: message
 * ════════════════════════════════════════════════════════════════════ */

static void render_minimal(DiagEngine* eng, const Diagnostic* d) {
    FILE* out = eng->output;

    /* Find primary span for location. */
    SourceSpan primary = SPAN_NONE;
    for (int i = 0; i < d->label_count; i++) {
        if (d->labels[i].is_primary) { primary = d->labels[i].span; break; }
    }
    if (primary.file == FILE_ID_NONE && d->label_count > 0)
        primary = d->labels[0].span;

    SourceFile* sf = source_map_get_file(&eng->source_map, primary.file);
    if (sf && primary.line_start > 0) {
        fprintf(out, "%s:%u:%u: ",
                sf->filename ? sf->filename : "<input>",
                primary.line_start, primary.col_start);
    }

    fprintf(out, "%s", diag_severity_name(d->severity));
    if (d->code != DIAG_CODE_NONE)
        fprintf(out, "[E%04u]", (unsigned)d->code);
    fprintf(out, ": %s\n", d->message);
}

/* ════════════════════════════════════════════════════════════════════
 * RENDERING — VERBOSE FORMAT (human + extra)
 * ════════════════════════════════════════════════════════════════════ */

static void render_verbose(DiagEngine* eng, const Diagnostic* d) {
    /* Verbose = human + extra metadata header. */
    FILE* out = eng->output;
    bool col = eng->use_colors;
    const char* dim = diag_color_dim(col);
    const char* reset = diag_color_reset(col);

    fprintf(out, "%s[diag: severity=%s stage=%s code=E%04u labels=%d notes=%d helps=%d]%s\n",
            dim,
            diag_severity_name(d->severity),
            diag_stage_name(d->stage),
            (unsigned)d->code,
            d->label_count, d->note_count, d->help_count,
            reset);
    render_human(eng, d);
}

/* ════════════════════════════════════════════════════════════════════
 * DIAGNOSTIC RENDERING DISPATCH
 * ════════════════════════════════════════════════════════════════════ */

static void render_diagnostic(DiagEngine* eng, const Diagnostic* d) {
    switch (eng->format) {
        case DIAG_FMT_HUMAN:   render_human(eng, d);   break;
        case DIAG_FMT_JSON:    render_json(eng, d);     break;
        case DIAG_FMT_MINIMAL: render_minimal(eng, d);  break;
        case DIAG_FMT_VERBOSE: render_verbose(eng, d);  break;
    }
}

/* ════════════════════════════════════════════════════════════════════
 * EMISSION CORE
 * ════════════════════════════════════════════════════════════════════ */

static void emit_impl(DiagEngine* eng, Diagnostic* d) {
    /* Update error flag (always, even if suppressed). */
    if (d->severity == DIAG_ERROR)
        eng->has_errors = true;

    /* Check suppression BEFORE incrementing counters so the Nth error
     * (where N == max_errors) is still emitted. */
    bool suppress = false;

    /* Severity filter. */
    if ((int)d->severity > (int)eng->min_severity &&
        d->severity != DIAG_ERROR)
        suppress = true;

    /* Stage filter. */
    if (!suppress && d->stage != STAGE_NONE &&
        !(eng->stage_mask & (1u << d->stage)))
        suppress = true;

    /* Error budget: suppress if we have ALREADY emitted max_errors. */
    if (!suppress && eng->max_errors > 0 && d->severity == DIAG_ERROR &&
        eng->stats.counts[DIAG_ERROR] >= eng->max_errors)
        suppress = true;

    /* Now increment counters. */
    if (d->severity <= DIAG_DEBUG)
        eng->stats.counts[d->severity]++;
    if (d->stage < STAGE_COUNT)
        eng->stats.stage_counts[d->stage]++;

    if (suppress)
        return;

    if (eng->buffered) {
        /* Clone into heap for buffer chain. */
        Diagnostic* copy = (Diagnostic*)malloc(sizeof(Diagnostic));
        *copy = *d;
        copy->next = NULL;
        if (eng->buffer_tail) {
            eng->buffer_tail->next = copy;
            eng->buffer_tail = copy;
        } else {
            eng->buffer_head = eng->buffer_tail = copy;
        }
    } else {
        render_diagnostic(eng, d);
    }

    /* Print "too many errors" once. */
    if (eng->max_errors > 0 && d->severity == DIAG_ERROR &&
        eng->stats.counts[DIAG_ERROR] == eng->max_errors) {
        fprintf(eng->output,
                "%serror%s: aborting due to %d previous errors\n",
                diag_severity_color(DIAG_ERROR, eng->use_colors),
                diag_color_reset(eng->use_colors),
                eng->max_errors);
    }
}

/* ════════════════════════════════════════════════════════════════════
 * BUILDER API
 * ════════════════════════════════════════════════════════════════════ */

static DiagBuilder make_builder(DiagEngine* eng, DiagSeverity sev,
                                 DiagStage stage, DiagCode code,
                                 SourceSpan span,
                                 const char* fmt, va_list ap) {
    DiagBuilder b;
    memset(&b, 0, sizeof(b));
    b.engine = eng;
    b.diag.severity = sev;
    b.diag.stage = stage;
    b.diag.code = code;
    b.diag.message = arena_vsprintf(eng, fmt, ap);

    /* Add primary label from the span (no message yet). */
    if (span.file != FILE_ID_NONE || span.line_start > 0) {
        b.diag.labels[0].span = span;
        b.diag.labels[0].is_primary = true;
        b.diag.labels[0].message = NULL;
        b.diag.label_count = 1;
    }
    return b;
}

DiagBuilder diag_build(DiagEngine* eng, DiagSeverity sev, DiagStage stage,
                        DiagCode code, SourceSpan span,
                        const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    DiagBuilder b = make_builder(eng, sev, stage, code, span, fmt, ap);
    va_end(ap);
    return b;
}

DiagBuilder diag_build_error(DiagEngine* eng, DiagStage stage,
                              DiagCode code, SourceSpan span,
                              const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    DiagBuilder b = make_builder(eng, DIAG_ERROR, stage, code, span, fmt, ap);
    va_end(ap);
    return b;
}

DiagBuilder diag_build_warning(DiagEngine* eng, DiagStage stage,
                                DiagCode code, SourceSpan span,
                                const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    DiagBuilder b = make_builder(eng, DIAG_WARNING, stage, code, span, fmt, ap);
    va_end(ap);
    return b;
}

DiagBuilder* diag_add_label(DiagBuilder* b, SourceSpan span,
                             bool is_primary, const char* fmt, ...) {
    if (b->diag.label_count < DIAG_MAX_LABELS) {
        DiagLabel* lab = &b->diag.labels[b->diag.label_count++];
        lab->span = span;
        lab->is_primary = is_primary;
        if (fmt) {
            va_list ap;
            va_start(ap, fmt);
            lab->message = arena_vsprintf(b->engine, fmt, ap);
            va_end(ap);
        } else {
            lab->message = NULL;
        }
    }
    return b;
}

DiagBuilder* diag_add_note(DiagBuilder* b, const char* fmt, ...) {
    if (b->diag.note_count < DIAG_MAX_NOTES) {
        va_list ap;
        va_start(ap, fmt);
        b->diag.notes[b->diag.note_count++] =
            arena_vsprintf(b->engine, fmt, ap);
        va_end(ap);
    }
    return b;
}

DiagBuilder* diag_add_note_span(DiagBuilder* b, SourceSpan span,
                                 const char* fmt, ...) {
    /* A note with a span becomes a secondary label. */
    if (b->diag.label_count < DIAG_MAX_LABELS) {
        DiagLabel* lab = &b->diag.labels[b->diag.label_count++];
        lab->span = span;
        lab->is_primary = false;
        if (fmt) {
            va_list ap;
            va_start(ap, fmt);
            lab->message = arena_vsprintf(b->engine, fmt, ap);
            va_end(ap);
        }
    }
    return b;
}

DiagBuilder* diag_add_help(DiagBuilder* b, const char* fmt, ...) {
    if (b->diag.help_count < DIAG_MAX_HELPS) {
        va_list ap;
        va_start(ap, fmt);
        b->diag.helps[b->diag.help_count++] =
            arena_vsprintf(b->engine, fmt, ap);
        va_end(ap);
    }
    return b;
}

DiagBuilder* diag_add_suggestion(DiagBuilder* b, const char* text,
                                  int distance) {
    if (b->diag.suggestion_count < DIAG_MAX_SUGGEST) {
        DiagSuggestion* s = &b->diag.suggestions[b->diag.suggestion_count++];
        s->suggestion = arena_strdup(b->engine, text);
        s->edit_distance = distance;
    }
    return b;
}

void diag_emit(DiagBuilder* b) {
    emit_impl(b->engine, &b->diag);
}

/* ════════════════════════════════════════════════════════════════════
 * CONVENIENCE FUNCTIONS (no builder needed)
 * ════════════════════════════════════════════════════════════════════ */

void diag_error(DiagEngine* eng, DiagStage stage, DiagCode code,
                 SourceSpan span, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    DiagBuilder b = make_builder(eng, DIAG_ERROR, stage, code, span, fmt, ap);
    va_end(ap);
    emit_impl(eng, &b.diag);
}

void diag_warning(DiagEngine* eng, DiagStage stage, DiagCode code,
                   SourceSpan span, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    DiagBuilder b = make_builder(eng, DIAG_WARNING, stage, code, span, fmt, ap);
    va_end(ap);
    emit_impl(eng, &b.diag);
}

void diag_note(DiagEngine* eng, DiagStage stage,
                SourceSpan span, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    DiagBuilder b = make_builder(eng, DIAG_NOTE, stage, DIAG_CODE_NONE,
                                  span, fmt, ap);
    va_end(ap);
    emit_impl(eng, &b.diag);
}

void diag_info(DiagEngine* eng, DiagStage stage, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    DiagBuilder b = make_builder(eng, DIAG_INFO, stage, DIAG_CODE_NONE,
                                  SPAN_NONE, fmt, ap);
    va_end(ap);
    emit_impl(eng, &b.diag);
}

void diag_debug(DiagEngine* eng, DiagStage stage, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    DiagBuilder b = make_builder(eng, DIAG_DEBUG, stage, DIAG_CODE_NONE,
                                  SPAN_NONE, fmt, ap);
    va_end(ap);
    emit_impl(eng, &b.diag);
}

/* ════════════════════════════════════════════════════════════════════
 * SUMMARY
 * ════════════════════════════════════════════════════════════════════ */

void diag_engine_print_summary(DiagEngine* eng, FILE* out) {
    bool col = eng->use_colors;
    const char* reset = diag_color_reset(col);
    int errs = eng->stats.counts[DIAG_ERROR];
    int warns = eng->stats.counts[DIAG_WARNING];

    if (errs > 0) {
        fprintf(out, "%s%s: aborting due to %d error%s",
                diag_severity_color(DIAG_ERROR, col), 
                diag_severity_name(DIAG_ERROR),
                errs, errs == 1 ? "" : "s");
        if (warns > 0)
            fprintf(out, " and %d warning%s", warns, warns == 1 ? "" : "s");
        fprintf(out, "%s\n", reset);
    } else if (warns > 0) {
        fprintf(out, "%s%s: %d warning%s emitted%s\n",
                diag_severity_color(DIAG_WARNING, col),
                diag_severity_name(DIAG_WARNING),
                warns, warns == 1 ? "" : "s", reset);
    }
}

void diag_engine_flush(DiagEngine* eng) {
    Diagnostic* d = eng->buffer_head;
    while (d) {
        render_diagnostic(eng, d);
        Diagnostic* next = d->next;
        free(d);
        d = next;
    }
    eng->buffer_head = eng->buffer_tail = NULL;
}

/* ════════════════════════════════════════════════════════════════════
 * PERFORMANCE TRACER
 * ════════════════════════════════════════════════════════════════════ */

void perf_init(PerfTracer* pt) {
    memset(pt, 0, sizeof(*pt));
    perf_init_clock();
}

void perf_start(PerfTracer* pt, const char* name, DiagStage stage) {
    if (!pt->enabled) return;
    if (pt->stack_depth >= PERF_MAX_DEPTH) return;
    int i = pt->stack_depth++;
    pt->stack[i].name = name;
    pt->stack[i].stage = stage;
    pt->stack[i].start_time = perf_now_ms();
    pt->stack[i].start_memory = 0; /* TODO: hook into malloc stats */
    pt->stack[i].items = 0;
}

void perf_set_items(PerfTracer* pt, int count) {
    if (!pt->enabled || pt->stack_depth == 0) return;
    pt->stack[pt->stack_depth - 1].items = count;
}

void perf_end(PerfTracer* pt) {
    if (!pt->enabled || pt->stack_depth == 0) return;
    int i = --pt->stack_depth;
    if (pt->entry_count >= PERF_MAX_ENTRIES) return;

    PerfEntry* e = &pt->entries[pt->entry_count++];
    e->name = pt->stack[i].name;
    e->stage = pt->stack[i].stage;
    e->elapsed_ms = perf_now_ms() - pt->stack[i].start_time;
    e->memory_delta = 0;
    e->depth = i;
    e->items_processed = pt->stack[i].items;
}

void perf_print_report(PerfTracer* pt, FILE* out, bool use_colors) {
    if (pt->entry_count == 0) return;
    const char* bold  = use_colors ? "\x1b[1m" : "";
    const char* dim   = use_colors ? "\x1b[2m" : "";
    const char* cyan  = use_colors ? "\x1b[36m" : "";
    const char* reset = use_colors ? "\x1b[0m" : "";

    fprintf(out, "\n%s──── Performance Report ────%s\n", bold, reset);
    fprintf(out, "%s%-8s  %-28s  %10s  %8s%s\n",
            dim, "Stage", "Pass", "Time (ms)", "Items", reset);
    fprintf(out, "%s%-8s  %-28s  %10s  %8s%s\n",
            dim, "────────", "────────────────────────────",
            "──────────", "────────", reset);

    double total = 0.0;
    for (int i = 0; i < pt->entry_count; i++) {
        PerfEntry* e = &pt->entries[i];
        /* Indent by depth. */
        fprintf(out, "%s%-8s%s  ", cyan,
                diag_stage_name(e->stage), reset);
        for (int d = 0; d < e->depth; d++) fprintf(out, "  ");
        int name_width = 28 - e->depth * 2;
        if (name_width < 10) name_width = 10;
        fprintf(out, "%-*s  %10.3f", name_width, e->name, e->elapsed_ms);
        if (e->items_processed > 0)
            fprintf(out, "  %8d", e->items_processed);
        fprintf(out, "\n");

        if (e->depth == 0) total += e->elapsed_ms;
    }
    fprintf(out, "%s%-8s  %-28s  %10.3f%s\n",
            bold, "", "TOTAL", total, reset);
    fprintf(out, "%s───────────────────────────────────────────────────────%s\n\n",
            bold, reset);
}

void perf_print_json(PerfTracer* pt, FILE* out) {
    fprintf(out, "{\"perf\":[");
    for (int i = 0; i < pt->entry_count; i++) {
        if (i) fputc(',', out);
        PerfEntry* e = &pt->entries[i];
        fprintf(out, "{\"name\":\"%s\",\"stage\":\"%s\","
                     "\"elapsed_ms\":%.3f,\"depth\":%d,\"items\":%d}",
                e->name, diag_stage_name(e->stage),
                e->elapsed_ms, e->depth, e->items_processed);
    }
    fprintf(out, "]}\n");
}
void perf_print_compact(PerfTracer* pt, FILE* out, const char* filename, bool use_colors) {
    if (pt->entry_count == 0) {
        fprintf(out, "  [DEBUG] entry_count is 0\n");
        return;
    }
    const char* bold  = use_colors ? "\x1b[1m" : "";
    const char* dim   = use_colors ? "\x1b[2m" : "";
    const char* cyan  = use_colors ? "\x1b[36m" : "";
    const char* reset = use_colors ? "\x1b[0m" : "";

    /* Header: casprix v1.0.0 · filename */
    fprintf(out, "\n%scasprix v1.0.0%s  %s\xC2\xB7%s  %s%s%s\n\n", 
            bold, reset, dim, reset, bold, filename, reset);

    double total = 0.0;
    for (int i = 0; i < pt->entry_count; i++) {
        PerfEntry* e = &pt->entries[i];
        if (e->depth > 0) continue; /* Only top-level stages */

        const char* stage_name = diag_stage_name(e->stage);
        fprintf(out, "  %s%-8s%s  %6.1fms  ", 
                cyan, stage_name, reset, e->elapsed_ms);

        /* Stage-specific info */
        switch (e->stage) {
            case STAGE_LEX:
                fprintf(out, "tokenized %d tokens", e->items_processed);
                break;
            case STAGE_PARSE:
                fprintf(out, "%d top-level declarations", e->items_processed);
                break;
            case STAGE_SEMA:
                fprintf(out, "type check passed");
                break;
            case STAGE_CODEGEN:
                fprintf(out, "generated assembly");
                break;
            case STAGE_LINK:
                fprintf(out, "\xE2\x9E\x94 binary linked");
                break;
            default:
                if (e->name) fprintf(out, "%s", e->name);
                break;
        }
        fprintf(out, "\n");
        total += e->elapsed_ms;
    }

    fprintf(out, "\n  %sdone in %.1fms%s\n\n", bold, total, reset);
}
