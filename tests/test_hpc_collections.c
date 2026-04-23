#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "../include/casprix/collections.h"
#include "../src/support/arena.h"

#define NUM_ITER 100000
#define NUM_ENTRIES 10000

void test_list_simd(Arena* a) {
    printf("[TEST] NuwanList SIMD Sum... ");
    NuwanList* l = nuwan_list_new(a);
    nuwan_list_reserve(l, NUM_ENTRIES);
    
    int64_t expected = 0;
    for (int i = 0; i < NUM_ENTRIES; i++) {
        nuwan_list_push(l, i);
        expected += i;
    }
    
    int64_t actual = nuwan_list_sum(l);
    assert(actual == expected);
    printf("PASSED (Sum: %ld)\n", actual);
}

void test_map_robin_hood(Arena* a) {
    printf("[TEST] NuwanMap Robin Hood Hashing... ");
    NuwanMap* m = nuwan_map_new(a);
    nuwan_map_reserve(m, NUM_ENTRIES);
    
    char key[64];
    for (int i = 0; i < NUM_ENTRIES; i++) {
        sprintf(key, "key_%d", i);
        nuwan_map_put(m, key, i);
    }
    
    for (int i = 0; i < NUM_ENTRIES; i++) {
        sprintf(key, "key_%d", i);
        assert(nuwan_map_get(m, key) == i);
    }
    
    printf("PASSED (Size: %zu)\n", nuwan_map_size(m));
}

void test_arena_efficiency(Arena* a) {
    printf("[TEST] Arena Allocation Efficiency... ");
    size_t allocated, used, blocks;
    arena_stats(a, &allocated, &used, &blocks);
    
    /* We expect some waste due to growth resizes, but it should be bounded. */
    double efficiency = (double)used / (double)allocated;
    printf("Used: %zu, Allocated: %zu, Blocks: %zu, Efficiency: %.2f%%\n", 
           used, allocated, blocks, efficiency * 100.0);
    
    assert(efficiency > 0.4); /* At least 40% efficiency in a high-growth stress test is acceptable for a bump allocator */
}

int main() {
    Arena* a = arena_create();
    if (!a) return 1;
    
    clock_t start = clock();
    
    test_list_simd(a);
    test_map_robin_hood(a);
    test_arena_efficiency(a);
    
    clock_t end = clock();
    double time_spent = (double)(end - start) / CLOCKS_PER_SEC;
    
    printf("\n[RESULT] All HPC Collection tests passed in %.4f seconds.\n", time_spent);
    
    arena_destroy(a);
    return 0;
}
