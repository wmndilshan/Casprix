#define _GNU_SOURCE
#include "cx_task.h"
#include "cx_runtime.h"
#include "../memory/arc.h"

#define CxTask ThreadPoolTask
#include "../net/threadpool.h"
#undef CxTask
#include <stdatomic.h>
#include <stdlib.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

__thread CxTask* tls_current_task = NULL;
__thread bool    tls_suspend_flag = false;

static void cx_task_entry_trampoline(void* ctx);
static void cx_task_waker_cb(void* result, void* userdata);

static void cx_future_arc_destructor(void* obj) {
    Future* f = (Future*)obj;
    if (f->callbacks) free(f->callbacks);
    if (f->callback_data) free(f->callback_data);
    if (f->mutex) {
        pthread_mutex_destroy((pthread_mutex_t*)f->mutex);
        free(f->mutex);
    }
    if (f->cond) {
        pthread_cond_destroy((pthread_cond_t*)f->cond);
        free(f->cond);
    }
}

Future* cx_future_create_arc(MemoryManager* mm) {
    Future* f = (Future*)mem_arc_alloc_with_destructor(mm, sizeof(Future), cx_future_arc_destructor);
    if (!f) return NULL;
    
    f->state = FUTURE_PENDING;
    f->mutex = malloc(sizeof(pthread_mutex_t));
    f->cond = malloc(sizeof(pthread_cond_t));
    pthread_mutex_init((pthread_mutex_t*)f->mutex, NULL);
    pthread_cond_init((pthread_cond_t*)f->cond, NULL);
    
    return f;
}

CxTask* cx_task_create(CxArena* arena, MemoryManager* mm, TaskFunc entry, void* data, const char* name) {
    CxTask* task = (CxTask*)cx_arena_alloc(arena, sizeof(CxTask));
    if (!task) return NULL;
    
    task->entry = entry;
    task->data = data;
    atomic_init(&task->state, TASK_READY);
    task->priority = TASK_PRIORITY_NORMAL;
    task->result = cx_future_create_arc(mm);
    task->continuation = NULL;
    task->coro = NULL;
    task->arena = arena;
    task->rt = NULL; 
    task->bridge_ref = NULL;
    task->task_id = 0; 
    task->name = name;
    
    return task;
}

void cx_task_execute(CxTask* task) {
    Coroutine* caller_coro = coro_current();
    
    if (!task->coro) {
        task->coro = coro_create(cx_task_entry_trampoline, task, CORO_DEFAULT_STACK_SIZE);
        if (!task->coro) return;
    }
    
    atomic_store_explicit(&task->state, TASK_RUNNING, memory_order_release);
    printf("Task %s: starting execute\n", task->name ? task->name : "anon");
    coro_switch(caller_coro, task->coro);
    printf("Task %s: returned from switch, finished=%d\n", task->name ? task->name : "anon", coro_is_finished(task->coro));
    
    if (coro_is_finished(task->coro)) {
        atomic_store_explicit(&task->state, TASK_COMPLETED, memory_order_release);
        future_complete(task->result, NULL);
        
        if (task->continuation && task->rt) {
            // Forward declare the trampoline if needed, or just use it if it's available.
            // Since it's in cx_runtime.c, I might need to export it or define it here.
            // Actually, I'll just use the one from cx_runtime.c if I can, but it's static there.
            // I'll make it non-static in cx_runtime.c or define a public one.
            void cx_task_execute_trampoline(void* arg);
            cx_threadpool_submit(task->rt->threadpool, cx_task_execute_trampoline, task->continuation);
        }
        
        coro_destroy(task->coro);
        task->coro = NULL;
    } else {
        // Task yielded for I/O or future
        if (atomic_load_explicit(&task->state, memory_order_acquire) == TASK_SUSPENDED) {
            tls_suspend_flag = true;
        } else if (atomic_load_explicit(&task->state, memory_order_acquire) == TASK_RUNNING) {
            atomic_store_explicit(&task->state, TASK_SUSPENDED, memory_order_release);
            tls_suspend_flag = true;
        }
    }
}

void cx_task_await_future(CxTask* task, Future* future) {
    if (future_is_ready(future)) {
        return;
    }
    
    future_then(future, cx_task_waker_cb, task);
    atomic_store_explicit(&task->state, TASK_SUSPENDED, memory_order_release);
    coro_yield();
}

static void cx_task_waker_cb(void* result, void* userdata) {
    CxTask* task = (CxTask*)userdata;
    atomic_store_explicit(&task->state, TASK_READY, memory_order_release);
    
    if (task->bridge_ref && task->rt) {
        cx_scheduler_bridge_push_ready(task->bridge_ref, task);
        
        // Futex-wake the threadpool via atomic add to pending_count
        atomic_fetch_add_explicit(&task->rt->threadpool->pending_count, 1, memory_order_acq_rel);
        
        syscall(SYS_futex, &task->rt->threadpool->pending_count, 
                FUTEX_WAKE_PRIVATE, task->rt->n_workers, NULL, NULL, 0);
    }
}

static void cx_task_entry_trampoline(void* ctx) {
    CxTask* task = (CxTask*)ctx;
    tls_current_task = task;
    
    task->entry(task->data);
    
    // coro_posix2.c sets finished = true and switches back.
    // So we don't strictly need to do much here, but instructions say:
    // "Set task->coro finished flag if not already. coro_switch back to parent."
    // coro_posix2.c's trampoline already does this.
}
