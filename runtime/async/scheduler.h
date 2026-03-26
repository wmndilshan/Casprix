// Casperix Runtime - Work-Stealing Scheduler
// Multi-threaded task scheduler with work stealing
//
// Architecture:
// - N worker threads, each with local work-stealing deque
// - Global injector queue for external task submission
// - Random victim selection for stealing
// - Exponential backoff when idle

#ifndef CASPERIX_SCHEDULER_H
#define CASPERIX_SCHEDULER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct WorkerPool WorkerPool;
typedef struct Worker Worker;
typedef void* (*TaskFunction)(void* arg);

// --- Worker Pool API ---

// Create a worker pool with N threads
// If num_workers == 0, uses CPU core count
WorkerPool* pool_create(size_t num_workers);

// Destroy the worker pool and wait for all workers to finish
void pool_destroy(WorkerPool* pool);

// Submit a task to the pool
// Task will be assigned to a worker or placed in global queue
bool pool_submit(WorkerPool* pool, TaskFunction func, void* arg);

// Get the current worker (NULL if not in worker thread)
Worker* pool_current_worker(void);

// Get worker pool statistics
typedef struct {
    size_t num_workers;
    size_t tasks_submitted;
    size_t tasks_completed;
    size_t tasks_stolen;
    size_t steal_attempts;
    size_t steal_failures;
} PoolStats;

PoolStats pool_get_stats(const WorkerPool* pool);
void pool_print_stats(const WorkerPool* pool);

// --- Worker Control ---

// Request all workers to stop (graceful shutdown)
void pool_shutdown(WorkerPool* pool);

// Check if pool is running
bool pool_is_running(const WorkerPool* pool);

// Get number of active workers
size_t pool_worker_count(const WorkerPool* pool);

#ifdef __cplusplus
}
#endif

#endif // CASPERIX_SCHEDULER_H
