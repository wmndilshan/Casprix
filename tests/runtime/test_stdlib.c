/*
 * Casprix Stdlib Test Suite
 * Tests: string operations, collections (list/map/stack/queue/pq)
 * Build: see build_stdlib.ps1 --Test
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "casprix/string_ops.h"
#include "casprix/collections.h"

/* ---- Test helpers ---- */
static int g_pass = 0, g_fail = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  [PASS] %s\n", name); g_pass++; } \
    else       { printf("  [FAIL] %s\n", name); g_fail++; } \
} while (0)

#define CHECK_STR(got, exp, name) do { \
    bool _ok = (got) && strcmp(got, exp) == 0; \
    if (_ok) { printf("  [PASS] %s\n", name); g_pass++; } \
    else { printf("  [FAIL] %s — got='%s' expected='%s'\n", name, (got)?got:"NULL", exp); g_fail++; } \
    if (got) free((void*)got); \
} while (0)

/* ============================================================
 *  Section 1: String Operations
 * ========================================================== */
static void test_strings(void) {
    printf("\n--- String Operations ---\n");

    /* concat */
    char* s = nuwan_string_concat("Hello", ", World!");
    CHECK_STR(s, "Hello, World!", "concat basic");

    s = nuwan_string_concat(NULL, "ok");
    CHECK_STR(s, "ok", "concat null left");

    s = nuwan_string_concat("ok", NULL);
    CHECK_STR(s, "ok", "concat null right");

    /* length */
    CHECK(nuwan_string_length("hello") == 5, "length 5");
    CHECK(nuwan_string_length("") == 0,       "length empty");
    CHECK(nuwan_string_length(NULL) == 0,      "length null");

    /* to_upper SSE2 */
    s = nuwan_string_to_upper("hello world 123");
    CHECK_STR(s, "HELLO WORLD 123", "to_upper SSE2");

    s = nuwan_string_to_lower("HELLO WORLD 123");
    CHECK_STR(s, "hello world 123", "to_lower SSE2");

    /* BMH search */
    CHECK(nuwan_string_index_of("hello world", "world") == 6, "index_of BMH");
    CHECK(nuwan_string_index_of("hello", "xyz") == -1,        "index_of miss");
    CHECK(nuwan_string_index_of("aaaa", "aa") == 0,            "index_of overlap");
    CHECK(nuwan_string_last_index_of("aabaab", "aa") == 3,     "last_index_of");

    /* starts_with / ends_with (memcmp) */
    CHECK(nuwan_string_starts_with("Hello World", "Hello"),  "starts_with yes");
    CHECK(!nuwan_string_starts_with("Hello", "World"),       "starts_with no");
    CHECK(nuwan_string_ends_with("Hello World", "World"),    "ends_with yes");
    CHECK(!nuwan_string_ends_with("Hello", "World"),         "ends_with no");

    /* trim */
    s = nuwan_string_trim("  hello  ");
    CHECK_STR(s, "hello", "trim");

    /* replace_all single-alloc */
    s = nuwan_string_replace_all("aabbaa", "aa", "X");
    CHECK_STR(s, "XbbX", "replace_all");

    /* split */
    size_t count = 0;
    char** parts = nuwan_string_split("a,b,c,d", ",", &count);
    CHECK(count == 4, "split count");
    CHECK(parts && strcmp(parts[0], "a") == 0, "split[0]");
    CHECK(parts && strcmp(parts[3], "d") == 0, "split[3]");
    if (parts) {
        for (size_t i = 0; i < count; i++) free(parts[i]);
        free(parts);
    }

    /* int_to_string fast path */
    s = nuwan_int_to_string(12345);
    CHECK_STR(s, "12345", "int_to_string pos");

    s = nuwan_int_to_string(-9876);
    CHECK_STR(s, "-9876", "int_to_string neg");

    s = nuwan_int_to_string(0);
    CHECK_STR(s, "0", "int_to_string zero");

    /* char_at bounds */
    CHECK(nuwan_string_char_at("hello", 1) == 'e', "char_at inbounds");
    CHECK(nuwan_string_char_at("hello", 99) == '\0', "char_at oob");

    /* hash (FNV-1a) — just check it's non-zero and stable */
    uint64_t h1 = nuwan_string_hash_public("Casprix");
    uint64_t h2 = nuwan_string_hash_public("Casprix");
    CHECK(h1 != 0 && h1 == h2, "hash deterministic");
    CHECK(nuwan_string_hash_public("abc") != nuwan_string_hash_public("xyz"), "hash collision-free");
}

/* ============================================================
 *  Section 2: NuwanList (cache-aligned, introsort, sum)
 * ========================================================== */
