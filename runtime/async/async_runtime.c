#include "async_runtime.h"
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
typedef HANDLE AsyncThread;
#else
#include <pthread.h>
typedef pthread_t AsyncThread;
#endif

typedef struct AsyncJob {
    AsyncFunction function;
    void* arg;
    Future* future;
    struct AsyncRuntime* runtime;
} AsyncJob;

struct AsyncRuntime {
    WorkerPool* workers;
    EventLoop* event_loop;
    AsyncThread event_thread;
    volatile long running;
    volatile long active_tasks;
};

static long atomic_load_long(const volatile long* value) {
#ifdef _WIN32
    return InterlockedCompareExchange((volatile LONG*)value, 0, 0);
#else
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}
static void atomic_store_long(volatile long* value, long desired) {
#ifdef _WIN32
    InterlockedExchange((volatile LONG*)value, desired);
#else
    __atomic_store_n(value, desired, __ATOMIC_RELEASE);
#endif
}
static void atomic_increment_long(volatile long* value) {
#ifdef _WIN32
    InterlockedIncrement((volatile LONG*)value);
#else
    __atomic_add_fetch(value, 1, __ATOMIC_RELAXED);
#endif
}
static void atomic_decrement_long(volatile long* value) {
#ifdef _WIN32
    InterlockedDecrement((volatile LONG*)value);
#else
    __atomic_sub_fetch(value, 1, __ATOMIC_RELAXED);
#endif
}

#ifdef _WIN32
static unsigned __stdcall event_thread_main(void* arg)
#else
static void* event_thread_main(void* arg)
#endif
{
    event_loop_run(((AsyncRuntime*)arg)->event_loop);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void* execute_job(void* arg) {
    AsyncJob* job = (AsyncJob*)arg;
    void* result = job->function(job->arg);
    if (job->future) future_complete(job->future, result);
    atomic_decrement_long(&job->runtime->active_tasks);
    free(job);
    return result;
}

AsyncRuntime* async_runtime_create(size_t worker_count) {
    AsyncRuntime* runtime = (AsyncRuntime*)calloc(1, sizeof(AsyncRuntime));
    if (!runtime) return NULL;
    runtime->workers = pool_create(worker_count);
    runtime->event_loop = event_loop_create();
    if (!runtime->workers || !runtime->event_loop) {
        if (runtime->workers) pool_destroy(runtime->workers);
        if (runtime->event_loop) event_loop_destroy(runtime->event_loop);
        free(runtime); return NULL;
    }
    atomic_store_long(&runtime->running, 1);
#ifdef _WIN32
    runtime->event_thread = (HANDLE)_beginthreadex(NULL, 0, event_thread_main, runtime, 0, NULL);
    if (!runtime->event_thread) {
#else
    if (pthread_create(&runtime->event_thread, NULL, event_thread_main, runtime) != 0) {
#endif
        atomic_store_long(&runtime->running, 0);
        event_loop_destroy(runtime->event_loop);
        pool_destroy(runtime->workers);
        free(runtime); return NULL;
    }
    return runtime;
}

void async_runtime_destroy(AsyncRuntime* runtime) {
    if (!runtime) return;
    atomic_store_long(&runtime->running, 0);
    /* Workers drain accepted jobs before the event loop and its callbacks are
       released, so jobs may safely schedule final readiness updates. */
    pool_destroy(runtime->workers);
    event_loop_stop(runtime->event_loop);
#ifdef _WIN32
    WaitForSingleObject(runtime->event_thread, INFINITE);
    CloseHandle(runtime->event_thread);
#else
    pthread_join(runtime->event_thread, NULL);
#endif
    event_loop_destroy(runtime->event_loop);
    free(runtime);
}

Future* async_runtime_submit(AsyncRuntime* runtime, AsyncFunction function, void* arg) {
    if (!runtime || !function || !atomic_load_long(&runtime->running)) return NULL;
    AsyncJob* job = (AsyncJob*)calloc(1, sizeof(AsyncJob));
    Future* future = future_create();
    if (!job || !future) { free(job); future_destroy(future); return NULL; }
    job->function = function; job->arg = arg; job->future = future; job->runtime = runtime;
    atomic_increment_long(&runtime->active_tasks);
    if (!pool_submit(runtime->workers, execute_job, job)) {
        atomic_decrement_long(&runtime->active_tasks);
        future_destroy(future); free(job); return NULL;
    }
    return future;
}

bool async_runtime_submit_detached(AsyncRuntime* runtime, AsyncFunction function, void* arg) {
    if (!runtime || !function || !atomic_load_long(&runtime->running)) return false;
    AsyncJob* job = (AsyncJob*)calloc(1, sizeof(AsyncJob));
    if (!job) return false;
    job->function = function; job->arg = arg; job->runtime = runtime;
    atomic_increment_long(&runtime->active_tasks);
    if (!pool_submit(runtime->workers, execute_job, job)) {
        atomic_decrement_long(&runtime->active_tasks); free(job); return false;
    }
    return true;
}

WorkerPool* async_runtime_workers(AsyncRuntime* runtime) { return runtime ? runtime->workers : NULL; }
EventLoop* async_runtime_event_loop(AsyncRuntime* runtime) { return runtime ? runtime->event_loop : NULL; }
bool async_runtime_is_running(const AsyncRuntime* runtime) {
    return runtime && atomic_load_long(&runtime->running) != 0;
}
AsyncRuntimeStats async_runtime_get_stats(const AsyncRuntime* runtime) {
    AsyncRuntimeStats stats = {0};
    if (!runtime) return stats;
    stats.workers = pool_get_stats(runtime->workers);
    stats.active_tasks = (size_t)atomic_load_long(&runtime->active_tasks);
    stats.running = atomic_load_long(&runtime->running) != 0;
    return stats;
}
