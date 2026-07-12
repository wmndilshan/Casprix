/*
 * Casprix Compiler — Regex → DFA → MIR Lowering
 *
 * See `mir_regex.h` for the end-to-end pipeline and the shape of the
 * emitted MIR function. Implementation overview:
 *
 *   1. parse_pattern        : recursive-descent parser over the regex
 *                             source → Regex AST (nodes allocated from
 *                             a per-compile arena so they live alongside
 *                             the DFA).
 *   2. build_nfa            : Thompson's construction. Each NFA state is
 *                             either a byte-class consumer (one class,
 *                             one next-state) or an epsilon split (up to
 *                             two next-states). One dedicated MATCH state
 *                             terminates every branch.
 *   3. nfa_to_dfa           : classic subset construction. Subsets are
 *                             hashed by their sorted NFA-state-id vector;
 *                             dead / unreachable transitions are marked
 *                             -1 so the MIR emitter knows not to emit a
 *                             switch case.
 *   4. emit_mir             : one block per DFA state (body+end), plus a
 *                             shared reject block. All control flow uses
 *                             only the public MIR builder API so the
 *                             existing optimizer / mem2reg / asmgen work
 *                             unchanged.
 *
 * The arena is owned by the caller's MirModule so every byte we allocate
 * here is freed atomically when the module is destroyed.
 */

#include "mir_regex.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

/* ============================================================
 * Shared helpers
 * ============================================================ */

#define REGEX_MAX_BYTES 256

/* 256-bit byte-set. Bit i set ⇔ byte i is a member. */
typedef struct ByteSet {
    uint64_t w[4];
} ByteSet;

static inline void bs_clear(ByteSet* s) { s->w[0]=s->w[1]=s->w[2]=s->w[3]=0; }
static inline void bs_fill (ByteSet* s) { s->w[0]=s->w[1]=s->w[2]=s->w[3]=~(uint64_t)0; }
static inline void bs_add  (ByteSet* s, int b) {
    s->w[(unsigned)b >> 6] |= ((uint64_t)1 << ((unsigned)b & 63));
}
static inline bool bs_has  (const ByteSet* s, int b) {
    return (s->w[(unsigned)b >> 6] >> ((unsigned)b & 63)) & 1u;
}
static inline void bs_invert(ByteSet* s) {
    s->w[0] = ~s->w[0]; s->w[1] = ~s->w[1];
    s->w[2] = ~s->w[2]; s->w[3] = ~s->w[3];
}
static void bs_add_range(ByteSet* s, int lo, int hi) {
    if (lo < 0)   lo = 0;
    if (hi > 255) hi = 255;
    for (int b = lo; b <= hi; b++) bs_add(s, b);
}

/* ============================================================
 * Regex AST
 * ============================================================ */

typedef enum {
    R_CLASS,   /* consume one byte from ByteSet                 */
    R_CONCAT,  /* l · r                                         */
    R_ALT,     /* l | r                                         */
    R_STAR,    /* l*                                            */
    R_PLUS,    /* l+                                            */
    R_OPT,     /* l?                                            */
    R_EMPTY    /* ε                                             */
} RegexKind;

typedef struct RegexNode {
    RegexKind kind;
    ByteSet   cls;                  /* R_CLASS */
    struct RegexNode* a;            /* lhs / child */
    struct RegexNode* b;            /* rhs        */
} RegexNode;

/* ============================================================
 * Compile context — single arena per compile
 * ============================================================ */

#define RX_ERRBUF 256

typedef struct RxCtx {
    MirArena*   arena;              /* lifetime == module */
    const char* src;
    int         pos;
    int         len;

    const char* error;              /* arena-alloc'd */
    int         error_pos;

    char        errbuf[RX_ERRBUF];  /* staging before arena-dup */
} RxCtx;

static void* rx_alloc(RxCtx* c, size_t n) {
    return mir_arena_alloc(c->arena, n);
}

static char* rx_strdup(RxCtx* c, const char* s) {
    return mir_arena_strdup(c->arena, s);
}

static void rx_seterr(RxCtx* c, int pos, const char* fmt, ...) {
    if (c->error) return;           /* keep first error */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->errbuf, sizeof c->errbuf, fmt, ap);
    va_end(ap);
    c->error     = rx_strdup(c, c->errbuf);
    c->error_pos = pos;
}

static RegexNode* rx_node(RxCtx* c, RegexKind k) {
    RegexNode* n = (RegexNode*)rx_alloc(c, sizeof *n);
    n->kind = k;
    bs_clear(&n->cls);
    n->a = n->b = NULL;
    return n;
}

static RegexNode* rx_class_single(RxCtx* c, int byte) {
    RegexNode* n = rx_node(c, R_CLASS);
    bs_add(&n->cls, byte);
    return n;
}

