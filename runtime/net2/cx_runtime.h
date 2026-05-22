#ifndef CASPRIX_NET2_RUNTIME_H
#define CASPRIX_NET2_RUNTIME_H

#include <stdatomic.h>
#include <stdint.h>

// Include project headers
#include "../net/cx_arena.h"
#include "../net/reactor.h"
#include "../net/iouring.h"
#include "../net/scheduler_bridge.h"
#include "../memory/memory.h"
#include "../async/future.h"

// Handle conflict with net/threadpool.h CxTask
#define CxTask ThreadPoolTask
#include "../net/threadpool.h"
#undef CxTask

// Forward declaration for my CxTask (defined in cx_task.h)
typedef struct CxTask CxTask;

typedef void (*TaskFunc)(void* data);

typedef struct CasprixRuntime {
    CxArena*            net_arena;
    MemoryManager*      mem;
    CxThreadPool*       threadpool;
    CxReactor*          reactor;
    CxSchedulerBridge*  bridge;
    CxIoUring*          iouring;
    int                 n_workers;
    pthread_t           reactor_thread;
    pthread_t           bridge_thread;
    _Atomic int         running;
} CasprixRuntime;

CasprixRuntime* cx_runtime_create(int n_workers);
void            cx_runtime_destroy(CasprixRuntime* rt);
Future*         cx_runtime_spawn(CasprixRuntime* rt, TaskFunc entry, void* arg, const char* name);
void            cx_runtime_run(CasprixRuntime* rt);
void*           cx_runtime_await(CasprixRuntime* rt, Future* f);

#endif
