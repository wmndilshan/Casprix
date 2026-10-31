// Casprix Runtime - Work-Stealing Scheduler Implementation
// Implements a multi-threaded work-stealing task scheduler

#include "scheduler.h"
#include "../sync/lockfree_deque.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
    #include <windows.h>
    #include <process.h>
    #if defined(_MSC_VER)
        #define THREAD_LOCAL __declspec(thread)
    #else
        #define THREAD_LOCAL __thread
    #endif
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
    uint32_t rng;
    thread_t thread;
    thread_id_t thread_id;
    LockFreeDeque* local_queue;
    WorkerPool* pool;
    volatile long should_exit;
    
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
    volatile long is_running;
    
    // Statistics
    volatile uint64_t total_submitted;
    volatile uint64_t total_completed;
    
    // Random seed for victim selection
    uint32_t rand_state;
#ifdef _WIN32
    CRITICAL_SECTION global_queue_lock;
    CONDITION_VARIABLE work_ready;
#else
    pthread_mutex_t global_queue_lock;
    pthread_cond_t work_ready;
#endif
};

// --- Thread-Local Storage ---

static THREAD_LOCAL Worker* tls_current_worker = NULL;

static long flag_load(const volatile long* value) {
#ifdef _WIN32
    return InterlockedCompareExchange((volatile LONG*)value, 0, 0);
#else
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}

static void flag_store(volatile long* value, long desired) {
#ifdef _WIN32
    InterlockedExchange((volatile LONG*)value, desired);
#else
    __atomic_store_n(value, desired, __ATOMIC_RELEASE);
#endif
}

static uint64_t count_load(const volatile uint64_t* value) {
#ifdef _WIN32
    return (uint64_t)InterlockedCompareExchange64((volatile LONG64*)value, 0, 0);
#else
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}

static void count_increment(volatile uint64_t* value) {
#ifdef _WIN32
    InterlockedIncrement64((volatile LONG64*)value);
#else
    __atomic_add_fetch(value, 1, __ATOMIC_RELEASE);
#endif
}

static void global_queue_lock(WorkerPool* pool) {
#ifdef _WIN32
    EnterCriticalSection(&pool->global_queue_lock);
#else
    pthread_mutex_lock(&pool->global_queue_lock);
#endif
}

static void global_queue_unlock(WorkerPool* pool) {
#ifdef _WIN32
    LeaveCriticalSection(&pool->global_queue_lock);
#else
    pthread_mutex_unlock(&pool->global_queue_lock);
#endif
}

static void wait_for_work(Worker* worker) {
    WorkerPool* pool = worker->pool;
    global_queue_lock(pool);
    /* Wait once, then rescan every local queue through worker_get_task(). A
       loop that only checks the global queue can put a signalled worker back
       to sleep while nested work is waiting in another worker's local deque. */
    if (flag_load(&pool->is_running) &&
        !flag_load(&worker->should_exit) &&
        deque_is_empty(pool->global_queue)) {
#ifdef _WIN32
        SleepConditionVariableCS(&pool->work_ready, &pool->global_queue_lock, INFINITE);
#else
        pthread_cond_wait(&pool->work_ready, &pool->global_queue_lock);
#endif
    }
    global_queue_unlock(pool);
}

static void wake_one_worker(WorkerPool* pool) {
#ifdef _WIN32
    WakeConditionVariable(&pool->work_ready);
#else
    pthread_cond_signal(&pool->work_ready);
#endif
}

