/*
 * Casprix ML Runtime — Work-Stealing Task Scheduler
 *
 * ════════════════════════════════════════════════════════════════════
 * CONCURRENCY ARCHITECTURE
 * ════════════════════════════════════════════════════════════════════
 *
 * Model: N worker threads (N = physical core count by default).
 * Each thread has a private double-ended queue (deque) of tasks.
 * The thread pops from the bottom (LIFO — good cache locality).
 * Thieves steal from the top of other threads' deques (FIFO).
 *
 * This provides:
 *   - Cache-hot execution of fresh tasks (local LIFO pop)
 *   - Automatic load balancing via stealing
 *   - No central mutex on the hot path (lock-free Chase-Lev deque)
 *
 * Task graph execution:
 *   Operators are nodes; data dependencies are edges.
 *   A task becomes runnable when all its input edges are satisfied.
 *   Continuation counter per task: when counter hits 0, task is pushed.
 *
 * ════════════════════════════════════════════════════════════════════
 * THREAD PINNING & NUMA
 * ════════════════════════════════════════════════════════════════════
 *
 * On NUMA systems (multi-socket or big.LITTLE):
 *   - Thread i is pinned to physical core i (via sched_setaffinity).
 *   - Each thread's arenas are mmap'd with NUMA-local policy
 *     (mbind(MPOL_BIND) on Linux, VirtualAllocExNuma on Windows).
 *   - The parameter tensor is replicated per NUMA node when
 *     num_params * sizeof(f32) < L3_per_node (model fits in cache).
 *   - Otherwise parameters are interleaved (MPOL_INTERLEAVE) to
 *     distribute bandwidth load evenly.
 *
 * ════════════════════════════════════════════════════════════════════
 * COROUTINE vs OS THREAD
 * ════════════════════════════════════════════════════════════════════
 *
 * This scheduler uses OS threads (not coroutines) because:
 *   1. GEMM micro-kernels must run on separate cores simultaneously.
 *      True parallelism requires OS threads.
 *   2. SIMD instructions (AVX2/AVX-512) require the full FPU state
 *      of a real CPU context — coroutine context switches would need
 *      to save/restore 512-byte ZMM registers (expensive).
 *   3. Blocking syscalls (file I/O during data loading) are naturally
 *      handled by OS thread preemption without blocking other workers.
 *
 * Coroutines ARE used for:
 *   - Asynchronous data loading (disk → pinned memory pipeline)
 *   - Token streaming in the LLM decode loop (yield per token)
 *   These are lightweight fibers with minimal register state.
 *
 * Latency vs throughput tradeoff:
 *   - batch_size=1 inference: single-threaded decode is often faster
 *     because thread synchronization overhead exceeds parallelism gain
 *     for small tensors (GEMV, not GEMM).
 *   - batch_size≥4 or seq_len≥256: parallelism pays off.
 *   - The scheduler exposes a "hint" mechanism: operators can declare
 *     their preferred parallelism (none/row/batch/sequence) so the
 *     dispatcher can skip synchronization for tiny ops.
 *
 * ════════════════════════════════════════════════════════════════════
 * PIPELINE PARALLELISM
 * ════════════════════════════════════════════════════════════════════
 *
 * For multi-layer models, a pipeline is staged across threads:
 *
 *   Thread 0: Layer 0 forward  →  Layer 0 backward  (stale grad OK)
 *   Thread 1: Layer 1 forward  →  Layer 1 backward
 *   ...
 *   Thread N: Layer N forward  →  Layer N backward
 *
 * Micro-batch pipelining: while thread N processes micro-batch k+1,
 * thread 0 processes micro-batch k+2.  Gradient accumulation is
 * over micro-batches before an optimizer step.
 */

#ifndef CPX_SCHEDULER_H
#define CPX_SCHEDULER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════════
 * 1. TASK
 * ════════════════════════════════════════════════════════════════════ */

/* Maximum in/out dependency edges per task. */
#define SCHED_MAX_DEPS 8

typedef void (*CpxTaskFn)(void* arg, int thread_id);

typedef struct CpxTask {
    CpxTaskFn           fn;
    void*               arg;

    /* Dependency counting: task becomes runnable when dep_count == 0. */
    atomic_int          dep_count;

    /* Successors that depend on this task completing. */
    struct CpxTask*     successors[SCHED_MAX_DEPS];
    int                 num_successors;

    /* Parallelism hint: how many threads this task benefits from. */
    int                 preferred_threads;  /* 1 = serial, 0 = auto    */

    /* Intrusive linked list for free-list allocation. */
    struct CpxTask*     next_free;
} CpxTask;

/* ════════════════════════════════════════════════════════════════════
 * 2. CHASE-LEV WORK-STEALING DEQUE
 *
 * A single-producer, multiple-consumer circular deque.
 * push_bottom / pop_bottom: owner thread only (no lock).
 * steal_top: thief threads (CAS on top pointer).
 *
 * Reference: Chase & Lev, "Dynamic Circular Work-Stealing Deque", 2005.
 * ════════════════════════════════════════════════════════════════════ */

#define DEQUE_INITIAL_SIZE  256   /* must be power of 2 */

typedef struct {
    atomic_size_t   top;
    atomic_size_t   bottom;
    atomic_uintptr_t* buffer;   /* atomic array of task pointers        */
    size_t          mask;       /* buffer size - 1                      */
    _Atomic(void*)  _pad[7];    /* pad to 64 bytes to avoid false share */
} CpxDeque;

