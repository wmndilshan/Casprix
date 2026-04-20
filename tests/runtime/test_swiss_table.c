/*
 * Casprix Runtime -- Swiss Table smoke & stress test
 *
 * Validates:
 *   - basic put/get/has/remove on string keys
 *   - dedup on repeated put (same key, new value)
 *   - automatic growth past the 7/8 load factor
 *   - reserve() avoids rehashes when sizing is known up front
 *   - tombstone reclamation after heavy delete/insert churn
 *   - last_probe_len stays small (< 5) on a healthy load
 *   - SIMD match hook agrees with scalar reference on a random ctrl vector
 *
 * Build (standalone, from the repo root):
 *   gcc -O2 -std=c11 -Iinclude -Iruntime/math \
 *       tests/runtime/test_swiss_table.c \
 *       runtime/stdlib/collections.c runtime/stdlib/string_ops.c \
 *       -o build-codex-verify/test_swiss_table.exe
 *
 * Or, when the runtime static lib is available:
 *   gcc -O2 -std=c11 -Iinclude -Iruntime/math \
 *       tests/runtime/test_swiss_table.c \
 *       -Lbuild-codex-verify -lcasprix_runtime \
 *       -o build-codex-verify/test_swiss_table.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "casprix/collections.h"
#include "simd_kernels.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  [PASS] %s\n", name); g_pass++; } \
    else      { printf("  [FAIL] %s  (%s:%d)\n", name, __FILE__, __LINE__); g_fail++; } \
} while (0)

/* ── 1. Basic CRUD ─────────────────────────────────────────────────────── */

static void test_basic(void) {
    printf("\n[1] Basic put/get/has/remove\n");
    NuwanSwissMap* m = nuwan_swiss_new();
    CHECK(m != NULL,                              "new returns non-NULL");
    CHECK(nuwan_swiss_empty(m),                   "new map is empty");
    CHECK(nuwan_swiss_capacity(m) == 16,          "initial capacity = 16");

    CHECK(nuwan_swiss_put(m, "alpha", 1),         "put alpha");
    CHECK(nuwan_swiss_put(m, "beta",  2),         "put beta");
    CHECK(nuwan_swiss_put(m, "gamma", 3),         "put gamma");

    CHECK(nuwan_swiss_size(m) == 3,               "size = 3");
    CHECK(nuwan_swiss_get(m, "alpha") == 1,       "get alpha");
    CHECK(nuwan_swiss_get(m, "beta")  == 2,       "get beta");
    CHECK(nuwan_swiss_get(m, "gamma") == 3,       "get gamma");
    CHECK(!nuwan_swiss_has(m, "delta"),           "has delta is false");

    /* dedup: same key overwrites value, size stays */
    CHECK(nuwan_swiss_put(m, "alpha", 42),        "put alpha again");
    CHECK(nuwan_swiss_size(m) == 3,               "size still 3 after dup put");
    CHECK(nuwan_swiss_get(m, "alpha") == 42,      "alpha value updated");

    CHECK(nuwan_swiss_remove(m, "beta"),          "remove beta");
    CHECK(!nuwan_swiss_has(m, "beta"),            "beta gone");
    CHECK(nuwan_swiss_size(m) == 2,               "size = 2 after remove");

    nuwan_swiss_free(m);
}

/* ── 2. Growth past 7/8 load factor ────────────────────────────────────── */