/* ============================================================
 * Parser
 *
 *     regex   := alt
 *     alt     := concat ('|' concat)*
 *     concat  := atom_q*                    (empty → R_EMPTY)
 *     atom_q  := atom ('*' | '+' | '?')?
 *     atom    := '(' alt ')' | '[' class ']' | '.' | '\\' esc | literal
 *
 * Anchors (^, $) and bounded reps ({n,m}) are rejected with a clear
 * diagnostic rather than silently accepted.
 * ============================================================ */

static RegexNode* parse_alt(RxCtx* c);

static int peek(RxCtx* c) {
    return c->pos < c->len ? (unsigned char)c->src[c->pos] : -1;
}
static int advance(RxCtx* c) {
    return c->pos < c->len ? (unsigned char)c->src[c->pos++] : -1;
}

/* \d \D \w \W \s \S and literal escapes */
static bool parse_escape_into(RxCtx* c, int esc_pos, ByteSet* out) {
    int ch = advance(c);
    if (ch < 0) { rx_seterr(c, esc_pos, "trailing backslash in pattern"); return false; }
    switch (ch) {
        case 'd': for (int b = '0'; b <= '9'; b++) bs_add(out, b); return true;
        case 'D': {
            ByteSet t; bs_clear(&t);
            for (int b = '0'; b <= '9'; b++) bs_add(&t, b);
            bs_invert(&t);
            for (int b = 0; b < 256; b++) if (bs_has(&t, b)) bs_add(out, b);
            return true;
        }
        case 'w': {
            for (int b = '0'; b <= '9'; b++) bs_add(out, b);
            for (int b = 'A'; b <= 'Z'; b++) bs_add(out, b);
            for (int b = 'a'; b <= 'z'; b++) bs_add(out, b);
            bs_add(out, '_');
            return true;
        }
        case 'W': {
            ByteSet t; bs_clear(&t);
            for (int b = '0'; b <= '9'; b++) bs_add(&t, b);
            for (int b = 'A'; b <= 'Z'; b++) bs_add(&t, b);
            for (int b = 'a'; b <= 'z'; b++) bs_add(&t, b);
            bs_add(&t, '_');
            bs_invert(&t);
            for (int b = 0; b < 256; b++) if (bs_has(&t, b)) bs_add(out, b);
            return true;
        }
        case 's': bs_add(out, ' '); bs_add(out, '\t'); bs_add(out, '\n');
                  bs_add(out, '\r'); bs_add(out, '\f'); bs_add(out, '\v');
                  return true;
        case 'S': {
            ByteSet t; bs_clear(&t);
            bs_add(&t, ' '); bs_add(&t, '\t'); bs_add(&t, '\n');
            bs_add(&t, '\r'); bs_add(&t, '\f'); bs_add(&t, '\v');
            bs_invert(&t);
            for (int b = 0; b < 256; b++) if (bs_has(&t, b)) bs_add(out, b);
            return true;
        }
        case 'n': bs_add(out, '\n'); return true;
        case 'r': bs_add(out, '\r'); return true;
        case 't': bs_add(out, '\t'); return true;
        case 'f': bs_add(out, '\f'); return true;
        case 'v': bs_add(out, '\v'); return true;
        case '0': bs_add(out, '\0'); return true;
        default:
            if ((ch >= 0x20 && ch < 0x7f) || ch == '\t') {
                bs_add(out, ch);
                return true;
            }
            rx_seterr(c, esc_pos, "unsupported escape '\\%c'", (char)ch);
            return false;
    }
}

/* Parse the contents of a [...] character class. `[` already consumed. */
static RegexNode* parse_class(RxCtx* c) {
    int start = c->pos - 1;
    RegexNode* n = rx_node(c, R_CLASS);

    bool negate = false;
    if (peek(c) == '^') { negate = true; advance(c); }

    bool first = true;
    while (peek(c) != ']') {
        int here = c->pos;
        int ch   = advance(c);
        if (ch < 0) { rx_seterr(c, start, "unterminated character class"); return NULL; }

        ByteSet elem; bs_clear(&elem);
        if (ch == '\\') {
            if (!parse_escape_into(c, here, &elem)) return NULL;
        } else {
            bs_add(&elem, ch);
        }

        /* Range?  a-z  (ASCII only; no escapes on RHS for simplicity) */
        if (peek(c) == '-' && c->pos + 1 < c->len && c->src[c->pos + 1] != ']') {
            advance(c);                          /* consume '-' */
            int rhs_pos = c->pos;
            int rhs     = advance(c);
            if (rhs == '\\') {
                ByteSet t; bs_clear(&t);
                if (!parse_escape_into(c, rhs_pos, &t)) return NULL;
                /* Ranges against a shorthand make no sense — reject. */
                rx_seterr(c, rhs_pos, "escape shorthand cannot be end of range");
                return NULL;
            }
            /* Find the single byte in `elem` to use as range LHS. */
            int lhs = -1;
            for (int b = 0; b < 256 && lhs < 0; b++) if (bs_has(&elem, b)) lhs = b;
            if (lhs < 0 || rhs < 0) {
                rx_seterr(c, here, "malformed character range");
                return NULL;
            }
            if (lhs > rhs) {
                rx_seterr(c, here, "character range %d-%d is empty", lhs, rhs);
                return NULL;
            }
            bs_clear(&elem);
            bs_add_range(&elem, lhs, rhs);
        }

        for (int b = 0; b < 256; b++) if (bs_has(&elem, b)) bs_add(&n->cls, b);
        first = false;
    }
    (void)first;
    advance(c);                                  /* consume ']' */
    if (negate) bs_invert(&n->cls);
    return n;
}

