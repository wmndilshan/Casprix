#include "threadpool.h"

#include <sched.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

static int cx_futex_wait(_Atomic int* addr, int expected) {
#ifdef __linux__
    return (int)syscall(SYS_futex, (int*)addr, FUTEX_WAIT_PRIVATE, expected, NULL, NULL, 0);
#else
    (void)addr; (void)expected;
    usleep(1000);
    return 0;
#endif
}

static int cx_futex_wake(_Atomic int* addr, int n) {
#ifdef __linux__
    return (int)syscall(SYS_futex, (int*)addr, FUTEX_WAKE_PRIVATE, n, NULL, NULL, 0);
#else
    (void)addr; (void)n;
    return 0;
#endif
}

static uint32_t cx_rand(uint32_t* s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static int cx_deque_init(CxDeque* dq, CxArena* arena, size_t capacity) {
    memset(dq, 0, sizeof(*dq));
    CxTask** buf = (CxTask**)cx_arena_alloc_aligned(arena, capacity * sizeof(CxTask*), 64);
    if (!buf) return -1;
    memset(buf, 0, capacity * sizeof(CxTask*));
    atomic_store_explicit(&dq->top, 0, memory_order_relaxed);
    atomic_store_explicit(&dq->bottom, 0, memory_order_relaxed);
    atomic_store_explicit(&dq->buf, buf, memory_order_release);
    atomic_store_explicit(&dq->capacity, capacity, memory_order_release);
    return 0;
}

static int cx_deque_grow(CxDeque* dq, CxArena* arena) {
    size_t old_cap = atomic_load_explicit(&dq->capacity, memory_order_acquire);
    size_t new_cap = old_cap * 2u;
    CxTask** old_buf = atomic_load_explicit(&dq->buf, memory_order_acquire);
    CxTask** new_buf = (CxTask**)cx_arena_alloc_aligned(arena, new_cap * sizeof(CxTask*), 64);
    if (!new_buf) return -1;
    memset(new_buf, 0, new_cap * sizeof(CxTask*));

    int64_t t = atomic_load_explicit(&dq->top, memory_order_acquire);
    int64_t b = atomic_load_explicit(&dq->bottom, memory_order_acquire);
    for (int64_t i = t; i < b; i++) {
        new_buf[(size_t)i & (new_cap - 1u)] = old_buf[(size_t)i & (old_cap - 1u)];
    }
    atomic_store_explicit(&dq->buf, new_buf, memory_order_release);
    atomic_store_explicit(&dq->capacity, new_cap, memory_order_release);
    return 0;
}

static int cx_deque_push(CxDeque* dq, CxArena* arena, CxTask* task) {
    int64_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    int64_t t = atomic_load_explicit(&dq->top, memory_order_acquire);
    size_t cap = atomic_load_explicit(&dq->capacity, memory_order_acquire);
    if ((size_t)(b - t) >= cap - 1u) {
        if (cx_deque_grow(dq, arena) != 0) return -1;
        cap = atomic_load_explicit(&dq->capacity, memory_order_acquire);
    }
    CxTask** buf = atomic_load_explicit(&dq->buf, memory_order_acquire);
    buf[(size_t)b & (cap - 1u)] = task;
    /* release fence before publishing bottom:
       task write must be visible before another thread observes new bottom. */
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&dq->bottom, b + 1, memory_order_relaxed);
    return 0;
}

static CxTask* cx_deque_pop(CxDeque* dq) {
    int64_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed) - 1;
    atomic_store_explicit(&dq->bottom, b, memory_order_relaxed);
    /* seq_cst fence orders owner pop with concurrent steals on top. */
    atomic_thread_fence(memory_order_seq_cst);
    int64_t t = atomic_load_explicit(&dq->top, memory_order_relaxed);
    if (t <= b) {
        size_t cap = atomic_load_explicit(&dq->capacity, memory_order_acquire);
        CxTask** buf = atomic_load_explicit(&dq->buf, memory_order_acquire);
        CxTask* task = buf[(size_t)b & (cap - 1u)];
        if (t == b) {
            if (!atomic_compare_exchange_strong_explicit(
                    &dq->top, &t, t + 1, memory_order_seq_cst, memory_order_relaxed)) {
                task = NULL;
            }
            atomic_store_explicit(&dq->bottom, b + 1, memory_order_relaxed);
        }
        return task;
    }
    atomic_store_explicit(&dq->bottom, b + 1, memory_order_relaxed);
    return NULL;
}

