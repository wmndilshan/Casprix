/** Shared scheduler + non-blocking I/O runtime. */
#ifndef CASPERIX_ASYNC_RUNTIME_H
#define CASPERIX_ASYNC_RUNTIME_H

#include "future.h"
#include "scheduler.h"
#include "../io/event_loop.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AsyncRuntime AsyncRuntime;
typedef void* (*AsyncFunction)(void* arg);

typedef struct AsyncRuntimeStats {
    PoolStats workers;
    size_t active_tasks;
    bool running;
} AsyncRuntimeStats;

AsyncRuntime* async_runtime_create(size_t worker_count);
void async_runtime_destroy(AsyncRuntime* runtime);
Future* async_runtime_submit(AsyncRuntime* runtime, AsyncFunction function, void* arg);
bool async_runtime_submit_detached(AsyncRuntime* runtime, AsyncFunction function, void* arg);
WorkerPool* async_runtime_workers(AsyncRuntime* runtime);
EventLoop* async_runtime_event_loop(AsyncRuntime* runtime);
bool async_runtime_is_running(const AsyncRuntime* runtime);
AsyncRuntimeStats async_runtime_get_stats(const AsyncRuntime* runtime);

#ifdef __cplusplus
}
#endif
#endif
