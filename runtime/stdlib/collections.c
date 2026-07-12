/*
 * Casprix Runtime - Collections (pure-C implementation)
 * Implements: NuwanList, NuwanMap, NuwanStack, NuwanQueue, NuwanPQ
 * Also provides the ASM-kernel stubs (sum, sort-net4) in portable C.
 */

#include "../../include/casprix/collections.h"
#include "../../src/support/arena.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <immintrin.h>

/* nuwan_string_hash is defined in string_ops.c */
extern uint64_t nuwan_string_hash(const char* s);

/* ── ASM kernel stubs ───────────────────────────────────────────────────── */

void nuwan_array_sort_net4(int64_t* a) {
    /* sorting network for exactly 4 elements */
#define SWAP4(i,j) do { if(a[i]>a[j]){int64_t t=a[i];a[i]=a[j];a[j]=t;} } while(0)
    SWAP4(0,1); SWAP4(2,3); SWAP4(0,2); SWAP4(1,3); SWAP4(1,2);
#undef SWAP4
}

int64_t nuwan_array_sum_i64(const int64_t* arr, size_t n) {
    if (!arr || n == 0) return 0;
    size_t i = 0;
    int64_t s = 0;

#if defined(__AVX2__)
    __m256i sum_v = _mm256_setzero_si256();
    for (; i + 4 <= n; i += 4) {
        __m256i v = _mm256_loadu_si256((const __m256i*)&arr[i]);
        sum_v = _mm256_add_epi64(sum_v, v);
    }
    int64_t temp[4];
    _mm256_storeu_si256((__m256i*)temp, sum_v);
    s = temp[0] + temp[1] + temp[2] + temp[3];
#elif defined(__SSE2__)
    __m128i sum_v = _mm_setzero_si128();
    for (; i + 2 <= n; i += 2) {
        __m128i v = _mm_loadu_si128((const __m128i*)&arr[i]);
        sum_v = _mm_add_epi64(sum_v, v);
    }
    int64_t temp[2];
    _mm_storeu_si128((__m128i*)temp, sum_v);
    s = temp[0] + temp[1];
#endif

    for (; i < n; i++) s += arr[i];
    return s;
}

/* nuwan_strlen_asm / memcopy / memset_fast — defined in string_ops.c */

/* ── NuwanList ──────────────────────────────────────────────────────────── */

#define LIST_INIT_CAP 16

struct NuwanList {
    Arena*   arena;
    int64_t* data;
    size_t   size;
    size_t   cap;
};

NuwanList* nuwan_list_new(Arena* a) {
    NuwanList* l = (NuwanList*)arena_alloc(a, sizeof(NuwanList));
    if (!l) return NULL;
    l->arena = a;
    l->size  = 0;
    l->cap   = LIST_INIT_CAP;
    /* Aligned to 64 bytes for optimal cache line utilization */
    l->data  = (int64_t*)arena_alloc_aligned(a, l->cap * sizeof(int64_t), 64);
    return l;
}

bool nuwan_list_reserve(NuwanList* l, size_t cap) {
    if (!l || cap <= l->cap) return true;
    int64_t* nd = (int64_t*)arena_alloc_aligned(l->arena, cap * sizeof(int64_t), 64);
    if (!nd) return false;
    if (l->size) memcpy(nd, l->data, l->size * sizeof(int64_t));
    l->data = nd;
    l->cap = cap;
    return true;
}

bool nuwan_list_push(NuwanList* l, int64_t v) {
    if (!l) return false;
    if (l->size == l->cap) {
        if (!nuwan_list_reserve(l, l->cap * 2)) return false;
    }
    l->data[l->size++] = v;
    return true;
}

int64_t nuwan_list_get(const NuwanList* l, size_t idx) {
    if (!l || idx >= l->size) return 0;
    return l->data[idx];
}

bool nuwan_list_set(NuwanList* l, size_t idx, int64_t v) {
    if (!l || idx >= l->size) return false;
    l->data[idx] = v;
    return true;
}

int64_t nuwan_list_pop(NuwanList* l) {
    if (!l || l->size == 0) return 0;
    return l->data[--l->size];
}

bool nuwan_list_remove(NuwanList* l, size_t idx) {
    if (!l || idx >= l->size) return false;
    memmove(l->data + idx, l->data + idx + 1, (l->size - idx - 1) * sizeof(int64_t));
    l->size--;
    return true;
}

bool nuwan_list_insert(NuwanList* l, size_t idx, int64_t v) {
    if (!l || idx > l->size) return false;
    if (!nuwan_list_push(l, 0)) return false;  /* grow */
    memmove(l->data + idx + 1, l->data + idx, (l->size - idx - 1) * sizeof(int64_t));
    l->data[idx] = v;
    return true;
}

size_t nuwan_list_size(const NuwanList* l)  { return l ? l->size : 0; }
bool   nuwan_list_empty(const NuwanList* l) { return !l || l->size == 0; }
void   nuwan_list_clear(NuwanList* l)       { if (l) l->size = 0; }

static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

void nuwan_list_sort(NuwanList* l) {
    if (l && l->size) qsort(l->data, l->size, sizeof(int64_t), cmp_i64);
}

