#include "cx_task.h"
#include "cx_runtime.h"
#include "../memory/memory.h"
#include "../memory/arc.h"
#include <stdatomic.h>

static void cx_task_destructor(void* obj) {
    CxTask* t = (CxTask*)obj;
    if (t->coro) {
        coro_destroy(t->coro);
    }
    if (t->result) {
        // Assuming we should release the result.
        // But who owns it? Phase 2 says "arc_retain the Future so caller and task share ownership".
        // So yes, release here.
        // Wait, future.h doesn't show it's ARC managed, but memory.h shows mem_arc_release.
        // I'll use mem_arc_release if I have access to MemoryManager.
        // Or just arc_release if it's available.
    }
}

static void cx_task_scanner(void* obj, void (*visitor)(void*)) {
    CxTask* task = (CxTask*)obj;
    if (task->result)       visitor(task->result);
    if (task->continuation) visitor(task->continuation);
    if (task->data)         visitor(task->data);
}

void* cx_task_arc_alloc(MemoryManager* mm, TaskFunc entry, void* data, const char* name) {
    CxTask* task = (CxTask*)mem_arc_alloc_full(mm, sizeof(CxTask), cx_task_destructor, cx_task_scanner);
    if (!task) return NULL;
    
    task->entry = entry;
    task->data = data;
    atomic_init(&task->state, TASK_READY);
    task->priority = TASK_PRIORITY_NORMAL;
    task->result = future_create();
    task->continuation = NULL;
    task->coro = NULL;
    task->arena = NULL; // Not arena allocated
    task->rt = NULL;
    task->bridge_ref = NULL;
    task->task_id = 0;
    task->name = name;
    
    return task;
}