static CxTask* cx_deque_steal(CxDeque* dq) {
    int64_t t = atomic_load_explicit(&dq->top, memory_order_acquire);
    /* seq_cst fence keeps steal in total order with owner pop fence. */
    atomic_thread_fence(memory_order_seq_cst);
    int64_t b = atomic_load_explicit(&dq->bottom, memory_order_acquire);
    if (t >= b) return NULL;
    size_t cap = atomic_load_explicit(&dq->capacity, memory_order_acquire);
    CxTask** buf = atomic_load_explicit(&dq->buf, memory_order_acquire);
    CxTask* task = buf[(size_t)t & (cap - 1u)];
    /* acquire on top ensures reading task happens after observing top state. */
    if (!atomic_compare_exchange_strong_explicit(
            &dq->top, &t, t + 1, memory_order_seq_cst, memory_order_relaxed)) {
        return NULL;
    }
    return task;
}

static void* cx_worker_main(void* arg) {
    CxThreadPool* tp = (CxThreadPool*)arg;
    int id = -1;
    pthread_t self = pthread_self();
    for (int i = 0; i < tp->n_threads; i++) {
        if (pthread_equal(tp->workers[i].thread, self)) {
            id = i;
            break;
        }
    }
    if (id < 0) return NULL;
    CxWorker* me = &tp->workers[id];

    while (atomic_load_explicit(&tp->running, memory_order_acquire)) {
        CxTask* t = (CxTask*)cx_mpsc_dequeue(tp->inject_q);
        if (t) {
            if (cx_deque_push(&me->deque, tp->arena, t) != 0) {
                t->fn(t->arg);
                atomic_fetch_sub_explicit(&tp->pending_count, 1, memory_order_acq_rel);
            }
            continue;
        }
        t = cx_deque_pop(&me->deque);
        if (!t) {
            int victim = (int)(cx_rand(&me->rng) % (uint32_t)tp->n_threads);
            if (victim != me->id) {
                t = cx_deque_steal(&tp->workers[victim].deque);
            }
        }
        if (t) {
            t->fn(t->arg);
            atomic_fetch_sub_explicit(&tp->pending_count, 1, memory_order_acq_rel);
            continue;
        }
        if (atomic_load_explicit(&tp->pending_count, memory_order_acquire) == 0) {
            cx_futex_wait(&tp->pending_count, 0);
        } else {
            sched_yield();
        }
    }
    return NULL;
}

CxThreadPool* cx_threadpool_create(int n_threads, CxArena* arena) {
    if (!arena || n_threads <= 0) return NULL;
    CxThreadPool* tp = (CxThreadPool*)cx_arena_alloc_aligned(arena, sizeof(CxThreadPool), 64);
    if (!tp) return NULL;
    memset(tp, 0, sizeof(*tp));
    tp->n_threads = n_threads;
    tp->arena = arena;
    tp->workers = (CxWorker*)cx_arena_alloc_aligned(arena, (size_t)n_threads * sizeof(CxWorker), 64);
    if (!tp->workers) return NULL;
    tp->inject_q = cx_mpsc_queue_create(arena);
    if (!tp->inject_q) return NULL;
    memset(tp->workers, 0, (size_t)n_threads * sizeof(CxWorker));
    atomic_store_explicit(&tp->running, 1, memory_order_release);
    atomic_store_explicit(&tp->pending_count, 0, memory_order_release);

    for (int i = 0; i < n_threads; i++) {
        tp->workers[i].id = i;
        tp->workers[i].rng = (uint32_t)time(NULL) ^ (uint32_t)(i * 0x9e3779b9u);
        if (cx_deque_init(&tp->workers[i].deque, arena, 1024) != 0) return NULL;
    }
    for (int i = 0; i < n_threads; i++) {
        if (pthread_create(&tp->workers[i].thread, NULL, cx_worker_main, tp) != 0) {
            atomic_store_explicit(&tp->running, 0, memory_order_release);
            return NULL;
        }
    }
    return tp;
}

void cx_threadpool_submit(CxThreadPool* tp, CxTaskFn fn, void* arg) {
    if (!tp || !fn) return;
    CxTask* task = (CxTask*)cx_arena_alloc_aligned(tp->arena, sizeof(CxTask), 64);
    if (!task) return;
    task->fn = fn;
    task->arg = arg;

    if (cx_mpsc_enqueue(tp->inject_q, task) != 0) {
        return;
    }
    atomic_fetch_add_explicit(&tp->pending_count, 1, memory_order_acq_rel);
    cx_futex_wake(&tp->pending_count, tp->n_threads);
}

void cx_threadpool_wait(CxThreadPool* tp) {
    if (!tp) return;
    while (atomic_load_explicit(&tp->pending_count, memory_order_acquire) > 0) {
        cx_futex_wait(&tp->pending_count, 0);
        sched_yield();
    }
}

void cx_threadpool_destroy(CxThreadPool* tp) {
    if (!tp) return;
    atomic_store_explicit(&tp->running, 0, memory_order_release);
    cx_futex_wake(&tp->pending_count, tp->n_threads);
    for (int i = 0; i < tp->n_threads; i++) {
        pthread_join(tp->workers[i].thread, NULL);
    }
}