int64_t nuwan_list_binary_search(const NuwanList* l, int64_t v) {
    if (!l || !l->size) return -1;
    size_t lo = 0, hi = l->size;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (l->data[mid] == v) return (int64_t)mid;
        if (l->data[mid] < v) lo = mid + 1; else hi = mid;
    }
    return -1;
}

int64_t nuwan_list_sum(const NuwanList* l) {
    return l ? nuwan_array_sum_i64(l->data, l->size) : 0;
}

void nuwan_list_free(NuwanList* l) {
    /* No-op in Arena-managed runtime. */
    (void)l;
}

/* ── NuwanMap (Robin Hood open-addressing, string key → int64) ─────────── */

#define LIST_INIT_CAP 16
#define MAP_INIT_CAP  16
#define MAP_LOAD_NUM 7
#define MAP_LOAD_DEN 8   /* load factor 0.875 - Robin Hood handles this well */
#define MAP_EMPTY    UINT64_MAX

typedef struct {
    char*    key;
    int64_t  val;
    uint64_t hash;
    uint32_t dib; /* Distance from ideal bucket */
} MapEntry;

struct NuwanMap {
    Arena*    arena;
    MapEntry* slots;
    size_t    size;
    size_t    cap;
};

NuwanMap* nuwan_map_new(Arena* a) {
    NuwanMap* m = (NuwanMap*)arena_calloc(a, 1, sizeof(NuwanMap));
    if (!m) return NULL;
    m->arena = a;
    m->cap   = MAP_INIT_CAP;
    m->slots = (MapEntry*)arena_alloc_aligned(a, m->cap * sizeof(MapEntry), 64);
    if (!m->slots) return NULL;
    for (size_t i = 0; i < m->cap; i++) m->slots[i].hash = MAP_EMPTY;
    return m;
}

static bool map_insert_raw(MapEntry* slots, size_t cap, char* key, int64_t val, uint64_t h) {
    size_t idx = h % cap;
    uint32_t cdib = 0;
    while (1) {
        if (slots[idx].hash == MAP_EMPTY) {
            slots[idx] = (MapEntry){key, val, h, cdib};
            return true;
        }
        if (slots[idx].hash == h && strcmp(slots[idx].key, key) == 0) {
            /* duplicate key — update value. Note: key was strdup'd by caller */
            slots[idx].val = val;
            return false;
        }
        
        /* Robin Hood: if current slot is "richer" (smaller DIB) than us, swap. */
        if (slots[idx].dib < cdib) {
            MapEntry tmp = slots[idx];
            slots[idx] = (MapEntry){key, val, h, cdib};
            key = tmp.key;
            val = tmp.val;
            h = tmp.hash;
            cdib = tmp.dib;
        }
        
        idx = (idx + 1) % cap;
        cdib++;
    }
}

static void map_grow(NuwanMap* m) {
    size_t nc = m->cap * 2;
    MapEntry* ns = (MapEntry*)arena_alloc_aligned(m->arena, nc * sizeof(MapEntry), 64);
    if (!ns) return;
    for (size_t i = 0; i < nc; i++) ns[i].hash = MAP_EMPTY;
    for (size_t i = 0; i < m->cap; i++) {
        if (m->slots[i].hash != MAP_EMPTY)
            map_insert_raw(ns, nc, m->slots[i].key, m->slots[i].val, m->slots[i].hash);
    }
    m->slots = ns;
    m->cap   = nc;
}

void nuwan_map_reserve(NuwanMap* m, size_t expected_size) {
    if (!m) return;
    size_t target_cap = MAP_INIT_CAP;
    while (expected_size * MAP_LOAD_DEN >= target_cap * MAP_LOAD_NUM) target_cap <<= 1;
    if (target_cap > m->cap) {
        /* This is slightly wasteful in an Arena, but necessary if not pre-sized. */
        MapEntry* ns = (MapEntry*)arena_alloc_aligned(m->arena, target_cap * sizeof(MapEntry), 64);
        if (!ns) return;
        for (size_t i = 0; i < target_cap; i++) ns[i].hash = MAP_EMPTY;
        for (size_t i = 0; i < m->cap; i++) {
            if (m->slots[i].hash != MAP_EMPTY)
                map_insert_raw(ns, target_cap, m->slots[i].key, m->slots[i].val, m->slots[i].hash);
        }
        m->slots = ns;
        m->cap = target_cap;
    }
}

void nuwan_map_put(NuwanMap* m, const char* key, int64_t val) {
    if (!m || !key) return;
    if (m->size * MAP_LOAD_DEN >= m->cap * MAP_LOAD_NUM) map_grow(m);
    uint64_t h = nuwan_string_hash(key);
    /* We must copy the key into the arena */
    char* ak = arena_strdup(m->arena, key);
    if (map_insert_raw(m->slots, m->cap, ak, val, h)) {
        m->size++;
    }
}

int64_t nuwan_map_get(const NuwanMap* m, const char* key) {
    if (!m || !key) return 0;
    uint64_t h = nuwan_string_hash(key);
    size_t idx = h % m->cap;
    while (m->slots[idx].hash != MAP_EMPTY) {
        if (m->slots[idx].hash == h && strcmp(m->slots[idx].key, key) == 0)
            return m->slots[idx].val;
        idx = (idx + 1) % m->cap;
    }
    return 0;
}

