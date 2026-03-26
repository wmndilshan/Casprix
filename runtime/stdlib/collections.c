/*
 * Casprix Runtime - Collections (pure-C implementation)
 * Implements: NuwanList, NuwanMap, NuwanStack, NuwanQueue, NuwanPQ
 * Also provides the ASM-kernel stubs (sum, sort-net4) in portable C.
 */

#include "../../include/casprix/collections.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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
    int64_t s = 0;
    for (size_t i = 0; i < n; i++) s += arr[i];
    return s;
}

/* nuwan_strlen_asm / memcopy / memset_fast — defined in string_ops.c */

/* ── NuwanList ──────────────────────────────────────────────────────────── */

#define LIST_INIT_CAP 16

struct NuwanList {
    int64_t* data;
    size_t   size;
    size_t   cap;
};

NuwanList* nuwan_list_new(void) {
    NuwanList* l = (NuwanList*)malloc(sizeof(NuwanList));
    if (!l) return NULL;
    l->data = (int64_t*)malloc(LIST_INIT_CAP * sizeof(int64_t));
    if (!l->data) { free(l); return NULL; }
    l->size = 0;
    l->cap  = LIST_INIT_CAP;
    return l;
}

bool nuwan_list_push(NuwanList* l, int64_t v) {
    if (!l) return false;
    if (l->size == l->cap) {
        size_t nc = l->cap * 2;
        int64_t* nd = (int64_t*)realloc(l->data, nc * sizeof(int64_t));
        if (!nd) return false;
        l->data = nd; l->cap = nc;
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
    if (!l) return;
    free(l->data);
    free(l);
}

/* ── NuwanMap (Robin Hood open-addressing, string key → int64) ─────────── */

#define MAP_INIT_CAP 16
#define MAP_LOAD_NUM 3
#define MAP_LOAD_DEN 4   /* load factor 0.75 */
#define MAP_EMPTY    UINT64_MAX

typedef struct { char* key; int64_t val; uint64_t hash; } MapEntry;

struct NuwanMap {
    MapEntry* slots;
    size_t    size;
    size_t    cap;
};

NuwanMap* nuwan_map_new(void) {
    NuwanMap* m = (NuwanMap*)calloc(1, sizeof(NuwanMap));
    if (!m) return NULL;
    m->cap   = MAP_INIT_CAP;
    m->slots = (MapEntry*)calloc(m->cap, sizeof(MapEntry));
    if (!m->slots) { free(m); return NULL; }
    for (size_t i = 0; i < m->cap; i++) m->slots[i].hash = MAP_EMPTY;
    return m;
}

static void map_insert_raw(MapEntry* slots, size_t cap, char* key, int64_t val, uint64_t h) {
    size_t idx = h % cap;
    while (1) {
        if (slots[idx].hash == MAP_EMPTY) {
            slots[idx] = (MapEntry){key, val, h};
            return;
        }
        if (slots[idx].hash == h && strcmp(slots[idx].key, key) == 0) {
            free(key);  /* duplicate key — update value */
            slots[idx].val = val;
            return;
        }
        idx = (idx + 1) % cap;
    }
}

static void map_grow(NuwanMap* m) {
    size_t nc = m->cap * 2;
    MapEntry* ns = (MapEntry*)calloc(nc, sizeof(MapEntry));
    if (!ns) return;
    for (size_t i = 0; i < nc; i++) ns[i].hash = MAP_EMPTY;
    for (size_t i = 0; i < m->cap; i++) {
        if (m->slots[i].hash != MAP_EMPTY)
            map_insert_raw(ns, nc, m->slots[i].key, m->slots[i].val, m->slots[i].hash);
    }
    free(m->slots);
    m->slots = ns;
    m->cap   = nc;
}

void nuwan_map_put(NuwanMap* m, const char* key, int64_t val) {
    if (!m || !key) return;
    if (m->size * MAP_LOAD_DEN >= m->cap * MAP_LOAD_NUM) map_grow(m);
    uint64_t h = nuwan_string_hash(key);
    map_insert_raw(m->slots, m->cap, strdup(key), val, h);
    m->size++;
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
    for (size_t i = 0; i < m->cap; i++) {
        if (m->slots[i].hash != MAP_EMPTY) { free(m->slots[i].key); m->slots[i].hash = MAP_EMPTY; }
    }
    m->size = 0;
}

void nuwan_map_free(NuwanMap* m) {
    if (!m) return;
    nuwan_map_clear(m);
    free(m->slots);
    free(m);
}

/* ── NuwanStack (alias over NuwanList) ─────────────────────────────────── */

NuwanStack* nuwan_stack_new(void)                    { return nuwan_list_new(); }
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
    int64_t* data;
    size_t   head, tail, size, cap;
};

NuwanQueue* nuwan_queue_new(void) {
    NuwanQueue* q = (NuwanQueue*)calloc(1, sizeof(NuwanQueue));
    if (!q) return NULL;
    q->data = (int64_t*)malloc(QUEUE_INIT_CAP * sizeof(int64_t));
    if (!q->data) { free(q); return NULL; }
    q->cap = QUEUE_INIT_CAP;
    return q;
}

bool nuwan_queue_enqueue(NuwanQueue* q, int64_t v) {
    if (!q) return false;
    if (q->size == q->cap) {
        size_t nc = q->cap * 2;
        int64_t* nd = (int64_t*)malloc(nc * sizeof(int64_t));
        if (!nd) return false;
        /* linearise ring */
        for (size_t i = 0; i < q->size; i++) nd[i] = q->data[(q->head + i) % q->cap];
        free(q->data);
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
    if (!q) return;
    free(q->data);
    free(q);
}

/* ── NuwanPQ (binary min-heap) ──────────────────────────────────────────── */

#define PQ_INIT_CAP 16

struct NuwanPQ {
    int64_t* prios;
    int64_t* vals;
    size_t   size, cap;
};

NuwanPQ* nuwan_pq_new(void) {
    NuwanPQ* pq = (NuwanPQ*)calloc(1, sizeof(NuwanPQ));
    if (!pq) return NULL;
    pq->prios = (int64_t*)malloc(PQ_INIT_CAP * sizeof(int64_t));
    pq->vals  = (int64_t*)malloc(PQ_INIT_CAP * sizeof(int64_t));
    if (!pq->prios || !pq->vals) { free(pq->prios); free(pq->vals); free(pq); return NULL; }
    pq->cap = PQ_INIT_CAP;
    return pq;
}

static void pq_swap(NuwanPQ* pq, size_t a, size_t b) {
    int64_t tp = pq->prios[a]; pq->prios[a] = pq->prios[b]; pq->prios[b] = tp;
    int64_t tv = pq->vals[a];  pq->vals[a]  = pq->vals[b];  pq->vals[b]  = tv;
}

bool nuwan_pq_push(NuwanPQ* pq, int64_t priority, int64_t value) {
    if (!pq) return false;
    if (pq->size == pq->cap) {
        size_t nc = pq->cap * 2;
        int64_t* np = (int64_t*)realloc(pq->prios, nc * sizeof(int64_t));
        int64_t* nv = (int64_t*)realloc(pq->vals,  nc * sizeof(int64_t));
        if (!np || !nv) return false;
        pq->prios = np; pq->vals = nv; pq->cap = nc;
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
    if (!pq) return;
    free(pq->prios);
    free(pq->vals);
    free(pq);
}
