// Coroutine Test Suite
// Tests for Windows fiber-based coroutine implementation

#include "runtime/async/coroutine.h"
#include <stdio.h>
#include <assert.h>
#include <windows.h>

// --- Test Utilities ---

static int test_count = 0;
static int test_passed = 0;

#define TEST(name) \
    static void test_##name(void); \
    static void run_test_##name(void) { \
        printf("Running: %s...", #name); \
        test_count++; \
        test_##name(); \
        test_passed++; \
        printf(" OK\n"); \
    } \
    static void test_##name(void)

// --- Test Cases ---

// Test 1: Basic coroutine creation and destruction
TEST(basic_create_destroy) {
    Coroutine* main = coro_thread_init();
    assert(main != NULL);
    assert(coro_current() == main);
    
    Coroutine* coro = coro_create(NULL, NULL, 0);
    assert(coro != NULL);
    
    coro_destroy(coro);
    coro_thread_cleanup();
}

// Test 2: Simple coroutine execution
static int exec_counter = 0;

static void simple_entry(void* ctx) {
    exec_counter++;
    int value = *(int*)ctx;
    assert(value == 42);
}

TEST(simple_execution) {
    exec_counter = 0;
    
    coro_thread_init();
    
    int value = 42;
    Coroutine* coro = coro_create(simple_entry, &value, 0);
    assert(coro != NULL);
    
    Coroutine* main = coro_get_main();
    coro_switch(main, coro);
    
    assert(exec_counter == 1);
    assert(coro_is_finished(coro));
    
    coro_destroy(coro);
    coro_thread_cleanup();
}

// Test 3: Coroutine yielding
static int yield_count = 0;

static void yielding_entry(void* ctx) {
    for (int i = 0; i < 5; i++) {
        yield_count++;
        coro_yield();
    }
}

TEST(yielding) {
    yield_count = 0;
    
    coro_thread_init();
    Coroutine* main = coro_get_main();
    
    Coroutine* coro = coro_create(yielding_entry, NULL, 0);
    
    // Resume 5 times
    for (int i = 0; i < 5; i++) {
        coro_switch(main, coro);
        assert(yield_count == i + 1);
    }
    
    // Final resume to complete
    coro_switch(main, coro);
    assert(coro_is_finished(coro));
    
    coro_destroy(coro);
    coro_thread_cleanup();
}

// Test 4: Multiple coroutines
static int multi_sum = 0;

static void adder_entry(void* ctx) {
    int value = *(int*)ctx;
    multi_sum += value;
}

TEST(multiple_coroutines) {
    multi_sum = 0;
    
    coro_thread_init();
    Coroutine* main = coro_get_main();
    
    int values[3] = {10, 20, 30};
    Coroutine* coros[3];
    
    // Create 3 coroutines
    for (int i = 0; i < 3; i++) {
        coros[i] = coro_create(adder_entry, &values[i], 0);
        assert(coros[i] != NULL);
    }
    
    // Run them
    for (int i = 0; i < 3; i++) {
        coro_switch(main, coros[i]);
        assert(coro_is_finished(coros[i]));
    }
    
    assert(multi_sum == 60);
    
    // Cleanup
    for (int i = 0; i < 3; i++) {
        coro_destroy(coros[i]);
    }
    
    coro_thread_cleanup();
}

// Test 5: Nested coroutine calls
static int nested_depth = 0;

static void leaf_entry(void* ctx) {
    nested_depth = 3;
}

static void middle_entry(void* ctx) {
    nested_depth = 2;
    Coroutine* current = coro_current();
    Coroutine* leaf = coro_create(leaf_entry, NULL, 0);
    coro_switch(current, leaf);
    assert(nested_depth == 3);
    coro_destroy(leaf);
}

static void root_entry(void* ctx) {
    nested_depth = 1;
    Coroutine* current = coro_current();
    Coroutine* middle = coro_create(middle_entry, NULL, 0);
    coro_switch(current, middle);
    assert(nested_depth == 3);
    coro_destroy(middle);
}

TEST(nested_coroutines) {
    nested_depth = 0;
    
    coro_thread_init();
    Coroutine* main = coro_get_main();
    
    Coroutine* root = coro_create(root_entry, NULL, 0);
    coro_switch(main, root);
    
    assert(nested_depth == 3);
    coro_destroy(root);
    
    coro_thread_cleanup();
}

// Test 6: Stack size validation
TEST(stack_sizes) {
    coro_thread_init();
    
    // Default size
    Coroutine* c1 = coro_create(NULL, NULL, 0);
    assert(c1 != NULL);
    coro_destroy(c1);
    
    // Explicit size
    Coroutine* c2 = coro_create(NULL, NULL, 128 * 1024);
    assert(c2 != NULL);
    coro_destroy(c2);
    
    // Too small (should be clamped to minimum)
    Coroutine* c3 = coro_create(NULL, NULL, 1024);
    assert(c3 != NULL);
    coro_destroy(c3);
    
    coro_thread_cleanup();
}

// Test 7: Statistics
TEST(statistics) {
    CoroutineStats stats_before = coro_get_stats();
    
    coro_thread_init();
    
    Coroutine* c1 = coro_create(NULL, NULL, 64 * 1024);
    Coroutine* c2 = coro_create(NULL, NULL, 64 * 1024);
    
    CoroutineStats stats_after = coro_get_stats();
    
    // Should have 2 more created (plus 1 for main = 3 total)
    assert(stats_after.total_created >= stats_before.total_created + 3);
    assert(stats_after.current_alive >= 3);
    
    coro_destroy(c1);
    coro_destroy(c2);
    coro_thread_cleanup();
}

// Test 8: Parent-child relationships
TEST(parent_child) {
    coro_thread_init();
    Coroutine* main = coro_get_main();
    
    Coroutine* child = coro_create(NULL, NULL, 0);
    assert(coro_get_parent(child) == main);
    
    coro_destroy(child);
    coro_thread_cleanup();
}

// --- Test Runner ---

int main(void) {
    printf("=================================\n");
    printf("Coroutine Test Suite\n");
    printf("=================================\n\n");
    
    run_test_basic_create_destroy();
    run_test_simple_execution();
    run_test_yielding();
    run_test_multiple_coroutines();
    run_test_nested_coroutines();
    run_test_stack_sizes();
    run_test_statistics();
    run_test_parent_child();
    
    printf("\n=================================\n");
    printf("Results: %d/%d tests passed\n", test_passed, test_count);
    printf("=================================\n");
    
    if (test_passed == test_count) {
        printf("\nALL TESTS PASSED!\n");
        coro_print_stats();
        return 0;
    } else {
        printf("\nSOME TESTS FAILED!\n");
        return 1;
    }
}
