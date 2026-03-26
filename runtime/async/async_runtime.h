// Casperix Runtime - Async Runtime Core
// Main async/await runtime integrating coroutines, scheduler, and event loop
//
// Architecture:
// - Task = lightweight async execution unit (built on coroutines)
// - WorkerPool = thread pool with work-stealing scheduler
// - EventLoop = I/O multiplexing (epoll/IOCP/kqueue)
// - Future = handle for awaiting async results

#ifndef CASPERIX_ASYNC_RUNTIME_H
#define CASPERIX_ASYNC_RUNTIME_H

#include "coroutine.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Task ---

typedef enum {
    TASK_STATE_PENDING,      // Created but not started
    TASK_STATE_RUNNING,      // Currently executing
    TASK_STATE_SUSPENDED,    // Awaiting I/O or child task
    TASK_STATE_READY,        // Woken up, ready to resume
    TASK_STATE_COMPLETED,    // Finished successfully
    TASK_STATE_FAILED        // Failed with error
} TaskState;

typedef struct Task Task;
typedef void* (*TaskEntry)(void* arg);

// Create a new task (does not start it yet)
Task* task_create(TaskEntry entry, void* arg);

// Get task state
TaskState task_get_state(const Task* task);

// Get task result (blocks if not complete)
void* task_get_result(Task* task);

// Destroy a task
void task_destroy(Task* task);

// --- Future ---

typedef struct Future Future;

// Check if future is ready
bool future_is_ready(const Future* future);

// Wait for future to complete and get result
void* future_await(Future* future);

// Create a future from a task
Future* future_from_task(Task* task);

// Destroy a future
void future_destroy(Future* future);

// --- Async Runtime ---

// Initialize the async runtime with specified number of worker threads
// If num_workers == 0, uses number of CPU cores
void async_runtime_init(size_t num_workers);

// Shutdown the async runtime and wait for all tasks to complete
void async_runtime_shutdown(void);

// Check if runtime is initialized
bool async_runtime_is_running(void);

// --- Task Spawning ---

// Spawn a new task on the runtime
// Returns a future that can be awaited
Future* async_spawn(TaskEntry entry, void* arg);

// Spawn a task and don't wait for result (fire-and-forget)
void async_spawn_detached(TaskEntry entry, void* arg);

// --- Async/Await ---

// Yield control back to the scheduler
// Allows other tasks to run
void async_yield(void);

// Sleep for specified milliseconds
Future* async_sleep(uint64_t milliseconds);

// Get the currently running task (NULL if not in async context)
Task* async_current_task(void);

// --- Utilities ---

// Wait for all spawned tasks to complete
void async_await_all(void);

// Get number of active tasks
size_t async_active_task_count(void);

// --- Statistics ---

typedef struct {
    size_t tasks_created;
    size_t tasks_completed;
    size_t tasks_failed;
    size_t tasks_active;
    size_t worker_threads;
    uint64_t total_switches;
} AsyncStats;

AsyncStats async_get_stats(void);
void async_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif // CASPERIX_ASYNC_RUNTIME_H
