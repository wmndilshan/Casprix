// Casperix Runtime - Work-Stealing Scheduler Implementation
// Implements a multi-threaded work-stealing task scheduler

#include "scheduler.h"
#include "coroutine.h"
#include "../sync/lockfree_deque.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
    #include <windows.h>
    #include <process.h>
    #define THREAD_LOCAL __declspec(thread)
    typedef HANDLE thread_t;
    typedef DWORD thread_id_t;
#else
    #include <pthread.h>
    #include <unistd.h>
    #define THREAD_LOCAL __thread
    typedef pthread_t thread_t;
    typedef pthread_t thread_id_t;
#endif

// --- Task Structure ---

typedef struct Task {
    TaskFunction func;
    void* arg;
    void* result;
    bool is_complete;
} Task;

// --- Worker Structure ---

struct Worker {
    uint32_t id;
    thread_t thread;
    thread_id_t thread_id;
    LockFreeDeque* local_queue;
    WorkerPool* pool;
    volatile bool should_exit;
    
    // Statistics
    uint64_t tasks_executed;
    uint64_t tasks_stolen;
    uint64_t steal_attempts;
    uint64_t steal_failures;
    
    // Padding to prevent false sharing
    char padding[64];
};

// --- Worker Pool Structure ---

struct WorkerPool {
    Worker* workers;
    size_t num_workers;
    LockFreeDeque* global_queue;  // Injector queue
    volatile bool is_running;
    
    // Statistics
    uint64_t total_submitted;
    uint64_t total_completed;
    
    // Random seed for victim selection
    uint32_t rand_state;
};

// --- Thread-Local Storage ---

static THREAD_LOCAL Worker* tls_current_worker = NULL;

// --- Task Management ---

static Task* task_create(TaskFunction func, void* arg) {
    Task* task = (Task*)malloc(sizeof(Task));
    if (!task) return NULL;
    
    task->func = func;
    task->arg = arg;
    task->result = NULL;
    task->is_complete = false;
    
    return task;
}

static void task_execute(Task* task) {
    if (!task || !task->func) return;
    
    task->result = task->func(task->arg);
    task->is_complete = true;
}

static void task_destroy(Task* task) {
    if (task) {
        free(task);
    }
}

// --- Random Number Generator (for victim selection) ---