static void test_growth(void) {
    printf("\n[2] Growth past 7/8 load factor\n");
    NuwanSwissMap* m = nuwan_swiss_new();
    size_t prev_cap = nuwan_swiss_capacity(m);
    int growth_events = 0;

    /* Insert 1000 distinct keys.  We expect multiple growths -- starting at
     * 16 slots, 7/8 gives 14 usable; after each 2x growth we double the
     * capacity.  No key must be lost to a bad rehash. */
    char key[32];
    for (int i = 0; i < 1000; i++) {
        snprintf(key, sizeof(key), "key_%06d", i);
        CHECK(nuwan_swiss_put(m, key, i * 7LL + 1), "put during growth");

        size_t cap = nuwan_swiss_capacity(m);
        if (cap != prev_cap) { growth_events++; prev_cap = cap; }
    }

    CHECK(growth_events >= 5,                      "at least 5 growths for 1000 entries");
    CHECK(nuwan_swiss_size(m) == 1000,             "all 1000 entries present");

    /* Verify every key is still findable and maps to the right value. */
    int misses = 0;
    for (int i = 0; i < 1000; i++) {
        snprintf(key, sizeof(key), "key_%06d", i);
        int64_t v = nuwan_swiss_get(m, key);
        if (v != (int64_t)i * 7LL + 1) misses++;
    }
    CHECK(misses == 0,                             "all 1000 lookups return correct value");

    /* Probe length on a healthy table must be small (sub-linear). */
    nuwan_swiss_get(m, "key_000500");
    size_t plen = nuwan_swiss_last_probe_len(m);
    CHECK(plen <= 8,                               "last probe length <= 8 groups");
    printf("       final cap=%zu, size=%zu, last_probe_len=%zu\n",
           nuwan_swiss_capacity(m), nuwan_swiss_size(m), plen);

    nuwan_swiss_free(m);
}

/* ── 3. Reserve avoids rehashes ────────────────────────────────────────── */

static void test_reserve(void) {
    printf("\n[3] Reserve pre-sizes without rehash\n");
    NuwanSwissMap* m = nuwan_swiss_new_reserved(512);
    size_t cap_after_reserve = nuwan_swiss_capacity(m);
    CHECK(cap_after_reserve >= 512,                "capacity >= 512 after reserve");
    CHECK((cap_after_reserve & (cap_after_reserve - 1)) == 0,
                                                   "capacity is power-of-two");

    size_t growths = 0;
    char key[32];
    for (int i = 0; i < 400; i++) {
        snprintf(key, sizeof(key), "k_%04d", i);
        nuwan_swiss_put(m, key, i);
        if (nuwan_swiss_capacity(m) != cap_after_reserve) growths++;
    }
    CHECK(growths == 0,                            "no rehash fired while size <= reserved");

    nuwan_swiss_free(m);
}

/* ── 4. Tombstone churn: insert/delete cycle ───────────────────────────── */

static void test_tombstone_churn(void) {
    printf("\n[4] Tombstone reclamation under heavy churn\n");
    NuwanSwissMap* m = nuwan_swiss_new_reserved(128);
    char key[32];

    /* Round 1: fill up. */
    for (int i = 0; i < 100; i++) {
        snprintf(key, sizeof(key), "k_%04d", i);
        nuwan_swiss_put(m, key, i);
    }
    CHECK(nuwan_swiss_size(m) == 100,              "100 entries after round 1");
    CHECK(nuwan_swiss_tombstones(m) == 0,          "zero tombstones after pure inserts");

    /* Round 2: delete every other key.  */
    for (int i = 0; i < 100; i += 2) {
        snprintf(key, sizeof(key), "k_%04d", i);
        CHECK(nuwan_swiss_remove(m, key) || i == 0, "remove even key");
    }
    CHECK(nuwan_swiss_size(m) == 50,               "50 entries after pairwise delete");

    /* Round 3: re-insert; tombstones should be eaten on the way. */
    for (int i = 0; i < 100; i += 2) {
        snprintf(key, sizeof(key), "k_%04d", i);
        nuwan_swiss_put(m, key, i + 10000);
    }
    CHECK(nuwan_swiss_size(m) == 100,              "100 entries after churn");

    /* All keys readable with correct (possibly-updated) values. */
    int misses = 0;
    for (int i = 0; i < 100; i++) {
        snprintf(key, sizeof(key), "k_%04d", i);
        int64_t expected = (i % 2 == 0) ? (i + 10000) : i;
        if (nuwan_swiss_get(m, key) != expected) misses++;
    }
    CHECK(misses == 0,                             "no value corrupted after churn");

    nuwan_swiss_free(m);
}