bool nuwan_map_has(const NuwanMap* m, const char* key) {
    if (!m || !key) return false;
    uint64_t h = nuwan_string_hash(key);
    size_t idx = h % m->cap;
    while (m->slots[idx].hash != MAP_EMPTY) {
        if (m->slots[idx].hash == h && strcmp(m->slots[idx].key, key) == 0)
            return true;
        idx = (idx + 1) % m->cap;
    }
    return false;
}

bool nuwan_map_remove(NuwanMap* m, const char* key) {
    if (!m || !key) return false;
    uint64_t h = nuwan_string_hash(key);
    size_t idx = h % m->cap;
    while (m->slots[idx].hash != MAP_EMPTY) {
        if (m->slots[idx].hash == h && strcmp(m->slots[idx].key, key) == 0) {
            free(m->slots[idx].key);
            m->slots[idx].hash = MAP_EMPTY;
            m->slots[idx].key  = NULL;
            m->size--;
            return true;
        }
        idx = (idx + 1) % m->cap;
    }
    return false;
}

size_t nuwan_map_size(const NuwanMap* m)  { return m ? m->size : 0; }
bool   nuwan_map_empty(const NuwanMap* m) { return !m || m->size == 0; }

void nuwan_map_clear(NuwanMap* m) {
    if (!m) return;
    /* In an Arena, we don't free individual keys. Just mark everything empty. */
    for (size_t i = 0; i < m->cap; i++) {
        m->slots[i].hash = MAP_EMPTY;
        m->slots[i].key = NULL;
    }
    m->size = 0;
}

void nuwan_map_free(NuwanMap* m) {
    (void)m;
}

/* ── NuwanStack (alias over NuwanList) ─────────────────────────────────── */

NuwanStack* nuwan_stack_new(Arena* a)                    { return nuwan_list_new(a); }
bool        nuwan_stack_push(NuwanStack* s, int64_t v) { return nuwan_list_push(s, v); }
int64_t     nuwan_stack_pop(NuwanStack* s)           { return nuwan_list_pop(s); }
int64_t     nuwan_stack_peek(const NuwanStack* s)    { return (s && s->size) ? s->data[s->size-1] : 0; }
size_t      nuwan_stack_size(const NuwanStack* s)    { return nuwan_list_size(s); }
bool        nuwan_stack_empty(const NuwanStack* s)   { return nuwan_list_empty(s); }
void        nuwan_stack_clear(NuwanStack* s)         { nuwan_list_clear(s); }
void        nuwan_stack_free(NuwanStack* s)          { nuwan_list_free(s); }

/* ── NuwanQueue (ring-buffer FIFO) ─────────────────────────────────────── */

#define QUEUE_INIT_CAP 16

struct NuwanQueue {
    Arena*   arena;
    int64_t* data;
    size_t   head, tail, size, cap;
};

NuwanQueue* nuwan_queue_new(Arena* a) {
    NuwanQueue* q = (NuwanQueue*)arena_calloc(a, 1, sizeof(NuwanQueue));
    if (!q) return NULL;
    q->arena = a;
    q->data  = (int64_t*)arena_alloc_aligned(a, QUEUE_INIT_CAP * sizeof(int64_t), 64);
    if (!q->data) return NULL;
    q->cap   = QUEUE_INIT_CAP;
    return q;
}

bool nuwan_queue_enqueue(NuwanQueue* q, int64_t v) {
    if (!q) return false;
    if (q->size == q->cap) {
        size_t nc = q->cap * 2;
        int64_t* nd = (int64_t*)arena_alloc_aligned(q->arena, nc * sizeof(int64_t), 64);
        if (!nd) return false;
        /* linearise ring */
        for (size_t i = 0; i < q->size; i++) nd[i] = q->data[(q->head + i) % q->cap];
        q->data = nd; q->head = 0; q->tail = q->size; q->cap = nc;
    }
    q->data[q->tail] = v;
    q->tail = (q->tail + 1) % q->cap;
    q->size++;
    return true;
}

int64_t nuwan_queue_dequeue(NuwanQueue* q) {
    if (!q || !q->size) return 0;
    int64_t v = q->data[q->head];
    q->head = (q->head + 1) % q->cap;
    q->size--;
    return v;
}

int64_t nuwan_queue_peek(const NuwanQueue* q) {
    return (q && q->size) ? q->data[q->head] : 0;
}

size_t nuwan_queue_size(const NuwanQueue* q)  { return q ? q->size : 0; }
bool   nuwan_queue_empty(const NuwanQueue* q) { return !q || q->size == 0; }

void nuwan_queue_clear(NuwanQueue* q) {
    if (q) { q->head = q->tail = q->size = 0; }
}

void nuwan_queue_free(NuwanQueue* q) {
    (void)q;
}

/* ── NuwanPQ (binary min-heap) ──────────────────────────────────────────── */

#define PQ_INIT_CAP 16

struct NuwanPQ {
    Arena*   arena;
    int64_t* prios;
    int64_t* vals;
    size_t   size, cap;
};

NuwanPQ* nuwan_pq_new(Arena* a) {
    NuwanPQ* pq = (NuwanPQ*)arena_calloc(a, 1, sizeof(NuwanPQ));
    if (!pq) return NULL;
    pq->arena = a;
    pq->cap   = PQ_INIT_CAP;
    pq->prios = (int64_t*)arena_alloc_aligned(a, pq->cap * sizeof(int64_t), 64);
    pq->vals  = (int64_t*)arena_alloc_aligned(a, pq->cap * sizeof(int64_t), 64);
    if (!pq->prios || !pq->vals) return NULL;
    return pq;
}

