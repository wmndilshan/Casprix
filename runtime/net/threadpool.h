#ifndef CASPRIX_NET_THREADPOOL_H
#define CASPRIX_NET_THREADPOOL_H

#include "cx_arena.h"
#include "mpsc_queue.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

typedef void (*CxTaskFn)(void* arg);
typedef struct { CxTaskFn fn; void* arg; } CxTask;

typedef struct CxDeque {
    _Atomic int64_t top;
    _Atomic int64_t bottom;
    _Atomic(CxTask**) buf;
    _Atomic size_t capacity;
} CxDeque;

typedef struct CxWorker {
    int       id;
    pthread_t thread;
    uint32_t  rng;
    CxDeque   deque;
} CxWorker;

typedef struct CxThreadPool {
    int            n_threads;
    CxArena*       arena;
    CxWorker*      workers;
    CxMpscQueue*   inject_q;
    _Atomic int    running;
    _Atomic int    pending_count;
} CxThreadPool;

CxThreadPool* cx_threadpool_create(int n_threads, CxArena* arena);
void          cx_threadpool_destroy(CxThreadPool* tp);
void          cx_threadpool_submit(CxThreadPool* tp, CxTaskFn fn, void* arg);
void          cx_threadpool_wait(CxThreadPool* tp);

#endif