static uint32_t xorshift32(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

// --- Work-Stealing Logic ---

static Task* worker_try_steal(Worker* worker) {
    WorkerPool* pool = worker->pool;
    size_t num_workers = pool->num_workers;
    
    if (num_workers <= 1) {
        return NULL; // No one to steal from
    }
    
    // Random victim selection
    uint32_t rand_val = xorshift32(&pool->rand_state);
    uint32_t victim_id = rand_val % num_workers;
    
    // Don't steal from ourselves
    if (victim_id == worker->id) {
        victim_id = (victim_id + 1) % num_workers;
    }
    
    Worker* victim = &pool->workers[victim_id];
    
    worker->steal_attempts++;
    Task* task = (Task*)deque_steal(victim->local_queue);
    
    if (task) {
        worker->tasks_stolen++;
    } else {
        worker->steal_failures++;
    }
    
    return task;
}

static Task* worker_get_task(Worker* worker) {
    // 1. Try to pop from local queue (LIFO for cache locality)
    Task* task = (Task*)deque_pop(worker->local_queue);
    if (task) {
        return task;
    }
    
    // 2. Try to steal from global queue
    task = (Task*)deque_steal(worker->pool->global_queue);
    if (task) {
        return task;
    }
    
    // 3. Try to steal from other workers
    // Try multiple times with exponential backoff
    for (int i = 0; i < 3; i++) {
        task = worker_try_steal(worker);
        if (task) {
            return task;
        }
        
        // Exponential backoff
        #ifdef _WIN32
            Sleep(1 << i);  // 1ms, 2ms, 4ms
        #else
            usleep((1 << i) * 1000);
        #endif
    }
    
    return NULL;
}

// --- Worker Thread Main Loop ---

#ifdef _WIN32
static unsigned __stdcall worker_thread_main(void* arg) {
#else
static void* worker_thread_main(void* arg) {
#endif
    Worker* worker = (Worker*)arg;
    tls_current_worker = worker;
    
    // Initialize coroutine system for this worker thread
    coro_thread_init();
    
    printf("Worker %u started (thread ID: %lu)\n", worker->id, 
           (unsigned long)worker->thread_id);
    
    while (!worker->should_exit) {
        Task* task = worker_get_task(worker);
        
        if (task) {
            // Execute task
            task_execute(task);
            worker->tasks_executed++;
            worker->pool->total_completed++;
            
            // Cleanup
            task_destroy(task);
        } else {
            // No work available, sleep briefly
            #ifdef _WIN32
                Sleep(10);
            #else
                usleep(10000);
            #endif
        }
    }
    
    printf("Worker %u stopping\n", worker->id);
    
    // Cleanup coroutine system
    coro_thread_cleanup();
    
    #ifdef _WIN32
        return 0;
    #else
        return NULL;
    #endif
}

// --- Worker Pool Implementation ---

WorkerPool* pool_create(size_t num_workers) {
    if (num_workers == 0) {
        // Auto-detect CPU count
        #ifdef _WIN32
            SYSTEM_INFO sysinfo;
            GetSystemInfo(&sysinfo);
            num_workers = sysinfo.dwNumberOfProcessors;
        #else
            num_workers = sysconf(_SC_NPROCESSORS_ONLN);
        #endif
        
        if (num_workers < 1) {
            num_workers = 1;
        }
    }
    
    WorkerPool* pool = (WorkerPool*)calloc(1, sizeof(WorkerPool));
    if (!pool) return NULL;
    
    pool->num_workers = num_workers;
    pool->is_running = true;
    pool->total_submitted = 0;
    pool->total_completed = 0;
    pool->rand_state = (uint32_t)time(NULL);
    
    // Create global queue
    pool->global_queue = deque_create(256);
    if (!pool->global_queue) {
        free(pool);
        return NULL;
    }
    
    // Allocate workers
    pool->workers = (Worker*)calloc(num_workers, sizeof(Worker));
    if (!pool->workers) {
        deque_destroy(pool->global_queue);
        free(pool);
        return NULL;
    }
    
    // Initialize and start workers
    for (size_t i = 0; i < num_workers; i++) {
        Worker* worker = &pool->workers[i];
        worker->id = (uint32_t)i;
        worker->pool = pool;
        worker->should_exit = false;
        worker->tasks_executed = 0;
        worker->tasks_stolen = 0;
        worker->steal_attempts = 0;
        worker->steal_failures = 0;
        
        // Create local queue for this worker
        worker->local_queue = deque_create(256);
        if (!worker->local_queue) {
            // Cleanup and fail
            for (size_t j = 0; j < i; j++) {
                deque_destroy(pool->workers[j].local_queue);
            }
            free(pool->workers);
            deque_destroy(pool->global_queue);
            free(pool);
            return NULL;
        }
        
        // Start worker thread
        #ifdef _WIN32
            unsigned int tid;
            worker->thread = (HANDLE)_beginthreadex(NULL, 0, worker_thread_main, 
                                                     worker, 0, &tid);
            worker->thread_id = tid;
            if (!worker->thread) {
        #else
            if (pthread_create(&worker->thread, NULL, worker_thread_main, worker) != 0) {
        #endif
                // Failed to create thread
                for (size_t j = 0; j <= i; j++) {
                    deque_destroy(pool->workers[j].local_queue);
                }
                free(pool->workers);
                deque_destroy(pool->global_queue);
                free(pool);
                return NULL;
            }
    }
    
    printf("Worker pool created with %llu workers\n", (unsigned long long)num_workers);
    return pool;
}

void pool_destroy(WorkerPool* pool) {
    if (!pool) return;
    
    printf("Shutting down worker pool...\n");
    
    // Signal all workers to exit
    pool_shutdown(pool);
    
    // Wait for all workers to finish
    for (size_t i = 0; i < pool->num_workers; i++) {
        Worker* worker = &pool->workers[i];
        
        #ifdef _WIN32
            WaitForSingleObject(worker->thread, INFINITE);
            CloseHandle(worker->thread);
        #else
            pthread_join(worker->thread, NULL);
        #endif
        
        deque_destroy(worker->local_queue);
    }
    
    // Cleanup
    free(pool->workers);
    deque_destroy(pool->global_queue);
    free(pool);
    
    printf("Worker pool destroyed\n");
}

bool pool_submit(WorkerPool* pool, TaskFunction func, void* arg) {
    if (!pool || !pool->is_running || !func) {
        return false;
    }
    
    Task* task = task_create(func, arg);
    if (!task) {
        return false;
    }
    
    // Try to push to current worker's queue if we're in a worker thread
    Worker* current = tls_current_worker;
    if (current && current->pool == pool) {
        if (deque_push(current->local_queue, task)) {
            pool->total_submitted++;
            return true;
        }
    }
    
    // Otherwise, push to global queue
    if (deque_push(pool->global_queue, task)) {
        pool->total_submitted++;
        return true;
    }
    
    // Failed to submit
    task_destroy(task);
    return false;
}

Worker* pool_current_worker(void) {
    return tls_current_worker;
}

void pool_shutdown(WorkerPool* pool) {
    if (!pool) return;
    
    pool->is_running = false;
    
    for (size_t i = 0; i < pool->num_workers; i++) {
        pool->workers[i].should_exit = true;
    }
}

bool pool_is_running(const WorkerPool* pool) {
    return pool ? pool->is_running : false;
}

size_t pool_worker_count(const WorkerPool* pool) {
    return pool ? pool->num_workers : 0;
}

PoolStats pool_get_stats(const WorkerPool* pool) {
    PoolStats stats = {0};
    
    if (!pool) return stats;
    
    stats.num_workers = pool->num_workers;
    stats.tasks_submitted = pool->total_submitted;
    stats.tasks_completed = pool->total_completed;
    
    for (size_t i = 0; i < pool->num_workers; i++) {
        stats.tasks_stolen += pool->workers[i].tasks_stolen;
        stats.steal_attempts += pool->workers[i].steal_attempts;
        stats.steal_failures += pool->workers[i].steal_failures;
    }
    
    return stats;
}

void pool_print_stats(const WorkerPool* pool) {
    if (!pool) return;
    
    PoolStats stats = pool_get_stats(pool);
    
    printf("\n=== Worker Pool Statistics ===\n");
    printf("Workers:          %llu\n", (unsigned long long)stats.num_workers);
    printf("Tasks submitted:  %llu\n", (unsigned long long)stats.tasks_submitted);
    printf("Tasks completed:  %llu\n", (unsigned long long)stats.tasks_completed);
    printf("Tasks stolen:     %llu\n", (unsigned long long)stats.tasks_stolen);
    printf("Steal attempts:   %llu\n", (unsigned long long)stats.steal_attempts);
    printf("Steal failures:   %llu\n", (unsigned long long)stats.steal_failures);
    
    if (stats.steal_attempts > 0) {
        double success_rate = (double)stats.tasks_stolen / stats.steal_attempts * 100.0;
        printf("Steal success:    %.1f%%\n", success_rate);
    }
    
    printf("\nPer-Worker Statistics:\n");
    for (size_t i = 0; i < pool->num_workers; i++) {
        Worker* w = &pool->workers[i];
        printf("  Worker %u: executed=%llu stolen=%llu attempts=%llu\n",
               w->id,
               (unsigned long long)w->tasks_executed,
               (unsigned long long)w->tasks_stolen,
               (unsigned long long)w->steal_attempts);
    }
    printf("==============================\n");
}
