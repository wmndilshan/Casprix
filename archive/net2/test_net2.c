#include "cx_runtime.h"
#include "cx_task.h"
#include "cx_io.h"
#include "../async/coroutine.h"
#include "../async/future.h"
#include "../memory/memory.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

// Forward declarations for ARC test
void* cx_task_arc_alloc(MemoryManager* mm, TaskFunc entry, void* data, const char* name);

// --- TEST 1: Coroutine switch ---
static int g_test1_seq = 0;
static void test1_coro_fn(void* arg) {
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < 2; i++) {
        assert(coro_current() != NULL);
        g_test1_seq++;
        coro_yield();
    }
}

void test1_coro_switch() {
    printf("Running TEST 1: coroutine switch...\n");
    coro_thread_init();
    g_test1_seq = 0;
    
    Coroutine* c1 = coro_create(test1_coro_fn, (void*)1, 0);
    Coroutine* c2 = coro_create(test1_coro_fn, (void*)2, 0);
    Coroutine* c3 = coro_create(test1_coro_fn, (void*)3, 0);
    
    while (!coro_is_finished(c1) || !coro_is_finished(c2) || !coro_is_finished(c3)) {
        if (!coro_is_finished(c1)) coro_switch(coro_current(), c1);
        if (!coro_is_finished(c2)) coro_switch(coro_current(), c2);
        if (!coro_is_finished(c3)) coro_switch(coro_current(), c3);
    }
    
    assert(g_test1_seq == 6);
    coro_destroy(c1);
    coro_destroy(c2);
    coro_destroy(c3);
    printf("TEST 1 passed.\n");
}

// --- TEST 2: Runtime create/destroy ---
void test2_runtime_lifecycle() {
    printf("Running TEST 2: runtime create/destroy...\n");
    CasprixRuntime* rt = cx_runtime_create(2);
    assert(rt != NULL);
    assert(rt->net_arena != NULL);
    assert(rt->mem != NULL);
    assert(rt->threadpool != NULL);
    assert(rt->reactor != NULL);
    
    // Check ARC stats if possible
    MemoryStats stats = mem_get_stats(rt->mem);
    // Initial objects might be > 0 due to internal setup (e.g. cycle collector)
    
    cx_runtime_destroy(rt);
    printf("TEST 2 passed.\n");
}

// --- TEST 3: Single async task ---
static void test3_task_fn(void* arg) {
    *(int*)arg = 42;
}

void test3_single_task() {
    printf("Running TEST 3: single async task...\n");
    CasprixRuntime* rt = cx_runtime_create(2);
    int result = 0;
    Future* f = cx_runtime_spawn(rt, test3_task_fn, &result, "test3");
    cx_runtime_await(rt, f);
    assert(result == 42);
    mem_arc_release(rt->mem, f);
    cx_runtime_destroy(rt);
    printf("TEST 3 passed.\n");
}

// --- TEST 4: Task continuation chain ---
static void test4_a(void* arg) { ((int*)arg)[0] = 1; }
static void test4_b(void* arg) { ((int*)arg)[1] = 2; }
static void test4_c(void* arg) { 
    int* vals = (int*)arg;
    assert(vals[0] + vals[1] == 3);
    vals[2] = 1; // Mark as done
}

void test4_continuation() {
    printf("Running TEST 4: task continuation chain...\n");
    CasprixRuntime* rt = cx_runtime_create(2);
    int vals[3] = {0, 0, 0};
    
    CxTask* ta = cx_task_create(rt->net_arena, rt->mem, test4_a, vals, "A");
    CxTask* tb = cx_task_create(rt->net_arena, rt->mem, test4_b, vals, "B");
    CxTask* tc = cx_task_create(rt->net_arena, rt->mem, test4_c, vals, "C");
    
    ta->rt = rt; tb->rt = rt; tc->rt = rt;
    ta->continuation = tb;
    tb->continuation = tc;
    
    // Submit A
    void cx_task_execute_trampoline(void* arg);
    cx_threadpool_submit(rt->threadpool, cx_task_execute_trampoline, ta);
    
    // Wait for C's future
    cx_runtime_await(rt, tc->result);
    assert(vals[2] == 1);
    
    cx_runtime_destroy(rt);
    printf("TEST 4 passed.\n");
}

// --- TEST 5: I/O suspension and resume ---
static void test5_task_fn(void* arg) {
    CasprixRuntime* rt = (CasprixRuntime*)arg;
    int pipefd[2];
    pipe(pipefd);
    
    // In a real scenario, the task would be passed the fd.
    // For this test, we'll use a global or pass it via arg.
}

// Global for test 5
static int g_test5_pipe[2];
static char g_test5_buf[10];

static void test5_reader_task(void* arg) {
    CasprixRuntime* rt = (CasprixRuntime*)arg;
    CxTask* task = tls_current_task;
    
    // This will suspend until data is available
    Future* f = cx_io_read_async(rt, task, g_test5_pipe[0], NULL, 0);
    future_wait(f);
    
    read(g_test5_pipe[0], g_test5_buf, 5);
    mem_arc_release(rt->mem, f);
}

void test5_io_resume() {
    printf("Running TEST 5: I/O suspension and resume...\n");
    CasprixRuntime* rt = cx_runtime_create(2);
    pipe(g_test5_pipe);
    fcntl(g_test5_pipe[0], F_SETFL, O_NONBLOCK);
    
    Future* f = cx_runtime_spawn(rt, test5_reader_task, rt, "reader");
    
    // Give some time for task to suspend (or just wait a bit)
    usleep(100000); 
    
    write(g_test5_pipe[1], "hello", 5);
    
    cx_runtime_await(rt, f);
    assert(strncmp(g_test5_buf, "hello", 5) == 0);
    
    mem_arc_release(rt->mem, f);
    close(g_test5_pipe[0]);
    close(g_test5_pipe[1]);
    cx_runtime_destroy(rt);
    printf("TEST 5 passed.\n");
}

// --- TEST 6: ARC cycle collection ---
static void dummy_task_fn(void* arg) { (void)arg; }

void test6_arc_cycle() {
    printf("Running TEST 6: ARC cycle collection...\n");
    MemoryManager* mm = mem_init();
    
    CxTask* a = (CxTask*)cx_task_arc_alloc(mm, dummy_task_fn, NULL, "A");
    CxTask* b = (CxTask*)cx_task_arc_alloc(mm, dummy_task_fn, NULL, "B");
    
    a->continuation = b;
    mem_arc_retain(mm, b); // b is now retained by a
    
    b->continuation = a;
    mem_arc_retain(mm, a); // a is now retained by b
    
    // Release our local handles
    mem_arc_release(mm, a);
    mem_arc_release(mm, b);
    
    // Should still be alive due to cycle
    MemoryStats stats = mem_get_stats(mm);
    assert(stats.arc.current_objects >= 2);
    
    mem_force_collect_cycles(mm);
    
    stats = mem_get_stats(mm);
    // Ideally 0, but there might be other objects. 
    // We expect the 2 tasks to be gone.
    // Since we just created mm, it should be 0 if everything works.
    assert(stats.arc.current_objects == 0);
    
    mem_shutdown(mm);
    printf("TEST 6 passed.\n");
}

int main() {
    test1_coro_switch();
    test2_runtime_lifecycle();
    test3_single_task();
    test4_continuation();
    test5_io_resume();
    test6_arc_cycle();
    
    printf("\nALL TESTS PASSED!\n");
    return 0;
}
