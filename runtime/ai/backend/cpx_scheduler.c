/*
 * Casprix ML Runtime — Work-Stealing Scheduler Implementation
 *
 * Thread model:
 *   - N OS threads created at cpx_scheduler_create (N = physical cores).
 *   - Each thread has its own Chase-Lev deque.
 *   - Owner: pushes/pops from own deque (bottom).
 *   - Stealers: steal from top of other workers' deques.
 *   - When deque empty: spin briefly, then sleep on condvar.
 *
 * Memory layout:
 *   - CpxWorker padded to CPX_CACHE_LINE × 2 (false-sharing prevention).
 *   - Deque buffer allocated as power-of-2; resized lazily.
 *   - Task pool: flat slab pre-allocated, free-list linked.
 */

#include "cpx_scheduler.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define sched_yield()   SwitchToThread()
#else
#  include <pthread.h>
#  include <sched.h>
#  include <unistd.h>
#  include <semaphore.h>
#endif

/* ════════════════════════════════════════════════════════════════════
 * 1. PLATFORM THREAD ABSTRACTION
 * ════════════════════════════════════════════════════════════════════ */

#if defined(_WIN32)
typedef HANDLE   PlatThread;
typedef CRITICAL_SECTION PlatMutex;
typedef CONDITION_VARIABLE PlatCond;

static void plat_mutex_init(PlatMutex* m)   { InitializeCriticalSection(m); }
static void plat_mutex_lock(PlatMutex* m)   { EnterCriticalSection(m); }
static void plat_mutex_unlock(PlatMutex* m) { LeaveCriticalSection(m); }
static void plat_mutex_destroy(PlatMutex* m){ DeleteCriticalSection(m); }
static void plat_cond_init(PlatCond* c)     { InitializeConditionVariable(c); }
static void plat_cond_wait(PlatCond* c, PlatMutex* m) {
    SleepConditionVariableCS(c, m, INFINITE);
}
static void plat_cond_signal(PlatCond* c)  { WakeConditionVariable(c); }
static void plat_cond_broadcast(PlatCond* c){ WakeAllConditionVariable(c); }
static void plat_cond_destroy(PlatCond* c)  { (void)c; }

