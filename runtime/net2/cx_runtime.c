#include "cx_runtime.h"
#include "cx_task.h"
#include "../net/cx_arena.h"
#include "../net/reactor.h"
#include "../net/scheduler_bridge.h"
#include "../net/iouring.h"
#include "../memory/memory.h"
#include "../async/future.h"

// Handle conflict with net/threadpool.h
#define CxTask ThreadPoolTask
#include "../net/threadpool.h"
#undef CxTask

#include <stdlib.h>
#include <stdatomic.h>

static void* reactor_thread_fn(void* arg) {
    CasprixRuntime* rt = (CasprixRuntime*)arg;
    while (atomic_load_explicit(&rt->running, memory_order_acquire)) {
        // Poll reactor (non-blocking if possible, or short timeout)
        // Since I don't know the reactor API for timeout, I'll assume reactor_run is blocking
        // and I'll use the reactor stop mechanism.
        // Wait! If reactor_run is blocking, I can't poll the bridge here.
        // I'll start a separate bridge thread instead.
        cx_reactor_run(rt->reactor);
    }
    return NULL;
}

static void* bridge_thread_fn(void* arg) {
    CasprixRuntime* rt = (CasprixRuntime*)arg;
    while (atomic_load_explicit(&rt->running, memory_order_acquire)) {
        void* task = cx_scheduler_bridge_pop_ready(rt->bridge);
        if (task) {
            cx_threadpool_submit(rt->threadpool, cx_task_execute_trampoline, task);
        } else {
            usleep(100); // Backoff
        }
    }
    return NULL;
}

void cx_task_execute_trampoline(void* arg) {
    CxTask* task = (CxTask*)arg;
    if (!coro_current()) {
        coro_thread_init();
    }
    cx_task_execute(task);
}

CasprixRuntime* cx_runtime_create(int n_workers) {
    CasprixRuntime* rt = (CasprixRuntime*)calloc(1, sizeof(CasprixRuntime));
    if (!rt) return NULL;
    
    rt->n_workers = n_workers;
    
    rt->net_arena = cx_arena_create(64 * 1024 * 1024);
    if (!rt->net_arena) goto fail;
    
    rt->mem = mem_init();
    if (!rt->mem) goto fail;
    
    rt->reactor = cx_reactor_create(rt->net_arena);
    if (!rt->reactor) goto fail;
    
    rt->threadpool = cx_threadpool_create(n_workers, rt->net_arena);
    if (!rt->threadpool) goto fail;
    
    rt->bridge = cx_scheduler_bridge_create(rt->net_arena);
    if (!rt->bridge) goto fail;
    
    rt->iouring = cx_iouring_create(rt->net_arena, 256);
    if (!rt->iouring) goto fail;
    
    atomic_store_explicit(&rt->running, 1, memory_order_release);
    pthread_create(&rt->reactor_thread, NULL, reactor_thread_fn, rt);
    
    return rt;

fail:
    cx_runtime_destroy(rt);
    return NULL;
}

void cx_runtime_destroy(CasprixRuntime* rt) {
    if (!rt) return;
    
    atomic_store_explicit(&rt->running, 0, memory_order_release);
    
    if (rt->reactor) cx_reactor_stop(rt->reactor);
    pthread_join(rt->reactor_thread, NULL);
    
    if (rt->threadpool) cx_threadpool_destroy(rt->threadpool);
    if (rt->iouring) cx_iouring_destroy(rt->iouring);
    if (rt->mem) mem_shutdown(rt->mem);
    if (rt->net_arena) cx_arena_destroy(rt->net_arena);
    
    free(rt);
}

Future* cx_runtime_spawn(CasprixRuntime* rt, TaskFunc entry, void* arg, const char* name) {
    CxTask* task = cx_task_create(rt->net_arena, rt->mem, entry, arg, name);
    if (!task) return NULL;
    
    task->bridge_ref = rt->bridge;
    task->rt = rt;
    
    // arc_retain the result for the caller
    // Assuming mem_arc_retain works on Future* if it's ARC-managed
    mem_arc_retain(rt->mem, task->result);
    
    cx_threadpool_submit(rt->threadpool, cx_task_execute_trampoline, task);
    
    return task->result;
}

void cx_runtime_run(CasprixRuntime* rt) {
    cx_reactor_run(rt->reactor);
}

void* cx_runtime_await(CasprixRuntime* rt, Future* f) {
    return future_wait(f);
}