static void wake_all_workers(WorkerPool* pool) {
#ifdef _WIN32
    WakeAllConditionVariable(&pool->work_ready);
#else
    pthread_cond_broadcast(&pool->work_ready);
#endif
}

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
    uint32_t rand_val = xorshift32(&worker->rng);
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
    global_queue_lock(worker->pool);
    task = (Task*)deque_steal(worker->pool->global_queue);
    global_queue_unlock(worker->pool);
    if (task) {
        return task;
    }
    
    // 3. Try to steal from other workers
    // Probe several victims before sleeping. Condition variables provide the
    // idle backoff; sleeping here adds milliseconds of latency after wake-up.
    for (int i = 0; i < 3; i++) {
        task = worker_try_steal(worker);
        if (task) {
            return task;
        }
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
    
    while (!flag_load(&worker->should_exit)) {
        Task* task = worker_get_task(worker);
        
        if (task) {
            // Execute task
            task_execute(task);
            worker->tasks_executed++;
            count_increment(&worker->pool->total_completed);
            
            // Cleanup
            task_destroy(task);
        } else {
            wait_for_work(worker);
        }
    }
    
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
        if (num_workers > 64) {
            num_workers = 64;
        }
    }
    
    WorkerPool* pool = (WorkerPool*)calloc(1, sizeof(WorkerPool));
    if (!pool) return NULL;
    
    pool->num_workers = num_workers;
    flag_store(&pool->is_running, 1);
    pool->total_submitted = 0;
    pool->total_completed = 0;
    pool->rand_state = (uint32_t)time(NULL);
#ifdef _WIN32
    InitializeCriticalSection(&pool->global_queue_lock);
    InitializeConditionVariable(&pool->work_ready);
#else
    pthread_mutex_init(&pool->global_queue_lock, NULL);
    pthread_cond_init(&pool->work_ready, NULL);
#endif
    
    // Create global queue
    pool->global_queue = deque_create(65536);
    if (!pool->global_queue) {
#ifdef _WIN32
        DeleteCriticalSection(&pool->global_queue_lock);
#else
        pthread_cond_destroy(&pool->work_ready);
        pthread_mutex_destroy(&pool->global_queue_lock);
#endif
        free(pool);
        return NULL;
    }
    
    // Allocate workers
    pool->workers = (Worker*)calloc(num_workers, sizeof(Worker));
    if (!pool->workers) {
        deque_destroy(pool->global_queue);
#ifdef _WIN32
        DeleteCriticalSection(&pool->global_queue_lock);
#else
        pthread_cond_destroy(&pool->work_ready);
        pthread_mutex_destroy(&pool->global_queue_lock);
#endif
        free(pool);
        return NULL;
    }

    /* Allocate every queue before starting threads so partial construction
       never leaves workers running against freed pool memory. */
    for (size_t i = 0; i < num_workers; i++) {
        Worker* worker = &pool->workers[i];
        worker->id = (uint32_t)i;
        worker->rng = pool->rand_state ^ ((uint32_t)i + 1u) * 0x9e3779b9u;
        worker->pool = pool;
        flag_store(&worker->should_exit, 0);
        worker->tasks_executed = 0;
        worker->tasks_stolen = 0;
        worker->steal_attempts = 0;
        worker->steal_failures = 0;
        worker->local_queue = deque_create(1024);
        if (!worker->local_queue) {
            for (size_t j = 0; j < i; j++) {
                deque_destroy(pool->workers[j].local_queue);
            }
            free(pool->workers);
            deque_destroy(pool->global_queue);
#ifdef _WIN32
            DeleteCriticalSection(&pool->global_queue_lock);
#else
            pthread_cond_destroy(&pool->work_ready);
            pthread_mutex_destroy(&pool->global_queue_lock);
#endif
            free(pool);
            return NULL;
        }
    }

    size_t started = 0;
    for (; started < num_workers; started++) {
        Worker* worker = &pool->workers[started];
        #ifdef _WIN32
            unsigned int tid;
            worker->thread = (HANDLE)_beginthreadex(NULL, 0, worker_thread_main, 
                                                     worker, 0, &tid);
            worker->thread_id = tid;
            if (!worker->thread) break;
        #else
            if (pthread_create(&worker->thread, NULL, worker_thread_main, worker) != 0) break;
        #endif
    }
    if (started != num_workers) {
        for (size_t i = 0; i < started; i++) flag_store(&pool->workers[i].should_exit, 1);
        for (size_t i = 0; i < started; i++) {
#ifdef _WIN32
            WaitForSingleObject(pool->workers[i].thread, INFINITE);
            CloseHandle(pool->workers[i].thread);
#else
            pthread_join(pool->workers[i].thread, NULL);
#endif
        }
        for (size_t i = 0; i < num_workers; i++) {
            deque_destroy(pool->workers[i].local_queue);
        }
        free(pool->workers);
        deque_destroy(pool->global_queue);
#ifdef _WIN32
        DeleteCriticalSection(&pool->global_queue_lock);
#else
        pthread_cond_destroy(&pool->work_ready);
        pthread_mutex_destroy(&pool->global_queue_lock);
#endif
        free(pool);
        return NULL;
    }
    
    return pool;
}