static RegexNode* parse_atom(RxCtx* c) {
    int here = c->pos;
    int ch = advance(c);
    if (ch < 0) return NULL;

    switch (ch) {
        case '(': {
            RegexNode* inner = parse_alt(c);
            if (!inner) return NULL;
            if (peek(c) != ')') {
                rx_seterr(c, here, "missing ')' in group");
                return NULL;
            }
            advance(c);
            return inner;
        }
        case '[':
            return parse_class(c);
        case '.': {
            RegexNode* n = rx_node(c, R_CLASS);
            bs_fill(&n->cls);
            /* Pragmatic default: `.` matches any byte *including* NUL.
             * This is consistent with treating inputs as arbitrary byte
             * slices rather than C strings. */
            return n;
        }
        case '\\': {
            RegexNode* n = rx_node(c, R_CLASS);
            if (!parse_escape_into(c, here, &n->cls)) return NULL;
            return n;
        }
        case '^': case '$':
            rx_seterr(c, here, "anchors (^/$) are not supported (regex is implicitly full-match)");
            return NULL;
        case '{':
            rx_seterr(c, here, "bounded quantifier {..} not supported — use explicit concatenation");
            return NULL;
        case ')':                                 /* unbalanced */
            rx_seterr(c, here, "unexpected ')'");
            return NULL;
        case '|': case '*': case '+': case '?':
            rx_seterr(c, here, "unexpected metacharacter '%c'", (char)ch);
            return NULL;
        default:
            return rx_class_single(c, ch);
    }
}

static RegexNode* parse_atom_quant(RxCtx* c) {
    RegexNode* atom = parse_atom(c);
    if (!atom) return NULL;

    int q = peek(c);
    if (q == '*' || q == '+' || q == '?') {
        advance(c);
        RegexNode* wrap = rx_node(c, q == '*' ? R_STAR : q == '+' ? R_PLUS : R_OPT);
        wrap->a = atom;
        return wrap;
    }
    return atom;
}

static bool atom_starts_here(int ch) {
    /* Returns true iff `ch` can begin a new atom. Used so `concat` knows
     * when to stop gracefully (at '|', ')', end-of-input). */
    switch (ch) {
        case -1:
        case '|':
        case ')':
            return false;
        default:
            return true;
    }
}

static RegexNode* parse_concat(RxCtx* c) {
    if (!atom_starts_here(peek(c))) return rx_node(c, R_EMPTY);

    RegexNode* lhs = parse_atom_quant(c);
    if (!lhs) return NULL;
    while (atom_starts_here(peek(c))) {
        RegexNode* rhs = parse_atom_quant(c);
        if (!rhs) return NULL;
        RegexNode* cat = rx_node(c, R_CONCAT);
        cat->a = lhs;
        cat->b = rhs;
        lhs = cat;
    }
    return lhs;
}

static RegexNode* parse_alt(RxCtx* c) {
    RegexNode* lhs = parse_concat(c);
    if (!lhs) return NULL;
    while (peek(c) == '|') {
        advance(c);
        RegexNode* rhs = parse_concat(c);
        if (!rhs) return NULL;
        RegexNode* alt = rx_node(c, R_ALT);
        alt->a = lhs;
        alt->b = rhs;
        lhs = alt;
    }
    return lhs;
}

static RegexNode* parse_pattern(RxCtx* c, const char* pattern) {
    c->src = pattern;
    c->pos = 0;
    c->len = (int)strlen(pattern);
    RegexNode* tree = parse_alt(c);
    if (!tree) return NULL;
    if (c->pos != c->len) {
        rx_seterr(c, c->pos, "trailing garbage in pattern at offset %d", c->pos);
        return NULL;
    }
    return tree;
}