bool nuwan_pq_reserve(NuwanPQ* pq, size_t cap) {
    if (!pq || cap <= pq->cap) return true;
    int64_t* np = (int64_t*)arena_alloc_aligned(pq->arena, cap * sizeof(int64_t), 64);
    int64_t* nv = (int64_t*)arena_alloc_aligned(pq->arena, cap * sizeof(int64_t), 64);
    if (!np || !nv) return false;
    if (pq->size) {
        memcpy(np, pq->prios, pq->size * sizeof(int64_t));
        memcpy(nv, pq->vals,  pq->size * sizeof(int64_t));
    }
    pq->prios = np;
    pq->vals  = nv;
    pq->cap   = cap;
    return true;
}

static void pq_swap(NuwanPQ* pq, size_t a, size_t b) {
    int64_t tp = pq->prios[a]; pq->prios[a] = pq->prios[b]; pq->prios[b] = tp;
    int64_t tv = pq->vals[a];  pq->vals[a]  = pq->vals[b];  pq->vals[b]  = tv;
}

bool nuwan_pq_push(NuwanPQ* pq, int64_t priority, int64_t value) {
    if (!pq) return false;
    if (pq->size == pq->cap) {
        if (!nuwan_pq_reserve(pq, pq->cap * 2)) return false;
    }
    size_t i = pq->size++;
    pq->prios[i] = priority; pq->vals[i] = value;
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (pq->prios[parent] <= pq->prios[i]) break;
        pq_swap(pq, parent, i);
        i = parent;
    }
    return true;
}

int64_t nuwan_pq_pop(NuwanPQ* pq) {
    if (!pq || !pq->size) return 0;
    int64_t v = pq->vals[0];
    pq->prios[0] = pq->prios[--pq->size];
    pq->vals[0]  = pq->vals[pq->size];
    size_t i = 0;
    while (1) {
        size_t l = 2*i+1, r = 2*i+2, smallest = i;
        if (l < pq->size && pq->prios[l] < pq->prios[smallest]) smallest = l;
        if (r < pq->size && pq->prios[r] < pq->prios[smallest]) smallest = r;
        if (smallest == i) break;
        pq_swap(pq, i, smallest);
        i = smallest;
    }
    return v;
}

int64_t nuwan_pq_peek(const NuwanPQ* pq)          { return (pq && pq->size) ? pq->vals[0]  : 0; }
int64_t nuwan_pq_peek_priority(const NuwanPQ* pq)  { return (pq && pq->size) ? pq->prios[0] : 0; }
size_t  nuwan_pq_size(const NuwanPQ* pq)           { return pq ? pq->size : 0; }
bool    nuwan_pq_empty(const NuwanPQ* pq)          { return !pq || pq->size == 0; }

void nuwan_pq_free(NuwanPQ* pq) {
    (void)pq;
}

/* ============================================================================
 * NuwanSwissMap -- Swiss Table (cache-friendly, SIMD-accelerated hash map)
 *
 * Design summary (see include/casprix/collections.h for the public contract):
 *
 *   * Each slot has a 1-byte control ("ctrl") that lives in a *separate*
 *     array from the slot payload.  Lookups scan 16 ctrl bytes at a time
 *     using a single SSE2 `_mm_cmpeq_epi8` + `_mm_movemask_epi8` pair on
 *     x86-64 (a single AVX2 VEX-encoded sequence when the ASM hook is
 *     linked), NEON `vceqq_u8` on ARM64, and a SWAR-style byte scan on
 *     portable builds.  The hot path is branch-free until a match or an
 *     EMPTY lane is found.
 *
 *   * Ctrl encoding is fixed by the high bit:
 *         EMPTY   = 0x80   (high bit set, low bit clear)
 *         DELETED = 0xFE   (high bit set, low bit set, tombstone)
 *         FULL    = 0..0x7F                  (H2: low 7 bits of the hash)
 *     Every FULL byte has the high bit clear, so a single `movemask` of the
 *     raw ctrl vector gives us "slot is non-full" for free.
 *
 *   * The hash is split as:
 *         H1 = hash >> 7                 — selects the starting 16-slot group
 *         H2 = hash & 0x7F               — stored verbatim in the ctrl byte
 *     This separation means the SIMD compare in the hot path never needs to
 *     touch the payload array at all unless we have a candidate H2 match.
 *
 *   * Probing is quadratic with triangular stride
 *         probe_i = (start + i(i+1)/2) & (num_groups - 1)
 *     which covers every group exactly once when `num_groups` is a power of
 *     two (a well-known property of triangular numbers modulo 2^k).
 *
 *   * Capacity is a power-of-two number of slots, minimum 16.  Load factor
 *     is 7/8 (cap - cap/8 usable slots).  A per-table `growth_left` counter
 *     decrements on every EMPTY-slot insert and triggers rehash at zero --
 *     no per-op `size * 8 < cap * 7` division on the hot path.
 *
 *   * Tombstones are reclaimed *without* growing when they exceed 50% of
 *     capacity; otherwise capacity doubles.  This is the "growth-aware"
 *     policy requested by the design brief: we do not blindly double on
 *     every fill-up -- we first try to reclaim deletes in place.
 *
 *   * Alignment: the `ctrl` array is 16-byte aligned so SSE2 loads are
 *     aligned in the common path (MOVDQA fast-path on older micro-archs;
 *     on Skylake+ unaligned loads are free and we'd be fine with MOVDQU).
 * ============================================================================
 */