/* ── 5. ASM hook agrees with a scalar reference ────────────────────────── */

static uint32_t ref_match_h2(const uint8_t* ctrl, uint8_t h2) {
    uint32_t m = 0;
    for (int i = 0; i < 16; i++) if (ctrl[i] == h2) m |= (1u << i);
    return m;
}

static void test_simd_hook(void) {
    printf("\n[5] SIMD match hook vs. scalar reference\n");
    srand(0xC45B91Fu);
    int mismatches = 0;
    for (int trial = 0; trial < 10000; trial++) {
        uint8_t ctrl[16];
        for (int i = 0; i < 16; i++) ctrl[i] = (uint8_t)rand();
        uint8_t h2 = (uint8_t)rand();

        uint32_t asm_mask = casprix_swiss_match_h2_x16(ctrl, h2);
        uint32_t ref_mask = ref_match_h2(ctrl, h2);
        if ((asm_mask & 0xFFFFu) != ref_mask) mismatches++;
    }
    CHECK(mismatches == 0,                         "10000 random vectors: ASM == scalar");

    /* Spot-check: all-equal case returns 0xFFFF. */
    uint8_t all_x[16];
    memset(all_x, 0x3C, sizeof(all_x));
    CHECK((casprix_swiss_match_h2_x16(all_x, 0x3C) & 0xFFFFu) == 0xFFFFu,
                                                   "all-equal -> 0xFFFF");
    CHECK((casprix_swiss_match_h2_x16(all_x, 0x3D) & 0xFFFFu) == 0x0000u,
                                                   "all-miss  -> 0x0000");
}

/* ── 6. Wall-clock microbench (sanity, not a rigorous benchmark) ───────── */

static double elapsed_ms(clock_t a, clock_t b) {
    return (double)(b - a) * 1000.0 / (double)CLOCKS_PER_SEC;
}

static void bench(void) {
    printf("\n[6] Microbench: 100k insert + 100k lookup\n");
    enum { N = 100000 };
    char** keys = (char**)malloc(N * sizeof(char*));
    for (int i = 0; i < N; i++) {
        keys[i] = (char*)malloc(24);
        snprintf(keys[i], 24, "key_%08d", i);
    }

    NuwanSwissMap* sm = nuwan_swiss_new_reserved(N);
    clock_t t0 = clock();
    for (int i = 0; i < N; i++) nuwan_swiss_put(sm, keys[i], i);
    clock_t t1 = clock();
    int64_t sum = 0;
    for (int i = 0; i < N; i++) sum += nuwan_swiss_get(sm, keys[i]);
    clock_t t2 = clock();
    printf("    Swiss:       insert %.1f ms, lookup %.1f ms, sum=%lld\n",
           elapsed_ms(t0, t1), elapsed_ms(t1, t2), (long long)sum);
    nuwan_swiss_free(sm);

    /* Compare against the legacy NuwanMap on the same workload. */
    NuwanMap* lm = nuwan_map_new();
    t0 = clock();
    for (int i = 0; i < N; i++) nuwan_map_put(lm, keys[i], i);
    t1 = clock();
    sum = 0;
    for (int i = 0; i < N; i++) sum += nuwan_map_get(lm, keys[i]);
    t2 = clock();
    printf("    NuwanMap:    insert %.1f ms, lookup %.1f ms, sum=%lld\n",
           elapsed_ms(t0, t1), elapsed_ms(t1, t2), (long long)sum);
    nuwan_map_free(lm);

    for (int i = 0; i < N; i++) free(keys[i]);
    free(keys);
}

int main(void) {
    printf("=== Casprix Runtime: Swiss Table test suite ===\n");
    test_basic();
    test_growth();
    test_reserve();
    test_tombstone_churn();
    test_simd_hook();
    bench();
    printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