/* ============================================================
 * Thompson NFA
 *
 * Each NFA state has one of two shapes:
 *
 *   BYTE   : one outgoing edge consuming any byte in `cls`, to `out1`.
 *   SPLIT  : up to two ε-edges (`out1`, `out2`), either/both may be -1.
 *
 * The "match" state is simply a SPLIT with no outgoing edges (both -1).
 * Thompson's construction is well-formed under this encoding.
 * ============================================================ */

typedef enum { NFA_BYTE, NFA_SPLIT } NfaKind;

typedef struct NfaState {
    NfaKind kind;
    ByteSet cls;            /* BYTE only */
    int     out1, out2;     /* -1 == none */
} NfaState;

typedef struct Frag {
    int entry;              /* NFA state id */
    /* List of state ids whose unresolved outgoing edge should be patched
     * to the next fragment's entry.  Each entry is a tagged id packed as
     * (state_id << 1) | which_slot (0 = out1, 1 = out2).
     */
    int*    patches;
    int     n_patches;
    int     cap_patches;
} Frag;

typedef struct Nfa {
    MirArena*  arena;
    NfaState*  states;
    int        n;
    int        cap;
    int        match_state;
} Nfa;

static int nfa_new(Nfa* n, NfaKind k) {
    if (n->n == n->cap) {
        int new_cap = n->cap ? n->cap * 2 : 16;
        NfaState* ns = (NfaState*)mir_arena_alloc(n->arena, new_cap * sizeof(NfaState));
        if (n->states) memcpy(ns, n->states, n->n * sizeof(NfaState));
        n->states = ns;
        n->cap    = new_cap;
    }
    n->states[n->n].kind = k;
    bs_clear(&n->states[n->n].cls);
    n->states[n->n].out1 = -1;
    n->states[n->n].out2 = -1;
    return n->n++;
}

static void frag_init(Frag* f, int entry) {
    f->entry = entry;
    f->patches = NULL;
    f->n_patches = f->cap_patches = 0;
}

static void frag_add_patch(Frag* f, MirArena* arena, int state_id, int slot) {
    if (f->n_patches == f->cap_patches) {
        int new_cap = f->cap_patches ? f->cap_patches * 2 : 4;
        int* np = (int*)mir_arena_alloc(arena, new_cap * sizeof(int));
        if (f->patches) memcpy(np, f->patches, f->n_patches * sizeof(int));
        f->patches = np;
        f->cap_patches = new_cap;
    }
    f->patches[f->n_patches++] = (state_id << 1) | (slot & 1);
}

static void frag_patch_to(Nfa* n, Frag* f, int target) {
    for (int i = 0; i < f->n_patches; i++) {
        int id   = f->patches[i] >> 1;
        int slot = f->patches[i] & 1;
        if (slot == 0) n->states[id].out1 = target;
        else           n->states[id].out2 = target;
    }
}

static void frag_merge_patches(Frag* dst, MirArena* arena, const Frag* other) {
    for (int i = 0; i < other->n_patches; i++) {
        int id   = other->patches[i] >> 1;
        int slot = other->patches[i] & 1;
        frag_add_patch(dst, arena, id, slot);
    }
}

static Frag build_frag(Nfa* n, RegexNode* r) {
    Frag out; frag_init(&out, -1);
    switch (r->kind) {
        case R_EMPTY: {
            int s = nfa_new(n, NFA_SPLIT);
            out.entry = s;
            frag_add_patch(&out, n->arena, s, 0);
            return out;
        }
        case R_CLASS: {
            int s = nfa_new(n, NFA_BYTE);
            n->states[s].cls = r->cls;
            out.entry = s;
            frag_add_patch(&out, n->arena, s, 0);
            return out;
        }
        case R_CONCAT: {
            Frag l = build_frag(n, r->a);
            Frag rr = build_frag(n, r->b);
            frag_patch_to(n, &l, rr.entry);
            out.entry = l.entry;
            out.patches   = rr.patches;
            out.n_patches = rr.n_patches;
            out.cap_patches = rr.cap_patches;
            return out;
        }
        case R_ALT: {
            int s = nfa_new(n, NFA_SPLIT);
            Frag l = build_frag(n, r->a);
            Frag rr = build_frag(n, r->b);
            n->states[s].out1 = l.entry;
            n->states[s].out2 = rr.entry;
            out.entry = s;
            frag_merge_patches(&out, n->arena, &l);
            frag_merge_patches(&out, n->arena, &rr);
            return out;
        }
        case R_STAR: {
            int s = nfa_new(n, NFA_SPLIT);
            Frag inner = build_frag(n, r->a);
            n->states[s].out1 = inner.entry;
            frag_patch_to(n, &inner, s);
            out.entry = s;
            frag_add_patch(&out, n->arena, s, 1);
            return out;
        }
        case R_PLUS: {
            Frag inner = build_frag(n, r->a);
            int s = nfa_new(n, NFA_SPLIT);
            n->states[s].out1 = inner.entry;
            frag_patch_to(n, &inner, s);
            out.entry = inner.entry;
            frag_add_patch(&out, n->arena, s, 1);
            return out;
        }
        case R_OPT: {
            int s = nfa_new(n, NFA_SPLIT);
            Frag inner = build_frag(n, r->a);
            n->states[s].out1 = inner.entry;
            out.entry = s;
            frag_add_patch(&out, n->arena, s, 1);
            frag_merge_patches(&out, n->arena, &inner);
            return out;
        }
    }
    return out;                                  /* unreachable */
}