#include <stdio.h>   /* for stderr diagnostics (rare) */
#include <assert.h>

/* Select a portable SIMD dialect.  The three paths are logically identical;
 * only the machine code differs.  Compilers collapse each `match_byte` to a
 * 3–5 instruction sequence inlined into the caller. */
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
    #include <emmintrin.h>
    #define SWISS_HAVE_SSE2 1
#endif
#if defined(__ARM_NEON) || defined(__aarch64__)
    #include <arm_neon.h>
    #define SWISS_HAVE_NEON 1
#endif

/* Optional link-time override: when defined, the Swiss Table routes the
 * 16-lane match through the NASM hook declared in runtime/math/simd_kernels.h
 * instead of inlining the intrinsic equivalent.  The two implementations
 * produce identical results; the flag exists so benchmark harnesses and
 * cross-arch builds can pick either path explicitly. */
#if defined(CASPRIX_SWISS_USE_ASM_HOOK)
extern uint32_t casprix_swiss_match_h2_x16(const uint8_t* ctrl16, uint8_t h2);
#endif

/* ── Constants ────────────────────────────────────────────────────────────── */

#define SWISS_GROUP_SIZE     16u                     /* SIMD lane count     */
#define SWISS_INIT_CAP       16u                     /* one group to start  */
#define SWISS_CTRL_EMPTY     ((uint8_t)0x80)
#define SWISS_CTRL_DELETED   ((uint8_t)0xFE)
/* SWISS_CTRL_FULL is the range 0x00..0x7F (the low 7 bits of the hash).   */

/* ── Data layout ──────────────────────────────────────────────────────────── */

typedef struct SwissEntry {
    uint64_t hash;      /* cached full hash (saves a re-hash on rehash)     */
    char*    key;       /* owned; strdup'd from caller's buffer             */
    int64_t  val;
} SwissEntry;

struct NuwanSwissMap {
    Arena*      arena;
    uint8_t*    ctrl;            /* cap bytes, 16-byte aligned               */
    SwissEntry* slots;           /* cap entries, parallel to ctrl            */
    size_t      cap;             /* slot count, power of 2, >= SWISS_GROUP_SIZE */
    size_t      size;            /* full slots                               */
    size_t      tombstones;      /* DELETED slots                            */
    size_t      growth_left;     /* decremented on EMPTY->FULL; 0 -> rehash  */
    size_t      last_probe_len;  /* observability: probe groups scanned on   */
                                 /* the most recent lookup/insert            */
};

/* ── 16-lane byte match: SSE2 / NEON / scalar ─────────────────────────────── */

/* Returns a 16-bit mask; bit i set iff ctrl16[i] == needle.                */
static inline uint32_t swiss_match_byte(const uint8_t* ctrl16, uint8_t needle) {
#if defined(CASPRIX_SWISS_USE_ASM_HOOK)
    return casprix_swiss_match_h2_x16(ctrl16, needle);
#elif defined(SWISS_HAVE_SSE2)
    __m128i g = _mm_loadu_si128((const __m128i*)ctrl16);
    __m128i q = _mm_set1_epi8((char)needle);
    return (uint32_t)(uint16_t)_mm_movemask_epi8(_mm_cmpeq_epi8(g, q));
#elif defined(SWISS_HAVE_NEON)
    /* NEON narrow-shift trick: after the 16-lane compare we have either
     * 0x00 or 0xFF per byte.  Reinterpret as u16, shift right by 4, and
     * narrow -- each u16 collapses to a 4-bit signature, packing 16 lanes
     * into a single 64-bit scalar (4 bits per lane).  Extract every 4th
     * bit to recover the match mask. */
    uint8x16_t g = vld1q_u8(ctrl16);
    uint8x16_t q = vdupq_n_u8(needle);
    uint8x16_t m = vceqq_u8(g, q);
    uint8x8_t narrow = vshrn_n_u16(vreinterpretq_u16_u8(m), 4);
    uint64_t packed = vget_lane_u64(vreinterpret_u64_u8(narrow), 0);
    /* Every 4th bit (& 0x1111...) holds the per-lane match; fold to 16-bit. */
    uint32_t mask = 0;
    for (int i = 0; i < 16; i++) {
        if ((packed >> (i * 4)) & 0x1u) mask |= (1u << i);
    }
    return mask;
#else
    /* Portable SWAR fallback.  Compilers auto-vectorise this on most
     * modern targets when -O2 or higher is in effect. */
    uint32_t mask = 0;
    for (int i = 0; i < 16; i++) {
        if (ctrl16[i] == needle) mask |= (1u << i);
    }
    return mask;
#endif
}

/* 16-bit mask of ctrl bytes that are EMPTY (a single hash-relative sentinel). */
static inline uint32_t swiss_match_empty(const uint8_t* ctrl16) {
    return swiss_match_byte(ctrl16, SWISS_CTRL_EMPTY);
}