static void test_list(void) {
    printf("\n--- NuwanList ---\n");

    NuwanList* l = nuwan_list_new();
    CHECK(l != NULL, "list_new");
    CHECK(nuwan_list_empty(l), "list_empty init");

    for (int i = 0; i < 10000; i++) nuwan_list_push(l, (int64_t)i);
    CHECK(nuwan_list_size(l) == 10000, "list push 10k");
    CHECK(nuwan_list_get(l, 9999) == 9999, "list_get last");
    CHECK(nuwan_list_get(l, 99999) == 0,   "list_get oob safe");

    /* set + get */
    nuwan_list_set(l, 0, 42);
    CHECK(nuwan_list_get(l, 0) == 42, "list_set");

    /* SSE2 sum */
    nuwan_list_set(l, 0, 0);    /* restore: sum = 0+1+2+...+9999 */
    int64_t expected_sum = (int64_t)9999 * 10000 / 2;
    int64_t got_sum = nuwan_list_sum(l);
    CHECK(got_sum == expected_sum, "list_sum SSE2");

    /* Sort */
    NuwanList* sl = nuwan_list_new();
    int64_t vals[] = {5, 3, 8, 1, 7, 2, 6, 4, 0, 9};
    for (int i = 0; i < 10; i++) nuwan_list_push(sl, vals[i]);
    nuwan_list_sort(sl);
    bool sorted = true;
    for (int i = 1; i < 10; i++) {
        if (nuwan_list_get(sl, i) < nuwan_list_get(sl, i-1)) { sorted = false; break; }
    }
    CHECK(sorted, "list_sort introsort");

    /* Binary search after sort */
    int64_t idx = nuwan_list_binary_search(sl, 7);
    CHECK(idx >= 0 && nuwan_list_get(sl, (size_t)idx) == 7, "list_binary_search");
    CHECK(nuwan_list_binary_search(sl, 999) == -1, "list_binary_search miss");

    /* Remove */
    nuwan_list_remove(sl, 0);
    CHECK(nuwan_list_size(sl) == 9, "list_remove");
    CHECK(nuwan_list_get(sl, 0) == 1, "list_remove shift");

    nuwan_list_free(l);
    nuwan_list_free(sl);
    printf("  [PASS] list_free (no crash)\n"); g_pass++;
}

/* ============================================================
 *  Section 3: NuwanMap (Robin Hood hashmap)
 * ========================================================== */
static void test_map(void) {
    printf("\n--- NuwanMap (Robin Hood) ---\n");

    NuwanMap* m = nuwan_map_new();
    CHECK(m != NULL, "map_new");
    CHECK(nuwan_map_empty(m), "map_empty init");

    nuwan_map_put(m, "alpha", 1);
    nuwan_map_put(m, "beta",  2);
    nuwan_map_put(m, "gamma", 3);
    CHECK(nuwan_map_size(m) == 3, "map_size after 3 puts");
    CHECK(nuwan_map_get(m, "alpha") == 1, "map_get alpha");
    CHECK(nuwan_map_get(m, "gamma") == 3, "map_get gamma");
    CHECK(nuwan_map_get(m, "delta") == 0, "map_get miss");
    CHECK(nuwan_map_has(m, "beta"),        "map_has yes");
    CHECK(!nuwan_map_has(m, "omega"),      "map_has no");

    /* Update */
    nuwan_map_put(m, "alpha", 100);
    CHECK(nuwan_map_get(m, "alpha") == 100, "map update");
    CHECK(nuwan_map_size(m) == 3, "map_size unchanged after update");

    /* Remove */
    CHECK(nuwan_map_remove(m, "beta"),    "map_remove yes");
    CHECK(!nuwan_map_remove(m, "beta"),   "map_remove already gone");
    CHECK(nuwan_map_size(m) == 2, "map_size after remove");

    /* Stress: 5000 inserts, verify all present */
    char key[32];
    for (int i = 0; i < 5000; i++) {
        snprintf(key, sizeof(key), "key_%d", i);
        nuwan_map_put(m, key, (int64_t)i * 7);
    }
    bool all_ok = true;
    for (int i = 0; i < 5000; i++) {
        snprintf(key, sizeof(key), "key_%d", i);
        if (nuwan_map_get(m, key) != (int64_t)i * 7) { all_ok = false; break; }
    }
    CHECK(all_ok, "map stress 5000 inserts+gets");

    nuwan_map_free(m);
    printf("  [PASS] map_free (no crash)\n"); g_pass++;
}

/* ============================================================
 *  Section 4: Stack, Queue, PriorityQueue
 * ========================================================== */
