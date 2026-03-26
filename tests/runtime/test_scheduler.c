// Test suite for Work-Stealing Scheduler

#include "runtime/async/scheduler.h"
#include <stdio.h>
#include <stdatomic.h>
#include <assert.h>

#ifdef _WIN32
    #include <windows.h>
    #define sleep_ms(ms) Sleep(ms)
#else
    #include <unistd.h>
    #define sleep_ms(ms) usleep((ms) * 1000)
#endif

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

// Test 1: Basic pool creation and destruction
TEST(basic_pool_lifecycle) {
    WorkerPool* pool = pool_create(2);
    assert(pool != NULL);
    assert(pool_is_running(pool));
    assert(pool_worker_count(pool) == 2);
    
    pool_destroy(pool);
}

// Test 2: Simple task execution
static atomic_int simple_counter = 0;

static void* simple_task(void* arg) {
    int value = *(int*)arg;
    atomic_fetch_add(&simple_counter, value);
    return NULL;
}

TEST(simple_task_execution) {
    atomic_store(&simple_counter, 0);
    
    WorkerPool* pool = pool_create(2);
    
    int values[] = {1, 2, 3, 4, 5};
    
    for (int i = 0; i < 5; i++) {
        assert(pool_submit(pool, simple_task, &values[i]));
    }
    
    // Wait for tasks to complete
    sleep_ms(500);
    
    pool_destroy(pool);
    
    assert(atomic_load(&simple_counter) == 15);  // 1+2+3+4+5
}

// Test 3: Many tasks to test work stealing
static atomic_int many_counter = 0;

static void* counting_task(void* arg) {
    atomic_fetch_add(&many_counter, 1);
    
    // Simulate work
    volatile int x = 0;
    for (int i = 0; i < 10000; i++) {
        x += i;
    }
    
    return NULL;
}

TEST(many_tasks_work_stealing) {
    atomic_store(&many_counter, 0);
    
    WorkerPool* pool = pool_create(4);
    
    // Submit 100 tasks
    for (int i = 0; i < 100; i++) {
        assert(pool_submit(pool, counting_task, NULL));
    }
    
    // Wait for completion
    sleep_ms(2000);
    
    PoolStats stats = pool_get_stats(pool);
    pool_print_stats(pool);
    
    pool_destroy(pool);
    
    assert(atomic_load(&many_counter) == 100);
    assert(stats.tasks_completed == 100);
    
    // Some stealing should have occurred with 4 workers
    printf("Work stealing occurred: %llu tasks stolen\n", 
           (unsigned long long)stats.tasks_stolen);
}

// Test 4: Nested task submission
static atomic_int nested_counter = 0;

static void* leaf_task(void* arg) {
    atomic_fetch_add(&nested_counter, 1);
    return NULL;
}

static void* parent_task(void* arg) {
    WorkerPool* pool = (WorkerPool*)arg;
    
    // Submit child tasks
    for (int i = 0; i < 5; i++) {
        pool_submit(pool, leaf_task, NULL);
    }
    
    return NULL;
}

TEST(nested_task_submission) {
    atomic_store(&nested_counter, 0);
    
    WorkerPool* pool = pool_create(2);
    
    // Submit 10 parent tasks, each spawning 5 children = 50 total
    for (int i = 0; i < 10; i++) {
        assert(pool_submit(pool, parent_task, pool));
    }
    
    // Wait for all tasks
    sleep_ms(1000);
    
    pool_destroy(pool);
    
    assert(atomic_load(&nested_counter) == 50);
}

// Test 5: Auto CPU detection
TEST(auto_cpu_detection) {
    WorkerPool* pool = pool_create(0);  // 0 = auto-detect
    assert(pool != NULL);
    
    size_t num_workers = pool_worker_count(pool);
    printf("\nAuto-detected %llu CPU cores\n", (unsigned long long)num_workers);
    assert(num_workers >= 1);
    
    pool_destroy(pool);
}

// Test 6: Statistics tracking
static void* noop_task(void* arg) {
    return NULL;
}

TEST(statistics_tracking) {
    WorkerPool* pool = pool_create(2);
    
    // Submit 10 tasks
    for (int i = 0; i < 10; i++) {
        pool_submit(pool, noop_task, NULL);
    }
    
    sleep_ms(500);
    
    PoolStats stats = pool_get_stats(pool);
    assert(stats.num_workers == 2);
    assert(stats.tasks_submitted == 10);
    assert(stats.tasks_completed == 10);
    
    pool_destroy(pool);
}

// --- Test Runner ---

int main(void) {
    printf("=================================\n");
    printf("Work-Stealing Scheduler Tests\n");
    printf("=================================\n\n");
    
    run_test_basic_pool_lifecycle();
    run_test_simple_task_execution();
    run_test_many_tasks_work_stealing();
    run_test_nested_task_submission();
    run_test_auto_cpu_detection();
    run_test_statistics_tracking();
    
    printf("\n=================================\n");
    printf("Results: %d/%d tests passed\n", test_passed, test_count);
    printf("=================================\n");
    
    if (test_passed == test_count) {
        printf("\nALL TESTS PASSED!\n");
        return 0;
    } else {
        printf("\nSOME TESTS FAILED!\n");
        return 1;
    }
}