/* 16-bit mask of ctrl bytes that are EMPTY or DELETED.  Both have the high
 * bit set; every FULL byte has it clear -- so a single movemask of the raw
 * ctrl vector returns exactly this set.  Saves one compare + broadcast. */
static inline uint32_t swiss_match_empty_or_deleted(const uint8_t* ctrl16) {
#if defined(SWISS_HAVE_SSE2)
    __m128i g = _mm_loadu_si128((const __m128i*)ctrl16);
    return (uint32_t)(uint16_t)_mm_movemask_epi8(g);
#else
    uint32_t mask = 0;
    for (int i = 0; i < 16; i++) {
        if (ctrl16[i] & 0x80u) mask |= (1u << i);
    }
    return mask;
#endif
}

/* Position of the lowest set bit (0..15).  Undefined when mask == 0. */
static inline int swiss_ctz16(uint32_t mask) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctz(mask);
#elif defined(_MSC_VER)
    unsigned long idx;
    _BitScanForward(&idx, mask);
    return (int)idx;
#else
    int i = 0;
    while (!(mask & 1u)) { mask >>= 1; i++; }
    return i;
#endif
}

/* ── Hash plumbing ────────────────────────────────────────────────────────── */

static inline uint64_t swiss_hash(const char* key) {
    /* nuwan_string_hash is FNV-1a 64; good enough distribution for our H2. */
    return nuwan_string_hash(key);
}

static inline uint8_t swiss_h2(uint64_t hash) {
    return (uint8_t)(hash & 0x7Fu);          /* top bit stays 0 -> FULL     */
}

static inline size_t swiss_h1(uint64_t hash) {
    return (size_t)(hash >> 7);
}

/* ── Capacity + growth bookkeeping ────────────────────────────────────────── */

static inline size_t swiss_max_load(size_t cap) {
    /* 7/8 of capacity, round down but never zero for cap >= 16. */
    return cap - (cap >> 3);
}

static size_t swiss_normalise_cap(size_t hint) {
    size_t cap = SWISS_INIT_CAP;
    while (cap < hint) cap <<= 1;
    return cap;
}

/* Allocate the parallel ctrl+slots arrays and initialise ctrl to all-EMPTY.
 * The ctrl array is 16-byte aligned for aligned SIMD loads; we use aligned
 * malloc where available and fall back to `malloc + memset` otherwise. */
static bool swiss_alloc_arrays(NuwanSwissMap* m, size_t cap) {
    /* Aligned to 16 bytes for SIMD metadata loads */
    uint8_t* ctrl = (uint8_t*)arena_alloc_aligned(m->arena, cap, 16);
    if (!ctrl) return false;

    SwissEntry* slots = (SwissEntry*)arena_calloc(m->arena, cap, sizeof(SwissEntry));
    if (!slots) return false;
    memset(ctrl, (int)SWISS_CTRL_EMPTY, cap);

    m->ctrl        = ctrl;
    m->slots       = slots;
    m->cap         = cap;
    m->size        = 0;
    m->tombstones  = 0;
    m->growth_left = swiss_max_load(cap);
    return true;
}

static void swiss_free_arrays(NuwanSwissMap* m) {
    /* No-op in Arena-managed runtime. Individual key freeing must be
     * handled by the Arena reset if they were allocated in the same arena. */
    (void)m;
}

/* ── Rehash-in-place vs. grow-then-rehash ─────────────────────────────────── */

/* Forward decls. */
static bool swiss_rehash(NuwanSwissMap* m, size_t new_cap);

/* If more than half of the unused capacity is tombstones, reclaim them by
 * rehashing into a fresh (same-size) table.  Otherwise grow to 2x capacity. */
static bool swiss_rehash_or_grow(NuwanSwissMap* m) {
    if (!m) return false;
    size_t max_load = swiss_max_load(m->cap);
    /* Tombstones above 50% of the slack (max_load - size) mean we're paying
     * O(tombstone) probe steps on every miss; reclaiming in place is a
     * strict win.  Otherwise grow. */
    if (m->size + m->tombstones > max_load &&
        m->tombstones > (max_load - m->size)) {
        return swiss_rehash(m, m->cap);
    }
    size_t new_cap = m->cap ? m->cap * 2 : SWISS_INIT_CAP;
    return swiss_rehash(m, new_cap);
}

/* ── Core find helper (shared by get / has / remove / insert) ─────────────── */

typedef struct {
    size_t index;       /* FOUND: slot index; NOT_FOUND: insert target      */
    bool   found;
    size_t probe_len;   /* number of groups actually scanned                */
} SwissProbe;

/* Full lookup.  When `found`, `index` is the matching slot.  When not found,
 * `index` points at the first EMPTY/DELETED slot we saw and is safe to
 * insert into (caller is responsible for ctrl-byte/key/val writes). */