static DWORD WINAPI thread_entry(LPVOID arg);
static PlatThread plat_thread_create(LPVOID arg) {
    return CreateThread(NULL, 0, thread_entry, arg, 0, NULL);
}
static void plat_thread_join(PlatThread t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
static void plat_thread_pin(int core_id) {
    DWORD_PTR mask = (DWORD_PTR)1 << core_id;
    SetThreadAffinityMask(GetCurrentThread(), mask);
}

#else /* POSIX */
typedef pthread_t   PlatThread;
typedef pthread_mutex_t PlatMutex;
typedef pthread_cond_t  PlatCond;

static void plat_mutex_init(PlatMutex* m)   { pthread_mutex_init(m, NULL); }
static void plat_mutex_lock(PlatMutex* m)   { pthread_mutex_lock(m); }
static void plat_mutex_unlock(PlatMutex* m) { pthread_mutex_unlock(m); }
static void plat_mutex_destroy(PlatMutex* m){ pthread_mutex_destroy(m); }
static void plat_cond_init(PlatCond* c)     { pthread_cond_init(c, NULL); }
static void plat_cond_wait(PlatCond* c, PlatMutex* m) {
    pthread_cond_wait(c, m);
}
static void plat_cond_signal(PlatCond* c)   { pthread_cond_signal(c); }
static void plat_cond_broadcast(PlatCond* c){ pthread_cond_broadcast(c); }
static void plat_cond_destroy(PlatCond* c)  { pthread_cond_destroy(c); }

static void* thread_entry(void* arg);
static PlatThread plat_thread_create(void* arg) {
    PlatThread t;
    pthread_create(&t, NULL, thread_entry, arg);
    return t;
}
static void plat_thread_join(PlatThread t)  { pthread_join(t, NULL); }
static void plat_thread_pin(int core_id) {
#  ifdef __linux__
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(core_id, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
#  endif
    (void)core_id;
}
#endif

/* ════════════════════════════════════════════════════════════════════
 * 2. CHASE-LEV DEQUE IMPLEMENTATION
 * ════════════════════════════════════════════════════════════════════ */

#define DEQUE_INIT_CAP 256  /* power of 2 */

typedef struct {
    CpxTask** buf;
    size_t    cap;   /* power of 2 */
} DequeRing;

static DequeRing* ring_alloc(size_t cap) {
    DequeRing* r = (DequeRing*)malloc(sizeof(DequeRing));
    r->buf = (CpxTask**)malloc(cap * sizeof(CpxTask*));
    r->cap = cap;
    return r;
}

/* Initialise owner deque. */
void cpx_deque_init(CpxDeque* d) {
    DequeRing* r = ring_alloc(DEQUE_INIT_CAP);
    atomic_store_explicit(&d->bottom, 0, memory_order_relaxed);
    atomic_store_explicit(&d->top,    0, memory_order_relaxed);
    atomic_store_explicit(&d->array, (uintptr_t)(void*)r,
                          memory_order_relaxed);
}

void cpx_deque_destroy(CpxDeque* d) {
    DequeRing* r = (DequeRing*)(void*)atomic_load(&d->array);
    if (r) { free(r->buf); free(r); }
}

/* Owner push (bottom). */
void cpx_deque_push(CpxDeque* d, CpxTask* task) {
    int64_t  b = atomic_load_explicit(&d->bottom, memory_order_relaxed);
    int64_t  t = atomic_load_explicit(&d->top,    memory_order_acquire);
    DequeRing* r = (DequeRing*)(void*)atomic_load_explicit(
                      &d->array, memory_order_relaxed);

    if (b - t > (int64_t)r->cap - 1) {
        /* Grow. */
        DequeRing* nr = ring_alloc(r->cap * 2);
        for (int64_t i = t; i < b; i++)
            nr->buf[i & (nr->cap - 1)] = r->buf[i & (r->cap - 1)];
        atomic_store_explicit(&d->array, (uintptr_t)(void*)nr,
                              memory_order_relaxed);
        /* Leak old ring — safe if tasks complete before shrink. */
        r = nr;
    }
    r->buf[b & (r->cap - 1)] = task;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&d->bottom, b + 1, memory_order_relaxed);
}

/* Owner pop (bottom). */
CpxTask* cpx_deque_pop(CpxDeque* d) {
    int64_t b = atomic_load_explicit(&d->bottom, memory_order_relaxed) - 1;
    DequeRing* r = (DequeRing*)(void*)atomic_load_explicit(
                      &d->array, memory_order_relaxed);
    atomic_store_explicit(&d->bottom, b, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    int64_t t = atomic_load_explicit(&d->top, memory_order_relaxed);

    if (t <= b) {
        CpxTask* task = r->buf[b & (r->cap - 1)];
        if (t == b) {
            /* Last element — race with stealers. */
            if (!atomic_compare_exchange_strong_explicit(
                    &d->top, &t, t + 1,
                    memory_order_seq_cst, memory_order_relaxed)) {
                task = NULL;
            }
            atomic_store_explicit(&d->bottom, b + 1, memory_order_relaxed);
        }
        return task;
    }
    atomic_store_explicit(&d->bottom, b + 1, memory_order_relaxed);
    return NULL;
}

/* Stealer steal (top). */
CpxTask* cpx_deque_steal(CpxDeque* d) {
    int64_t t = atomic_load_explicit(&d->top, memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    int64_t b = atomic_load_explicit(&d->bottom, memory_order_acquire);

    if (t >= b) return NULL;

    DequeRing* r = (DequeRing*)(void*)atomic_load_explicit(
                      &d->array, memory_order_consume);
    CpxTask* task = r->buf[t & (r->cap - 1)];

    if (!atomic_compare_exchange_strong_explicit(
            &d->top, &t, t + 1,
            memory_order_seq_cst, memory_order_relaxed)) {
        return NULL;   /* Lost the race — retry caller. */
    }
    return task;
}

/* ════════════════════════════════════════════════════════════════════
 * 3. SCHEDULER INTERNALS
 * ════════════════════════════════════════════════════════════════════ */

#define SPIN_COUNT   64    /* busy-spin iterations before sleep */

/* Internal runtime scheduler structure (extends public CpxScheduler). */
typedef struct {
    CpxScheduler   pub;           /* must be first — aliased by callers */

    /* Thread handles. */
    PlatThread*    threads;

    /* Shared sleep/wakeup infrastructure. */
    PlatMutex      sleep_mutex;
    PlatCond       sleep_cond;
    atomic_int     sleeping;      /* workers currently asleep          */

    /* Shutdown flag. */
    atomic_bool    shutdown;

    /* Task pool (fixed-size slab). */
    CpxTask*       task_pool;
    int            pool_cap;
    CpxTask*       free_head;     /* intrusive free list               */
    PlatMutex      pool_mutex;

    /* Thread-local scratch (per-thread 1 MiB) */
    void**         scratch;       /* [num_workers] pointers            */
    size_t         scratch_size;
} SchedInternal;

static _Thread_local int   tl_thread_id = -1;
static _Thread_local void* tl_scratch   = NULL;

int  cpx_thread_id(void)      { return tl_thread_id; }
void* cpx_thread_scratch(void){ return tl_scratch;   }

/* ════════════════════════════════════════════════════════════════════
 * 4. TASK POOL
 * ════════════════════════════════════════════════════════════════════ */

#define TASK_POOL_SIZE 4096

static void task_pool_init(SchedInternal* s) {
    s->task_pool = (CpxTask*)calloc(TASK_POOL_SIZE, sizeof(CpxTask));
    s->pool_cap  = TASK_POOL_SIZE;
    /* Link free list. */
    for (int i = 0; i < TASK_POOL_SIZE - 1; i++)
        s->task_pool[i].next_free = &s->task_pool[i+1];
    s->task_pool[TASK_POOL_SIZE-1].next_free = NULL;
    s->free_head = &s->task_pool[0];
    plat_mutex_init(&s->pool_mutex);
}

static CpxTask* task_alloc(SchedInternal* s) {
    plat_mutex_lock(&s->pool_mutex);
    CpxTask* t = s->free_head;
    if (t) s->free_head = t->next_free;
    plat_mutex_unlock(&s->pool_mutex);
    return t;
}

static void task_release(SchedInternal* s, CpxTask* t) {
    plat_mutex_lock(&s->pool_mutex);
    t->next_free = s->free_head;
    s->free_head = t;
    plat_mutex_unlock(&s->pool_mutex);
}

/* ════════════════════════════════════════════════════════════════════
 * 5. WORKER THREAD LOOP
 * ════════════════════════════════════════════════════════════════════ */

typedef struct { SchedInternal* s; int id; } WorkerArg;

static void execute_task(SchedInternal* s, CpxTask* task) {
    task->fn(task->arg);
    atomic_fetch_sub_explicit(&s->pub.active_tasks, 1,
                               memory_order_release);
    task_release(s, task);
    /* Wake any sleeping workers when tasks become available. */
    plat_cond_signal(&s->sleep_cond);
}

#if defined(_WIN32)
static DWORD WINAPI thread_entry(LPVOID arg_) {
#else
static void* thread_entry(void* arg_) {
#endif
    WorkerArg* wa = (WorkerArg*)arg_;
    SchedInternal* s  = wa->s;
    int id            = wa->id;
    free(wa);

    tl_thread_id = id;
    tl_scratch   = s->scratch[id];

    if (s->pub.pin_threads)
        plat_thread_pin(id % s->pub.workers[id].id); /* use logical id */

    CpxWorker* me = &s->pub.workers[id];

    while (true) {
        /* Try own deque first. */
        CpxTask* task = cpx_deque_pop(&me->deque);

        /* Steal from others. */
        if (!task) {
            int n = s->pub.num_workers;
            for (int v = 1; v < n && !task; v++) {
                int victim = (id + v) % n;
                task = cpx_deque_steal(&s->pub.workers[victim].deque);
                if (task) atomic_fetch_add(&s->pub.workers[id].steals, 1);
            }
        }

        if (task) {
            atomic_fetch_add(&me->tasks_executed, 1);
            execute_task(s, task);
            continue;
        }

        /* Nothing found — check shutdown. */
        if (atomic_load_explicit(&s->shutdown, memory_order_acquire))
            break;

        /* Spin before sleeping. */
        for (int spin = 0; spin < SPIN_COUNT; spin++) {
            task = cpx_deque_pop(&me->deque);
            if (task) goto got_task;
#if defined(_WIN32)
            YieldProcessor();
#else
            __asm__ __volatile__("pause");
#endif
        }
        /* Sleep. */
        plat_mutex_lock(&s->sleep_mutex);
        atomic_store(&me->sleeping, 1);
        atomic_fetch_add(&s->sleeping, 1);
        /* Re-check under lock before sleeping. */
        task = cpx_deque_pop(&me->deque);
        if (!task && !atomic_load(&s->shutdown)) {
            plat_cond_wait(&s->sleep_cond, &s->sleep_mutex);
        }
        atomic_store(&me->sleeping, 0);
        atomic_fetch_sub(&s->sleeping, 1);
        plat_mutex_unlock(&s->sleep_mutex);
        if (task) goto got_task;
        continue;
    got_task:
        atomic_fetch_add(&me->tasks_executed, 1);
        execute_task(s, task);
    }

#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

/* ════════════════════════════════════════════════════════════════════
 * 6. PUBLIC SCHEDULER API
 * ════════════════════════════════════════════════════════════════════ */

CpxScheduler* cpx_scheduler_create(int num_threads, bool pin_threads,
                                      size_t scratch_per_thread) {
    SchedInternal* s = (SchedInternal*)calloc(1, sizeof(SchedInternal));

    s->pub.num_workers  = num_threads;
    s->pub.pin_threads  = pin_threads;
    atomic_store(&s->pub.active_tasks, 0);

    s->pub.workers = (CpxWorker*)calloc(num_threads, sizeof(CpxWorker));
    s->threads     = (PlatThread*)malloc(num_threads * sizeof(PlatThread));
    s->scratch     = (void**)malloc(num_threads * sizeof(void*));
    s->scratch_size = scratch_per_thread;

    plat_mutex_init(&s->sleep_mutex);
    plat_cond_init(&s->sleep_cond);
    atomic_store(&s->sleeping, 0);
    atomic_store(&s->shutdown, false);

    task_pool_init(s);

    for (int t = 0; t < num_threads; t++) {
        CpxWorker* w = &s->pub.workers[t];
        w->id = t;
        cpx_deque_init(&w->deque);
        atomic_store(&w->sleeping, 0);
        atomic_store(&w->tasks_executed, 0);
        atomic_store(&w->steals, 0);

        s->scratch[t] = scratch_per_thread
                            ? malloc(scratch_per_thread) : NULL;
    }

    /* Start worker threads (skip thread 0 — that's the caller). */
    for (int t = 1; t < num_threads; t++) {
        WorkerArg* wa = (WorkerArg*)malloc(sizeof(WorkerArg));
        wa->s  = s;
        wa->id = t;
        s->threads[t] = plat_thread_create(wa);
    }
    /* Thread 0 = calling thread; set TLS manually. */
    tl_thread_id = 0;
    tl_scratch   = s->scratch[0];

    return &s->pub;
}

void cpx_scheduler_destroy(CpxScheduler* sched) {
    SchedInternal* s = (SchedInternal*)(void*)sched;

    atomic_store_explicit(&s->shutdown, true, memory_order_release);
    plat_cond_broadcast(&s->sleep_cond);

    for (int t = 1; t < sched->num_workers; t++)
        plat_thread_join(s->threads[t]);

    for (int t = 0; t < sched->num_workers; t++) {
        cpx_deque_destroy(&sched->workers[t].deque);
        if (s->scratch[t]) free(s->scratch[t]);
    }

    plat_mutex_destroy(&s->sleep_mutex);
    plat_cond_destroy(&s->sleep_cond);
    plat_mutex_destroy(&s->pool_mutex);

    free(s->task_pool);
    free(sched->workers);
    free(s->threads);
    free(s->scratch);
    free(s);
}

/* Submit one task to the given worker's deque (or any worker). */
static void submit_task(SchedInternal* s, int worker_id, CpxTask* task) {
    cpx_deque_push(&s->pub.workers[worker_id].deque, task);
    atomic_fetch_add_explicit(&s->pub.active_tasks, 1, memory_order_release);

    /* Wake a sleeping worker if any. */
    if (atomic_load(&s->sleeping) > 0) {
        plat_cond_signal(&s->sleep_cond);
    }
}

/* ════════════════════════════════════════════════════════════════════
 * 7. PARALLEL FOR
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    CpxParallelFn fn;
    void*         args_base;
    size_t        arg_stride;
    atomic_int*   counter;  /* counts down from N to 0 */
} ParForCtx;

typedef struct {
    ParForCtx* ctx;
    int        index;
} ParForItem;

static void par_for_kernel(void* arg_) {
    ParForItem* item = (ParForItem*)arg_;
    ParForCtx*  ctx  = item->ctx;
    void* arg = (uint8_t*)ctx->args_base + item->index * ctx->arg_stride;
    ctx->fn(arg);
    atomic_fetch_sub_explicit(ctx->counter, 1, memory_order_release);
}

void cpx_parallel_for(CpxScheduler* sched, CpxParallelFn fn,
                        void* args, size_t arg_stride, int n) {
    SchedInternal* s = (SchedInternal*)(void*)sched;

    atomic_int counter;
    atomic_store(&counter, n);

    ParForCtx ctx = { fn, args, arg_stride, &counter };

    /* Allocate item array on stack for small N, heap for large. */
    ParForItem* items = (ParForItem*)malloc(n * sizeof(ParForItem));
    CpxTask**   tasks = (CpxTask**)malloc(n * sizeof(CpxTask*));

    for (int i = 0; i < n; i++) {
        items[i].ctx   = &ctx;
        items[i].index = i;

        CpxTask* t = task_alloc(s);
        assert(t && "task pool exhausted");
        t->fn  = par_for_kernel;
        t->arg = &items[i];
        t->next_free = NULL;
        tasks[i] = t;

        int worker = i % sched->num_workers;
        submit_task(s, worker, t);
    }

    /* Thread 0 helps by draining its own deque while waiting. */
    while (atomic_load_explicit(&counter, memory_order_acquire) > 0) {
        CpxTask* t = cpx_deque_pop(&sched->workers[0].deque);
        if (t) {
            t->fn(t->arg);
            atomic_fetch_sub_explicit(&s->pub.active_tasks, 1,
                                       memory_order_release);
            task_release(s, t);
        } else {
#if defined(_WIN32)
            YieldProcessor();
#else
            __asm__ __volatile__("pause");
#endif
        }
    }

    free(items);
    free(tasks);
}

void cpx_parallel_for2d(CpxScheduler* sched, CpxParallelFn fn,
                          void* args, size_t arg_stride,
                          int rows, int cols) {
    cpx_parallel_for(sched, fn, args, arg_stride, rows * cols);
}

/* ════════════════════════════════════════════════════════════════════
 * 8. BARRIER
 * ════════════════════════════════════════════════════════════════════ */

void cpx_scheduler_barrier(CpxScheduler* sched) {
    SchedInternal* s = (SchedInternal*)(void*)sched;
    /* Busy-wait until all submitted tasks are done. */
    while (atomic_load_explicit(&s->pub.active_tasks,
                                 memory_order_acquire) > 0) {
        /* Help from thread 0. */
        CpxTask* t = cpx_deque_pop(&sched->workers[0].deque);
        if (!t) {
            for (int v = 1; v < sched->num_workers && !t; v++)
                t = cpx_deque_steal(&sched->workers[v].deque);
        }
        if (t) {
            t->fn(t->arg);
            atomic_fetch_sub_explicit(&s->pub.active_tasks, 1,
                                       memory_order_release);
            task_release(s, t);
        } else {
#if defined(_WIN32)
            YieldProcessor();
#else
            __asm__ __volatile__("pause");
#endif
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
 * 9. TASK GRAPH
 * ════════════════════════════════════════════════════════════════════ */

void cpx_taskgraph_init(CpxTaskGraph* g) {
    g->num_nodes = 0;
}

int cpx_taskgraph_add(CpxTaskGraph* g, CpxParallelFn fn, void* arg) {
    assert(g->num_nodes < CPX_TASKGRAPH_MAX_NODES);
    int id = g->num_nodes++;
    CpxTask* t = &g->tasks[id];
    t->fn          = fn;
    t->arg         = arg;
    t->next_free   = NULL;
    atomic_store(&t->dep_count, 0);
    t->num_successors = 0;
    return id;
}

void cpx_taskgraph_depend(CpxTaskGraph* g, int pred, int succ) {
    CpxTask* p = &g->tasks[pred];
    CpxTask* s = &g->tasks[succ];
    assert(p->num_successors < 8);
    p->successors[p->num_successors++] = s;
    atomic_fetch_add(&s->dep_count, 1);
}

typedef struct { SchedInternal* s; CpxTask* task; } GraphLaunch;
static atomic_int graph_done;

static void graph_task_wrapper(void* arg_) {
    GraphLaunch* gl = (GraphLaunch*)arg_;
    gl->task->fn(gl->task->arg);
    /* Decrement successor dep counts, submit ready ones. */
    for (int i = 0; i < gl->task->num_successors; i++) {
        CpxTask* succ = gl->task->successors[i];
        int rem = atomic_fetch_sub(&succ->dep_count, 1) - 1;
        if (rem == 0) {
            GraphLaunch* gl2 = (GraphLaunch*)malloc(sizeof(GraphLaunch));
            gl2->s    = gl->s;
            gl2->task = succ;
            CpxTask* t2 = task_alloc(gl->s);
            if (t2) {
                t2->fn  = graph_task_wrapper;
                t2->arg = gl2;
                submit_task(gl->s, tl_thread_id, t2);
            }
        }
    }
    atomic_fetch_sub(&graph_done, 1);
    free(gl);
}

void cpx_taskgraph_run(CpxTaskGraph* g, CpxScheduler* sched) {
    SchedInternal* s = (SchedInternal*)(void*)sched;
    int total = g->num_nodes;
    atomic_store(&graph_done, total);

    /* Submit all nodes with dep_count == 0. */
    for (int i = 0; i < total; i++) {
        if (atomic_load(&g->tasks[i].dep_count) == 0) {
            GraphLaunch* gl = (GraphLaunch*)malloc(sizeof(GraphLaunch));
            gl->s    = s;
            gl->task = &g->tasks[i];
            CpxTask* t = task_alloc(s);
            if (t) {
                t->fn  = graph_task_wrapper;
                t->arg = gl;
                submit_task(s, 0, t);
            }
        }
    }

    /* Wait for all to finish. */
    while (atomic_load_explicit(&graph_done, memory_order_acquire) > 0) {
        CpxTask* t = cpx_deque_pop(&sched->workers[0].deque);
        if (!t) {
            for (int v = 1; v < sched->num_workers && !t; v++)
                t = cpx_deque_steal(&sched->workers[v].deque);
        }
        if (t) {
            t->fn(t->arg);
            atomic_fetch_sub_explicit(&s->pub.active_tasks, 1,
                                       memory_order_release);
            task_release(s, t);
        } else {
#if defined(_WIN32)
            YieldProcessor();
#else
            __asm__ __volatile__("pause");
#endif
        }
    }
}

void cpx_taskgraph_reset(CpxTaskGraph* g) {
    for (int i = 0; i < g->num_nodes; i++) {
        /* dep_counts were decremented to 0; must not restore here
         * unless graph is rebuilt.  Caller should call init+add again. */
        atomic_store(&g->tasks[i].dep_count, 0);
        g->tasks[i].num_successors = 0;
    }
    g->num_nodes = 0;
}
