#include "cx_task.h"
#include "cx_runtime.h"
#include <stdbool.h>

static __thread bool tls_worker_initialized = false;

void cx_worker_init(void) {
    if (!tls_worker_initialized) {
        coro_thread_init();
        tls_worker_initialized = true;
    }
}

void cx_worker_run_task(CxTask* task) {
    cx_worker_init();
    cx_task_execute(task);
    
    if (task->state == TASK_COMPLETED) {
        // Task is done. 
        // The caller (threadpool) will decrement pending_count.
    }
    if (task->state == TASK_SUSPENDED) {
        // Do NOT decrement pending_count here.
        // Wait, if the threadpool loop is:
        //   while(1) { task = pop(); task->fn(task->arg); pending_count--; }
        // Then it will decrement. 
        // We need the patch to avoid this.
    }
}