static SwissProbe swiss_find_for_insert(const NuwanSwissMap* m,
                                        const char* key,
                                        uint64_t hash) {
    SwissProbe r = {0, false, 0};
    if (!m || m->cap == 0) return r;

    const size_t mask       = m->cap - 1;                 /* cap is 2^k     */
    const size_t num_groups = m->cap / SWISS_GROUP_SIZE;
    const size_t gmask      = num_groups - 1;
    const uint8_t h2        = swiss_h2(hash);

    size_t g = swiss_h1(hash) & gmask;
    size_t step = 0;
    size_t first_candidate = (size_t)-1;   /* first EMPTY/DELETED we saw   */

    for (size_t probes = 0; probes < num_groups; probes++) {
        r.probe_len++;
        const uint8_t* group = m->ctrl + g * SWISS_GROUP_SIZE;

        uint32_t match = swiss_match_byte(group, h2);
        while (match) {
            int b = swiss_ctz16(match);
            size_t idx = g * SWISS_GROUP_SIZE + (size_t)b;
            SwissEntry* e = &m->slots[idx];
            /* Hash cache test first -- skips strcmp on the fast path of a
             * pure H2 collision with a different full hash. */
            if (e->hash == hash && e->key && strcmp(e->key, key) == 0) {
                r.index = idx;
                r.found = true;
                return r;
            }
            match &= match - 1u;              /* clear lowest set bit       */
        }

        /* Remember the earliest EMPTY/DELETED as our insert target. */
        if (first_candidate == (size_t)-1) {
            uint32_t cand = swiss_match_empty_or_deleted(group);
            if (cand) {
                int b = swiss_ctz16(cand);
                first_candidate = g * SWISS_GROUP_SIZE + (size_t)b;
            }
        }

        /* Any EMPTY in this group terminates the probe -- an insert with
         * the same H1 would have stopped here. */
        if (swiss_match_empty(group)) {
            r.index = (first_candidate == (size_t)-1)
                      ? g * SWISS_GROUP_SIZE + (size_t)swiss_ctz16(swiss_match_empty(group))
                      : first_candidate;
            return r;
        }

        step++;
        g = (g + step) & gmask;                  /* triangular probing     */
        (void)mask;                              /* silence -Wunused-var   */
    }

    /* Table is pathologically full (every group has zero EMPTY).  Fall back
     * to any DELETED slot we noticed; otherwise report no room. */
    r.index = first_candidate;
    return r;
}

/* Read-only lookup.  Stops probing at the first EMPTY group. */
static SwissProbe swiss_find(const NuwanSwissMap* m,
                             const char* key,
                             uint64_t hash) {
    SwissProbe r = {0, false, 0};
    if (!m || m->cap == 0) return r;

    const size_t num_groups = m->cap / SWISS_GROUP_SIZE;
    const size_t gmask      = num_groups - 1;
    const uint8_t h2        = swiss_h2(hash);

    size_t g = swiss_h1(hash) & gmask;
    size_t step = 0;

    for (size_t probes = 0; probes < num_groups; probes++) {
        r.probe_len++;
        const uint8_t* group = m->ctrl + g * SWISS_GROUP_SIZE;
        uint32_t match = swiss_match_byte(group, h2);
        while (match) {
            int b = swiss_ctz16(match);
            size_t idx = g * SWISS_GROUP_SIZE + (size_t)b;
            SwissEntry* e = &m->slots[idx];
            if (e->hash == hash && e->key && strcmp(e->key, key) == 0) {
                r.index = idx;
                r.found = true;
                return r;
            }
            match &= match - 1u;
        }
        if (swiss_match_empty(group)) return r;   /* miss */
        step++;
        g = (g + step) & gmask;
    }
    return r;
}

/* ── Rehash ───────────────────────────────────────────────────────────────── */