void      cpx_deque_init(CpxDeque* d);
void      cpx_deque_destroy(CpxDeque* d);
void      cpx_deque_push(CpxDeque* d, CpxTask* t);   /* owner only     */
CpxTask*  cpx_deque_pop(CpxDeque* d);                /* owner only     */
CpxTask*  cpx_deque_steal(CpxDeque* d);              /* any thread     */

/* ════════════════════════════════════════════════════════════════════
 * 3. WORKER THREAD STATE
 * ════════════════════════════════════════════════════════════════════ */

#define WORKER_STATE_PAD (64 - sizeof(CpxDeque*) - sizeof(int) \
                         - sizeof(uint64_t)*2 - sizeof(atomic_bool))

typedef struct {
    CpxDeque*       deque;
    int             id;             /* 0-based thread index            */
    uint64_t        tasks_executed;
    uint64_t        steals;
    atomic_bool     sleeping;
    /* Pad to 2× cache lines to avoid false sharing between workers.   */
    char            _pad[64];
} CpxWorker;

/* ════════════════════════════════════════════════════════════════════
 * 4. SCHEDULER
 * ════════════════════════════════════════════════════════════════════ */

typedef struct CpxScheduler {
    CpxWorker*      workers;
    int             num_workers;

    /* Futex/event for sleeping workers. */
    atomic_int      active_tasks;
    atomic_bool     shutting_down;

    /* Task pool (arena-allocated, reused across steps). */
    CpxTask*        task_pool;
    int             task_pool_size;
    CpxTask*        free_list_head;
    atomic_int      free_list_lock;   /* spinlock for task pool         */

    /* Global "latch" counter for barrier-style parallel_for. */
    atomic_int      barrier_latch;
} CpxScheduler;

/* Create/destroy scheduler.
 * num_threads: 0 = detect physical cores automatically. */
CpxScheduler* cpx_scheduler_create(int num_threads, bool pin_threads);
void          cpx_scheduler_destroy(CpxScheduler* sched);

/* Submit a single task. */
void cpx_scheduler_submit(CpxScheduler* sched, CpxTask* task);

/* Submit and wait for a task to complete (blocking). */
void cpx_scheduler_run_sync(CpxScheduler* sched, CpxTask* task);

/* ════════════════════════════════════════════════════════════════════
 * 5. PARALLEL-FOR — the primary interface for tensor operators
 *
 * Splits range [0, n) across workers, calling body(i, thread_id).
 * Grain size controls minimum work per task (avoids overhead for tiny n).
 * Blocks the calling thread until all iterations complete.
 * ════════════════════════════════════════════════════════════════════ */

typedef void (*CpxParallelForFn)(int i, void* arg, int thread_id);

void cpx_parallel_for(CpxScheduler* sched,
                       int n, int grain,
                       CpxParallelForFn fn, void* arg);

/* 2D parallel-for (splits outer dimension across threads,
 * inner dimension is executed serially per task).            */
void cpx_parallel_for2d(CpxScheduler* sched,
                          int outer, int inner, int grain,
                          CpxParallelForFn fn, void* arg);

/* ════════════════════════════════════════════════════════════════════
 * 6. PARALLEL REDUCTION
 *
 * Parallel map + tree reduce.
 * Each thread computes a partial float result; results are combined
 * with `combine(a, b)` in a binary tree pattern.
 * ════════════════════════════════════════════════════════════════════ */

typedef float (*CpxReduceFn)(int i, void* arg, int thread_id);
typedef float (*CpxCombineFn)(float a, float b);

float cpx_parallel_reduce(CpxScheduler* sched,
                            int n, int grain,
                            CpxReduceFn map_fn,
                            CpxCombineFn combine_fn,
                            float identity,
                            void* arg);

/* ════════════════════════════════════════════════════════════════════
 * 7. TASK GRAPH — explicit dependency DAG execution
 * ════════════════════════════════════════════════════════════════════ */

#define TASK_GRAPH_MAX_NODES  4096

typedef struct {
    CpxTask*        nodes[TASK_GRAPH_MAX_NODES];
    int             node_count;
    CpxScheduler*   sched;
} CpxTaskGraph;

CpxTaskGraph* cpx_task_graph_create(CpxScheduler* sched);
void          cpx_task_graph_destroy(CpxTaskGraph* g);

/* Add a task node. Returns its node index. */
int  cpx_task_graph_add(CpxTaskGraph* g, CpxTaskFn fn, void* arg);

/* Declare that node `succ` depends on node `pred`. */
void cpx_task_graph_depend(CpxTaskGraph* g, int pred, int succ);

/* Execute the entire graph.  Blocks until all nodes complete. */
void cpx_task_graph_run(CpxTaskGraph* g);

/* Reset graph for reuse (preserves node structure, clears dep counts). */
void cpx_task_graph_reset(CpxTaskGraph* g);

/* ════════════════════════════════════════════════════════════════════
 * 8. THREAD-LOCAL STORAGE HELPERS
 * ════════════════════════════════════════════════════════════════════ */

/* Get the current worker thread ID (0-based; -1 if called from
 * outside the scheduler, e.g. main thread). */
int cpx_thread_id(void);

/* Get per-thread scratch pointer (set by scheduler at init). */
void* cpx_thread_scratch(void);

/* ════════════════════════════════════════════════════════════════════
 * 9. BARRIER — synchronise all workers at a point
 * ════════════════════════════════════════════════════════════════════ */

void cpx_scheduler_barrier(CpxScheduler* sched);

#ifdef __cplusplus
}
#endif

#endif /* CPX_SCHEDULER_H */
