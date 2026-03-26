// Casprix Async Runtime Library
// Provides Promise/Task implementation for async operations

#ifndef NUWAN_ASYNC_H
#define NUWAN_ASYNC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Promise states
typedef enum {
    PROMISE_PENDING = 0,
    PROMISE_RESOLVED = 1,
    PROMISE_REJECTED = 2
} nuwan_promise_state_t;

// Task states
typedef enum {
    TASK_PENDING = 0,
    TASK_RUNNING = 1,
    TASK_COMPLETED = 2,
    TASK_FAILED = 3
} nuwan_task_state_t;

// Forward declarations
typedef struct nuwan_promise nuwan_promise_t;
typedef struct nuwan_task nuwan_task_t;
typedef struct nuwan_task_scheduler nuwan_task_scheduler_t;
typedef struct nuwan_thread_pool nuwan_thread_pool_t;

// Callback types
typedef void (*nuwan_callback_t)(void* arg);
typedef void* (*nuwan_task_func_t)(void* arg);
typedef void (*nuwan_promise_callback_t)(void* result);

// Promise structure
struct nuwan_promise {
    nuwan_promise_state_t state;
    void* result;
    int64_t error_code;
    nuwan_promise_callback_t on_resolve;
    nuwan_promise_callback_t on_reject;
    void* user_data;
};

// Task structure
struct nuwan_task {
    int64_t handle;
    nuwan_task_state_t state;
    void* result;
    char* error_message;
    nuwan_task_func_t work_function;
    void* work_arg;
    void* thread_handle;
};

// Task scheduler
struct nuwan_task_scheduler {
    int64_t handle;
    int max_threads;
    int active_threads;
    int queue_size;
    void* task_queue;
    void* worker_threads;
    bool is_shutdown;
};

// Thread pool
struct nuwan_thread_pool {
    int64_t handle;
    int pool_size;
    void* workers;
    void* task_queue;
    bool is_shutdown;
};

// Promise API
nuwan_promise_t* nuwan_promise_create(void);
void nuwan_promise_resolve(nuwan_promise_t* promise, void* result);
void nuwan_promise_reject(nuwan_promise_t* promise, int64_t error_code);
void nuwan_promise_then(nuwan_promise_t* promise, nuwan_promise_callback_t callback);
void nuwan_promise_catch(nuwan_promise_t* promise, nuwan_promise_callback_t callback);
bool nuwan_promise_is_resolved(nuwan_promise_t* promise);
void* nuwan_promise_get_result(nuwan_promise_t* promise);
void nuwan_promise_destroy(nuwan_promise_t* promise);

// Task API
nuwan_task_t* nuwan_task_create(nuwan_task_func_t work_func, void* arg);
void* nuwan_task_await(nuwan_task_t* task);
bool nuwan_task_is_completed(nuwan_task_t* task);
void* nuwan_task_get_result(nuwan_task_t* task);
const char* nuwan_task_get_error(nuwan_task_t* task);
void nuwan_task_destroy(nuwan_task_t* task);

// Task Scheduler API
nuwan_task_scheduler_t* nuwan_task_scheduler_create(int max_threads);
nuwan_task_t* nuwan_task_scheduler_schedule(nuwan_task_scheduler_t* scheduler, nuwan_task_func_t work_func, void* arg);
void nuwan_task_scheduler_shutdown(nuwan_task_scheduler_t* scheduler);
int nuwan_task_scheduler_get_active_count(nuwan_task_scheduler_t* scheduler);
void nuwan_task_scheduler_destroy(nuwan_task_scheduler_t* scheduler);

// Thread Pool API
nuwan_thread_pool_t* nuwan_thread_pool_create(int pool_size);
nuwan_task_t* nuwan_thread_pool_submit(nuwan_thread_pool_t* pool, nuwan_task_func_t work_func, void* arg);
nuwan_task_t** nuwan_thread_pool_submit_many(nuwan_thread_pool_t* pool, nuwan_task_func_t* work_funcs, void** args, int count);
void nuwan_thread_pool_await_all(nuwan_thread_pool_t* pool);
void nuwan_thread_pool_shutdown(nuwan_thread_pool_t* pool);
void nuwan_thread_pool_destroy(nuwan_thread_pool_t* pool);

// Async utilities
nuwan_task_t* nuwan_async_run(nuwan_task_func_t work_func, void* arg);
nuwan_task_t** nuwan_async_parallel(nuwan_task_func_t* work_funcs, void** args, int count);
int nuwan_async_wait_any(nuwan_task_t** tasks, int count);
void nuwan_async_wait_all(nuwan_task_t** tasks, int count);

#ifdef __cplusplus
}
#endif

#endif // NUWAN_ASYNC_H
