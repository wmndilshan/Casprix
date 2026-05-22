#ifndef CASPRIX_NET2_TASK_H
#define CASPRIX_NET2_TASK_H

#include "../async/task.h"
#include "../async/coroutine.h"
#include "../async/future.h"
#include "../net/cx_arena.h"
#include "../net/scheduler_bridge.h"
#include "../memory/memory.h"
#include <stdatomic.h>
#include <stdint.h>

typedef struct CxTask CxTask;
typedef struct CasprixRuntime CasprixRuntime;

struct CxTask {
    TaskFunc           entry;
    void*              data;
    _Atomic TaskState  state;
    TaskPriority       priority;
    Future*            result;        // ARC-managed
    CxTask*            continuation;  // Next task in chain
    Coroutine*         coro;          // Stackful coroutine (owned)
    CxArena*           arena;         // Arena this task was allocated from
    CasprixRuntime*    rt;            // Reference to runtime
    CxSchedulerBridge* bridge_ref;    // Set at spawn time
    uint64_t           task_id;
    const char*        name;
};

CxTask* cx_task_create(CxArena* arena, MemoryManager* mm, TaskFunc entry, void* data, const char* name);
Future* cx_future_create_arc(MemoryManager* mm);
void    cx_task_execute(CxTask* task);
void    cx_task_await_future(CxTask* task, Future* future);

extern __thread CxTask* tls_current_task;
extern __thread bool    tls_suspend_flag;

#endif