static void test_stack(void) {
    printf("\n--- NuwanStack ---\n");
    NuwanStack* s = nuwan_stack_new();
    nuwan_stack_push(s, 10);
    nuwan_stack_push(s, 20);
    nuwan_stack_push(s, 30);
    CHECK(nuwan_stack_peek(s) == 30, "stack_peek");
    CHECK(nuwan_stack_pop(s) == 30,  "stack_pop 30");
    CHECK(nuwan_stack_pop(s) == 20,  "stack_pop 20");
    CHECK(nuwan_stack_pop(s) == 10,  "stack_pop 10");
    CHECK(nuwan_stack_pop(s) == 0,   "stack_pop empty safe");
    nuwan_stack_free(s);
}

static void test_queue(void) {
    printf("\n--- NuwanQueue (ring-buffer) ---\n");
    NuwanQueue* q = nuwan_queue_new();
    for (int i = 0; i < 100; i++) nuwan_queue_enqueue(q, i);
    CHECK(nuwan_queue_size(q) == 100, "queue size 100");
    CHECK(nuwan_queue_peek(q) == 0,   "queue peek front");
    bool fifo = true;
    for (int i = 0; i < 100; i++) {
        if (nuwan_queue_dequeue(q) != i) { fifo = false; break; }
    }
    CHECK(fifo, "queue FIFO order");
    CHECK(nuwan_queue_empty(q), "queue empty after drain");
    CHECK(nuwan_queue_dequeue(q) == 0, "queue dequeue empty safe");
    nuwan_queue_free(q);
}

static void test_pq(void) {
    printf("\n--- NuwanPQ (binary min-heap) ---\n");
    NuwanPQ* pq = nuwan_pq_new();
    nuwan_pq_push(pq, 5,  50);
    nuwan_pq_push(pq, 1,  10);
    nuwan_pq_push(pq, 3,  30);
    nuwan_pq_push(pq, 2,  20);
    nuwan_pq_push(pq, 4,  40);
    CHECK(nuwan_pq_peek_priority(pq) == 1, "pq peek min priority");
    CHECK(nuwan_pq_pop(pq) == 10, "pq pop priority=1 → val=10");
    CHECK(nuwan_pq_pop(pq) == 20, "pq pop priority=2 → val=20");
    CHECK(nuwan_pq_pop(pq) == 30, "pq pop priority=3 → val=30");
    nuwan_pq_free(pq);
}

/* ============================================================
 *  Performance micro-benchmark
 * ========================================================== */
static void bench(void) {
    printf("\n--- Micro-benchmarks ---\n");

    /* String concat 1M times */
    clock_t t0 = clock();
    for (int i = 0; i < 1000000; i++) {
        char* s = nuwan_string_concat("Hello", " World");
        free(s);
    }
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    printf("  concat x1M:      %.1f ms\n", ms);

    /* List push+pop 1M */
    NuwanList* l = nuwan_list_new();
    t0 = clock();
    for (int i = 0; i < 1000000; i++) nuwan_list_push(l, i);
    ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    printf("  list push x1M:   %.1f ms\n", ms);

    t0 = clock();
    nuwan_list_sort(l);
    ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    printf("  list sort 1M:    %.1f ms\n", ms);

    t0 = clock();
    int64_t sum = nuwan_list_sum(l);
    ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    printf("  SSE2 sum 1M:     %.1f ms (result=%lld)\n", ms, (long long)sum);
    nuwan_list_free(l);

    /* Map 100K inserts */
    NuwanMap* m = nuwan_map_new();
    char key[32];
    t0 = clock();
    for (int i = 0; i < 100000; i++) {
        snprintf(key, sizeof(key), "k%d", i);
        nuwan_map_put(m, key, i);
    }
    ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    printf("  map put x100K:   %.1f ms\n", ms);
    nuwan_map_free(m);
}

/* ============================================================
 *  Main
 * ========================================================== */
int main(void) {
    printf("\n");
    printf("==============================================\n");
    printf("  Casprix Stdlib Test Suite\n");
    printf("  High-Performance Runtime Verification\n");
    printf("==============================================\n");

    test_strings();
    test_list();
    test_map();
    test_stack();
    test_queue();
    test_pq();
    bench();

    printf("\n==============================================\n");
    int total = g_pass + g_fail;
    printf("  Results: %d/%d PASSED", g_pass, total);
    if (g_fail == 0) {
        printf("  ✓ ALL PASSED\n");
    } else {
        printf("  ✗ %d FAILED\n", g_fail);
    }
    printf("==============================================\n\n");
    return g_fail > 0 ? 1 : 0;
}