static void build_nfa(Nfa* n, MirArena* arena, RegexNode* root) {
    n->arena = arena;
    n->states = NULL; n->n = 0; n->cap = 0;
    Frag top = build_frag(n, root);
    n->match_state = nfa_new(n, NFA_SPLIT);
    frag_patch_to(n, &top, n->match_state);
}

/* ============================================================
 * Subset construction: NFA → DFA
 *
 * A DFA state is a sorted subset of NFA state ids. We hash subsets into
 * an open-addressing table keyed by FNV-1a of the id vector so the
 * construction is O((#DFA states) · 256 · avg-subset-size).
 * ============================================================ */

typedef struct DfaState {
    bool     accepting;
    int      ntrans;                             /* count of non-dead cases */
    int32_t  trans[REGEX_MAX_BYTES];             /* -1 = reject/dead */
    /* For subset-table key lookup: */
    int*     set;                                /* sorted NFA ids */
    int      set_len;
    uint64_t hash;
} DfaState;

typedef struct Dfa {
    MirArena*   arena;
    DfaState*   states;
    int         n;
    int         cap;

    /* open-addressing table: index into `states` or -1 */
    int32_t*    bucket;
    int         bucket_mask;
    int         bucket_used;
} Dfa;

static uint64_t fnv64_ids(const int* ids, int n) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) {
        uint32_t v = (uint32_t)ids[i];
        for (int b = 0; b < 4; b++) {
            h ^= (uint64_t)(v & 0xff);
            h *= 1099511628211ULL;
            v >>= 8;
        }
    }
    return h;
}

static void dfa_rehash(Dfa* d, int new_cap) {
    int32_t* nb = (int32_t*)mir_arena_alloc(d->arena, new_cap * sizeof(int32_t));
    for (int i = 0; i < new_cap; i++) nb[i] = -1;
    int mask = new_cap - 1;
    for (int i = 0; i < d->n; i++) {
        uint64_t h = d->states[i].hash;
        int k = (int)(h & (uint64_t)mask);
        while (nb[k] != -1) k = (k + 1) & mask;
        nb[k] = i;
    }
    d->bucket = nb;
    d->bucket_mask = mask;
}

static int dfa_find_or_add(Dfa* d, int* set, int set_len, bool accepting, bool* added) {
    if (d->bucket_used * 2 >= (d->bucket_mask + 1)) {
        dfa_rehash(d, (d->bucket_mask + 1) * 2);
    }
    uint64_t h = fnv64_ids(set, set_len);
    int mask = d->bucket_mask;
    int k = (int)(h & (uint64_t)mask);
    while (d->bucket[k] != -1) {
        int idx = d->bucket[k];
        if (d->states[idx].hash == h &&
            d->states[idx].set_len == set_len &&
            memcmp(d->states[idx].set, set, set_len * sizeof(int)) == 0) {
            *added = false;
            return idx;
        }
        k = (k + 1) & mask;
    }
    if (d->n == d->cap) {
        int new_cap = d->cap ? d->cap * 2 : 32;
        DfaState* ns = (DfaState*)mir_arena_alloc(d->arena, new_cap * sizeof(DfaState));
        if (d->states) memcpy(ns, d->states, d->n * sizeof(DfaState));
        d->states = ns;
        d->cap = new_cap;
    }
    int idx = d->n++;
    DfaState* s = &d->states[idx];
    s->accepting = accepting;
    s->ntrans    = 0;
    for (int i = 0; i < REGEX_MAX_BYTES; i++) s->trans[i] = -1;
    /* Copy the id vector into the arena so later comparisons don't alias
     * scratch buffers that the caller will reuse. */
    s->set     = (int*)mir_arena_alloc(d->arena, set_len * sizeof(int));
    memcpy(s->set, set, set_len * sizeof(int));
    s->set_len = set_len;
    s->hash    = h;
    d->bucket[k] = idx;
    d->bucket_used++;
    *added = true;
    return idx;
}