void pool_destroy(WorkerPool* pool) {
    if (!pool) return;
    
    pool_shutdown(pool);
    
    // Wait for all worker threads to finish first
    for (size_t i = 0; i < pool->num_workers; i++) {
        Worker* worker = &pool->workers[i];
        #ifdef _WIN32
            if (worker->thread) {
                WaitForSingleObject(worker->thread, INFINITE);
                CloseHandle(worker->thread);
                worker->thread = NULL;
            }
        #else
            if (worker->thread) {
                pthread_join(worker->thread, NULL);
                worker->thread = 0;
            }
        #endif
    }
    
    // Once all threads have exited, destroy their queues safely
    for (size_t i = 0; i < pool->num_workers; i++) {
        Worker* worker = &pool->workers[i];
        deque_destroy(worker->local_queue);
    }
    
    // Cleanup
    free(pool->workers);
    deque_destroy(pool->global_queue);
#ifdef _WIN32
    DeleteCriticalSection(&pool->global_queue_lock);
#else
    pthread_cond_destroy(&pool->work_ready);
    pthread_mutex_destroy(&pool->global_queue_lock);
#endif
    free(pool);
    
}

bool pool_submit(WorkerPool* pool, TaskFunction func, void* arg) {
    if (!pool || !func) {
        return false;
    }
    
    Task* task = task_create(func, arg);
    if (!task) {
        return false;
    }
    
    global_queue_lock(pool);
    if (!flag_load(&pool->is_running)) {
        global_queue_unlock(pool);
        task_destroy(task);
        return false;
    }

    Worker* current = tls_current_worker;
    bool queued = false;
    if (current && current->pool == pool) {
        queued = deque_push(current->local_queue, task);
    }
    if (!queued) queued = deque_push(pool->global_queue, task);
    if (queued) {
        count_increment(&pool->total_submitted);
        global_queue_unlock(pool);
        wake_one_worker(pool);
        return true;
    }
    global_queue_unlock(pool);
    
    // Failed to submit
    task_destroy(task);
    return false;
}

Worker* pool_current_worker(void) {
    return tls_current_worker;
}

void pool_shutdown(WorkerPool* pool) {
    if (!pool) return;

    global_queue_lock(pool);
    flag_store(&pool->is_running, 0); /* Reject new work before taking snapshot. */
    global_queue_unlock(pool);
    wake_all_workers(pool);

    /* A worker cannot synchronously wait for itself. Its owner must later call
       pool_destroy from outside the pool to finish the shutdown. */
    if (tls_current_worker && tls_current_worker->pool == pool) return;

    while (count_load(&pool->total_completed) <
           count_load(&pool->total_submitted)) {
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
    }
    for (size_t i = 0; i < pool->num_workers; i++) {
        flag_store(&pool->workers[i].should_exit, 1);
    }
    wake_all_workers(pool);
}

bool pool_is_running(const WorkerPool* pool) {
    return pool ? flag_load(&pool->is_running) != 0 : false;
}

size_t pool_worker_count(const WorkerPool* pool) {
    return pool ? pool->num_workers : 0;
}

PoolStats pool_get_stats(const WorkerPool* pool) {
    PoolStats stats = {0};
    
    if (!pool) return stats;
    
    stats.num_workers = pool->num_workers;
    stats.tasks_submitted = count_load(&pool->total_submitted);
    stats.tasks_completed = count_load(&pool->total_completed);
    
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