static bool swiss_rehash(NuwanSwissMap* m, size_t new_cap) {
    if (!m) return false;
    NuwanSwissMap tmp = {0};
    if (!swiss_alloc_arrays(&tmp, new_cap)) return false;

    /* Re-insert every FULL entry into `tmp`, transferring ownership of
     * `key`.  We never free `key` here -- each slot moves verbatim. */
    for (size_t i = 0; i < m->cap; i++) {
        if ((m->ctrl[i] & 0x80u) != 0u) continue;   /* skip EMPTY/DELETED */
        SwissEntry* src = &m->slots[i];
        uint64_t h = src->hash;

        /* Fast-path insert: the target is guaranteed empty so we can
         * bypass the "existing key" arm of swiss_find_for_insert. */
        const size_t num_groups = tmp.cap / SWISS_GROUP_SIZE;
        const size_t gmask      = num_groups - 1;
        size_t g = swiss_h1(h) & gmask;
        size_t step = 0;
        while (1) {
            uint8_t* group = tmp.ctrl + g * SWISS_GROUP_SIZE;
            uint32_t cand = swiss_match_empty_or_deleted(group);
            if (cand) {
                int b = swiss_ctz16(cand);
                size_t idx = g * SWISS_GROUP_SIZE + (size_t)b;
                tmp.ctrl[idx]  = swiss_h2(h);
                tmp.slots[idx] = *src;            /* move */
                tmp.size++;
                tmp.growth_left--;
                break;
            }
            step++;
            g = (g + step) & gmask;
        }
    }

    m->ctrl        = tmp.ctrl;
    m->slots       = tmp.slots;
    m->cap         = tmp.cap;
    m->size        = tmp.size;
    m->tombstones  = 0;
    m->growth_left = swiss_max_load(tmp.cap) - tmp.size;
    return true;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

NuwanSwissMap* nuwan_swiss_new(Arena* a) {
    NuwanSwissMap* m = (NuwanSwissMap*)arena_calloc(a, 1, sizeof(NuwanSwissMap));
    if (!m) return NULL;
    m->arena = a;
    if (!swiss_alloc_arrays(m, SWISS_INIT_CAP)) return NULL;
    return m;
}

NuwanSwissMap* nuwan_swiss_new_reserved(Arena* a, size_t expected_size) {
    NuwanSwissMap* m = nuwan_swiss_new(a);
    if (!m) return NULL;
    nuwan_swiss_reserve(m, expected_size);
    return m;
}

void nuwan_swiss_reserve(NuwanSwissMap* m, size_t expected_size) {
    if (!m) return;
    /* Need cap such that max_load(cap) >= expected_size.  Since
     * max_load(cap) = cap * 7/8, cap >= expected_size * 8/7. */
    size_t need = (expected_size * 8u + 6u) / 7u;
    size_t new_cap = swiss_normalise_cap(need > SWISS_INIT_CAP ? need : SWISS_INIT_CAP);
    if (new_cap > m->cap) {
        swiss_rehash(m, new_cap);
    }
}

bool nuwan_swiss_put(NuwanSwissMap* m, const char* key, int64_t val) {
    if (!m || !key) return false;
    if (m->growth_left == 0) {
        if (!swiss_rehash_or_grow(m)) return false;
    }
    uint64_t hash = swiss_hash(key);
    SwissProbe p = swiss_find_for_insert(m, key, hash);
    m->last_probe_len = p.probe_len;

    if (p.found) {
        m->slots[p.index].val = val;
        return true;
    }

    if (p.index == (size_t)-1) {
        if (!swiss_rehash_or_grow(m)) return false;
        p = swiss_find_for_insert(m, key, hash);
        m->last_probe_len = p.probe_len;
        if (p.index == (size_t)-1) return false;
    }

    const uint8_t prev_ctrl = m->ctrl[p.index];
    char* key_copy = arena_strdup(m->arena, key);
    if (!key_copy) return false;

    m->ctrl[p.index]        = swiss_h2(hash);
    m->slots[p.index].hash  = hash;
    m->slots[p.index].key   = key_copy;
    m->slots[p.index].val   = val;
    m->size++;
    if (prev_ctrl == SWISS_CTRL_EMPTY) {
        m->growth_left--;
    } else {
        assert(prev_ctrl == SWISS_CTRL_DELETED);
        m->tombstones--;
    }
    return true;
}

bool nuwan_swiss_get_opt(const NuwanSwissMap* m, const char* key,
                          int64_t* out_val) {
    if (!m || !key) return false;
    uint64_t hash = swiss_hash(key);
    SwissProbe p = swiss_find(m, key, hash);
    ((NuwanSwissMap*)m)->last_probe_len = p.probe_len;
    if (!p.found) return false;
    if (out_val) *out_val = m->slots[p.index].val;
    return true;
}

int64_t nuwan_swiss_get(const NuwanSwissMap* m, const char* key) {
    int64_t v = 0;
    (void)nuwan_swiss_get_opt(m, key, &v);
    return v;
}

bool nuwan_swiss_has(const NuwanSwissMap* m, const char* key) {
    return nuwan_swiss_get_opt(m, key, NULL);
}

bool nuwan_swiss_remove(NuwanSwissMap* m, const char* key) {
    if (!m || !key) return false;
    uint64_t hash = swiss_hash(key);
    SwissProbe p = swiss_find(m, key, hash);
    ((NuwanSwissMap*)m)->last_probe_len = p.probe_len;
    if (!p.found) return false;

    size_t idx = p.index;
    m->slots[idx].key  = NULL;
    m->slots[idx].hash = 0;
    m->slots[idx].val  = 0;
    m->size--;

    /* If the surrounding group has at least one EMPTY lane, the deleted
     * slot is no longer a probe-chain anchor and we can mark it EMPTY --
     * saving the entire future cost of this tombstone.  Abseil's
     * "convert-to-empty" trick (see: github.com/abseil/abseil-cpp).     */
    size_t group_start = idx & ~(SWISS_GROUP_SIZE - 1);
    const uint8_t* group = m->ctrl + group_start;
    if (swiss_match_empty(group) != 0) {
        m->ctrl[idx] = SWISS_CTRL_EMPTY;
        m->growth_left++;
    } else {
        m->ctrl[idx] = SWISS_CTRL_DELETED;
        m->tombstones++;
    }
    return true;
}

size_t nuwan_swiss_size(const NuwanSwissMap* m)      { return m ? m->size : 0; }
size_t nuwan_swiss_capacity(const NuwanSwissMap* m)  { return m ? m->cap  : 0; }
bool   nuwan_swiss_empty(const NuwanSwissMap* m)     { return !m || m->size == 0; }
size_t nuwan_swiss_last_probe_len(const NuwanSwissMap* m) {
    return m ? m->last_probe_len : 0;
}
size_t nuwan_swiss_tombstones(const NuwanSwissMap* m) {
    return m ? m->tombstones : 0;
}

void nuwan_swiss_clear(NuwanSwissMap* m) {
    if (!m) return;
    memset(m->ctrl, (int)SWISS_CTRL_EMPTY, m->cap);
    memset(m->slots, 0, m->cap * sizeof(SwissEntry));
    m->size        = 0;
    m->tombstones  = 0;
    m->growth_left = swiss_max_load(m->cap);
}

void nuwan_swiss_free(NuwanSwissMap* m) {
    (void)m;
}