/* sorted-insert `x` into `set[0..*n]` with dedup. */
static void set_sorted_add(int* set, int* n, int x) {
    int lo = 0, hi = *n;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (set[mid] < x) lo = mid + 1;
        else              hi = mid;
    }
    if (lo < *n && set[lo] == x) return;
    for (int i = *n; i > lo; i--) set[i] = set[i-1];
    set[lo] = x;
    (*n)++;
}

/* Epsilon closure: for a starting multiset, collect all states reachable
 * via zero or more ε transitions. */
static void eclose(const Nfa* nfa, int* stk, int* stk_top,
                   int* set, int* n, int start_id) {
    if (start_id < 0) return;
    /* Linear `contains` is fine since NFA states per closure are small. */
    for (int i = 0; i < *n; i++) if (set[i] == start_id) return;
    set_sorted_add(set, n, start_id);
    stk[(*stk_top)++] = start_id;
    while (*stk_top > 0) {
        int cur = stk[--(*stk_top)];
        const NfaState* s = &nfa->states[cur];
        if (s->kind == NFA_SPLIT) {
            int o1 = s->out1, o2 = s->out2;
            if (o1 >= 0) {
                bool seen = false;
                for (int i = 0; i < *n; i++) if (set[i] == o1) { seen = true; break; }
                if (!seen) { set_sorted_add(set, n, o1); stk[(*stk_top)++] = o1; }
            }
            if (o2 >= 0) {
                bool seen = false;
                for (int i = 0; i < *n; i++) if (set[i] == o2) { seen = true; break; }
                if (!seen) { set_sorted_add(set, n, o2); stk[(*stk_top)++] = o2; }
            }
        }
    }
}

static bool set_contains(const int* set, int n, int id) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (set[mid] < id)      lo = mid + 1;
        else if (set[mid] > id) hi = mid;
        else                    return true;
    }
    return false;
}

static void nfa_to_dfa(const Nfa* nfa, Dfa* dfa, MirArena* arena) {
    dfa->arena = arena;
    dfa->states = NULL; dfa->n = 0; dfa->cap = 0;
    dfa->bucket = NULL; dfa->bucket_mask = 0; dfa->bucket_used = 0;
    dfa_rehash(dfa, 32);

    /* Scratch buffers — one per DFA-state-worth of work. */
    int  nfa_n = nfa->n;
    int* stk   = (int*)malloc(nfa_n * sizeof(int));
    int  stk_top = 0;

    int* start_set = (int*)malloc(nfa_n * sizeof(int));
    int  start_len = 0;
    eclose(nfa, stk, &stk_top, start_set, &start_len, 0);
    bool start_accept = set_contains(start_set, start_len, nfa->match_state);

    bool added;
    int start_id = dfa_find_or_add(dfa, start_set, start_len, start_accept, &added);
    (void)start_id;
    free(start_set);

    /* Worklist is implicit: any DFA state whose trans[*] haven't been
     * computed yet. `processed` marks those already expanded. */
    int   processed = 0;
    int*  nxt_set   = (int*)malloc(nfa_n * sizeof(int));

    while (processed < dfa->n) {
        int cur = processed++;
        const DfaState* csnap = &dfa->states[cur];
        /* Snapshot the id-set pointer and length because `dfa->states`
         * may be reallocated by dfa_find_or_add below, invalidating
         * `csnap`. The underlying int* storage is in the arena, so
         * reading `cur_set`/`cur_set_len` remains valid. */
        int cur_set_len = csnap->set_len;
        const int* cur_set = csnap->set;

        for (int b = 0; b < 256; b++) {
            int nxt_len = 0;
            stk_top = 0;
            for (int i = 0; i < cur_set_len; i++) {
                const NfaState* s = &nfa->states[cur_set[i]];
                if (s->kind == NFA_BYTE && bs_has(&s->cls, b) && s->out1 >= 0) {
                    eclose(nfa, stk, &stk_top, nxt_set, &nxt_len, s->out1);
                }
            }
            if (nxt_len == 0) {
                /* dead transition — leave trans[b] == -1 */
                continue;
            }
            bool accept = set_contains(nxt_set, nxt_len, nfa->match_state);
            int nxt_id = dfa_find_or_add(dfa, nxt_set, nxt_len, accept, &added);
            dfa->states[cur].trans[b] = (int32_t)nxt_id;
            dfa->states[cur].ntrans++;
        }
    }

    free(nxt_set);
    free(stk);
}

/* ============================================================
 * Reference matcher (exposed for testing)
 * ============================================================ */

bool mir_regex_match_reference(const char* pattern,
                               const uint8_t* input,
                               size_t len,
                               const char** error_out) {
    MirArena* arena = mir_arena_create();
    RxCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.arena = arena;
    ctx.error_pos = -1;

    RegexNode* tree = parse_pattern(&ctx, pattern);
    if (!tree) {
        if (error_out) *error_out = ctx.error ? strdup(ctx.error) : NULL;
        mir_arena_destroy(arena);
        return false;
    }

    Nfa nfa;
    build_nfa(&nfa, arena, tree);

    Dfa dfa;
    nfa_to_dfa(&nfa, &dfa, arena);

    int state = 0;                               /* start is always 0 */
    for (size_t i = 0; i < len; i++) {
        int32_t nxt = dfa.states[state].trans[input[i]];
        if (nxt < 0) { mir_arena_destroy(arena); if (error_out) *error_out = NULL; return false; }
        state = (int)nxt;
    }
    bool ok = dfa.states[state].accepting;
    mir_arena_destroy(arena);
    if (error_out) *error_out = NULL;
    return ok;
}

/* ============================================================
 * MIR emission
 * ============================================================ */

/*
 * Build a MIR_SWITCH instruction manually: the public builder API does
 * not expose one yet (SWITCH is otherwise only synthesised by the match
 * lowering pass). We replicate what `mir_builder.c::emit_void` does
 * inline so we stay strictly within published MIR invariants.
 */
static void emit_switch(MirBuilder* b,
                        MirValueId  disc,
                        int64_t*    case_values,
                        MirBlock**  case_targets,
                        int         n_cases,
                        MirBlock*   default_bb) {
    MirArena* arena = b->module->arena;
    MirInst* inst = (MirInst*)mir_arena_alloc(arena, sizeof(MirInst));
    inst->opcode = MIR_SWITCH;
    inst->type   = mir_type_void(b->module);
    inst->result = MIR_VALUE_NONE;
    inst->src_line = 0;
    inst->src_col  = 0;
    inst->next = inst->prev = NULL;

    inst->as.sw.discriminant = disc;
    inst->as.sw.n_cases      = n_cases;
    inst->as.sw.default_bb   = default_bb;

    if (n_cases > 0) {
        inst->as.sw.case_values =
            (int64_t*)mir_arena_alloc(arena, n_cases * sizeof(int64_t));
        inst->as.sw.targets =
            (MirBlock**)mir_arena_alloc(arena, n_cases * sizeof(MirBlock*));
        memcpy(inst->as.sw.case_values, case_values, n_cases * sizeof(int64_t));
        memcpy(inst->as.sw.targets,     case_targets, n_cases * sizeof(MirBlock*));
    } else {
        inst->as.sw.case_values = NULL;
        inst->as.sw.targets     = NULL;
    }

    mir_block_append(b->current_block, inst);

    /* Wire CFG edges: one successor per unique case target + default.
     * `mir_block_add_*` tolerates duplicates fine (it just grows the
     * adjacency arrays), and duplicate edges are semantically correct
     * for the downstream passes that walk successors (they compute
     * dominance/frontier in terms of unique successor blocks via sets
     * anyway). */
    for (int i = 0; i < n_cases; i++) {
        mir_block_add_successor(b->current_block, case_targets[i]);
        mir_block_add_predecessor(case_targets[i], b->current_block);
    }
    mir_block_add_successor(b->current_block, default_bb);
    mir_block_add_predecessor(default_bb, b->current_block);
}

static MirFunction* emit_mir(MirModule* module, const char* func_name, const Dfa* dfa) {
    /* Subset construction always produces at least the start state. */
    assert(dfa->n >= 1);

    /* Signature:  i32 @func(ptr<u8>, i64)   — returns 1 on match, 0 on fail. */
    MirType* i32t = mir_type_i32(module);
    MirType* i64t = mir_type_i64(module);
    MirType* u8t  = mir_type_u8(module);
    MirType* ptr_u8 = mir_type_ptr(module, u8t);

    MirParam params[2];
    params[0].name = "input"; params[0].type = ptr_u8; params[0].value_id = 0;
    params[1].name = "len";   params[1].type = i64t;   params[1].value_id = 0;

    MirFunction* func = mir_module_add_function(module, func_name, i32t, params, 2);
    MirValueId   input_id = func->params[0].value_id;
    MirValueId   len_id   = func->params[1].value_id;

    MirBuilder b; mir_builder_init(&b, module, func);

    /* Pre-allocate the block set so per-state wiring can reference
     * forward targets. We have for each state S two blocks (head +
     * body) plus one shared reject block. */
    MirBlock* entry = mir_function_add_block(func, "entry");
    MirBlock* reject = mir_function_add_block(func, "reject");

    /* calloc so gcc's -Wmaybe-uninitialized is happy with the later
     * `head_bb[0]` read: it can't correlate the `dfa->n >= 1` assert
     * with the upcoming for-loop write across inlining boundaries. */
    MirBlock** head_bb = (MirBlock**)calloc((size_t)dfa->n, sizeof(MirBlock*));
    MirBlock** body_bb = (MirBlock**)calloc((size_t)dfa->n, sizeof(MirBlock*));
    MirBlock** end_bb  = (MirBlock**)calloc((size_t)dfa->n, sizeof(MirBlock*));

    char name_buf[64];
    for (int s = 0; s < dfa->n; s++) {
        snprintf(name_buf, sizeof name_buf, "state_%d", s);
        head_bb[s] = mir_function_add_block(func, name_buf);
        snprintf(name_buf, sizeof name_buf, "body_%d", s);
        body_bb[s] = mir_function_add_block(func, name_buf);
        snprintf(name_buf, sizeof name_buf, "end_%d", s);
        end_bb[s]  = mir_function_add_block(func, name_buf);
    }

    /* -------------------- entry -------------------- */
    mir_builder_set_block(&b, entry);
    MirValueId pos_slot = mir_build_alloca(&b, i64t);
    MirValueId zero_i64 = mir_build_const_int(&b, 0, i64t);
    mir_build_store(&b, pos_slot, zero_i64);
    mir_build_br(&b, head_bb[0]);

    /* -------------------- reject -------------------- */
    mir_builder_set_block(&b, reject);
    MirValueId zero_i32 = mir_build_const_int(&b, 0, i32t);
    mir_build_ret(&b, zero_i32);

    /* -------------------- per-state blocks -------------------- */
    for (int s = 0; s < dfa->n; s++) {
        const DfaState* st = &dfa->states[s];

        /* head_s: check end-of-input */
        mir_builder_set_block(&b, head_bb[s]);
        MirValueId pos = mir_build_load(&b, pos_slot, i64t);
        MirValueId at_end = mir_build_cmp_ge(&b, pos, len_id);
        mir_build_condbr(&b, at_end, end_bb[s], body_bb[s]);

        /* body_s: read byte, advance, dispatch */
        mir_builder_set_block(&b, body_bb[s]);
        MirValueId pos2   = mir_build_load(&b, pos_slot, i64t);
        MirValueId bp     = mir_build_get_elem_ptr(&b, input_id, pos2);
        MirValueId ch_u8  = mir_build_load(&b, bp, u8t);
        MirValueId ch_i64 = mir_build_zext(&b, ch_u8, i64t);
        MirValueId one    = mir_build_const_int(&b, 1, i64t);
        MirValueId pos3   = mir_build_add(&b, pos2, one);
        mir_build_store(&b, pos_slot, pos3);

        /* Collect non-dead transitions. Cases are byte values 0..255;
         * targets map to the corresponding state_T head block. */
        int n_cases = st->ntrans;
        int64_t*   cvals   = (int64_t*)  malloc(n_cases * sizeof(int64_t));
        MirBlock** ctgts   = (MirBlock**)malloc(n_cases * sizeof(MirBlock*));
        int k = 0;
        for (int bv = 0; bv < 256; bv++) {
            int32_t t = st->trans[bv];
            if (t < 0) continue;
            cvals[k] = bv;
            ctgts[k] = head_bb[t];
            k++;
        }
        if (n_cases == 0) {
            /* dead state: every byte rejects */
            mir_build_br(&b, reject);
        } else {
            emit_switch(&b, ch_i64, cvals, ctgts, n_cases, reject);
        }
        free(cvals);
        free(ctgts);

        /* end_s: return 1 if accepting else 0 */
        mir_builder_set_block(&b, end_bb[s]);
        MirValueId v = mir_build_const_int(&b, st->accepting ? 1 : 0, i32t);
        mir_build_ret(&b, v);
    }

    free(head_bb); free(body_bb); free(end_bb);
    return func;
}

/* ============================================================
 * Public entry point
 * ============================================================ */

MirRegexResult mir_regex_compile(MirModule*  module,
                                 const char* func_name,
                                 const char* pattern) {
    MirRegexResult r;
    memset(&r, 0, sizeof r);
    r.error_pos = -1;

    if (!module || !func_name || !pattern) {
        r.error = "null argument to mir_regex_compile";
        return r;
    }

    RxCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.arena     = module->arena;
    ctx.error_pos = -1;

    RegexNode* tree = parse_pattern(&ctx, pattern);
    if (!tree) {
        r.error     = ctx.error;
        r.error_pos = ctx.error_pos;
        return r;
    }

    Nfa nfa;
    build_nfa(&nfa, module->arena, tree);
    r.nfa_states = nfa.n;

    Dfa dfa;
    nfa_to_dfa(&nfa, &dfa, module->arena);
    r.dfa_states = dfa.n;

    int n_accept = 0;
    for (int i = 0; i < dfa.n; i++) if (dfa.states[i].accepting) n_accept++;
    r.accepting_states = n_accept;

    r.func = emit_mir(module, func_name, &dfa);
    r.ok   = (r.func != NULL);
    return r;
}
