/**
 * Casprix Runtime - Task Implementation
 */

#include "task.h"
#include "future.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif

static TaskState task_state_load(const Task* task) {
#ifdef _WIN32
    return (TaskState)InterlockedCompareExchange((volatile LONG*)&task->state, 0, 0);
#else
    return (TaskState)__atomic_load_n(&task->state, __ATOMIC_ACQUIRE);
#endif
}

static bool task_state_change(Task* task, TaskState expected, TaskState desired) {
#ifdef _WIN32
    return InterlockedCompareExchange((volatile LONG*)&task->state,
                                      (LONG)desired, (LONG)expected) == (LONG)expected;
#else
    return __atomic_compare_exchange_n(&task->state, &expected, desired, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#endif
}

// Global task ID counter
#ifdef _WIN32
static volatile LONG64 g_next_task_id = 1;
static uint64_t next_task_id(void) {
    return (uint64_t)InterlockedIncrement64(&g_next_task_id) - 1;
}
#else
static uint64_t g_next_task_id = 1;
static uint64_t next_task_id(void) {
    return __atomic_fetch_add(&g_next_task_id, 1, __ATOMIC_RELAXED);
}
#endif

Task* task_create(TaskFunc entry, void* data, const char* name) {
    Task* task = (Task*)calloc(1, sizeof(Task));
    if (!task) {
        fprintf(stderr, "Failed to allocate task\n");
        return NULL;
    }
    
    task->entry = entry;
    task->data = data;
    task->state = TASK_READY;
    task->priority = TASK_PRIORITY_NORMAL;
    task->result = future_create();
    task->continuation = NULL;
    task->worker_id = -1;
    task->task_id = next_task_id();
#ifdef _WIN32
    task->name = name ? _strdup(name) : NULL;
#else
    task->name = name ? strdup(name) : NULL;
#endif
    task->source_line = 0;
    
    return task;
}

void task_execute(Task* task) {
    if (!task || !task_state_change(task, TASK_READY, TASK_RUNNING)) {
        return;
    }
    
    // Execute the task function
    if (task->entry) {
        task->entry(task->data);
    }
    
    // Mark as completed
    bool completed = task_state_change(task, TASK_RUNNING, TASK_COMPLETED);

    // Cancellation wins a race with task completion.
    if (completed && task->result) {
        future_complete(task->result, NULL);
    }
    
    // If there's a continuation, run it immediately when no scheduler bridge is provided.
    if (completed && task->continuation) {
        Task* continuation = task->continuation;
        task->continuation = NULL;
        TaskState continuation_state = task_state_load(continuation);
        if (continuation_state == TASK_SUSPENDED)
            task_state_change(continuation, TASK_SUSPENDED, TASK_READY);
        if (continuation != task && task_state_load(continuation) == TASK_READY) {
            task_execute(continuation);
        }
    }
}

void task_set_priority(Task* task, TaskPriority priority) {
    if (task) {
        task->priority = priority;
    }
}

void task_cancel(Task* task) {
    if (!task) return;
    for (;;) {
        TaskState state = task_state_load(task);
        if (state == TASK_COMPLETED || state == TASK_CANCELLED) return;
        if (task_state_change(task, state, TASK_CANCELLED)) break;
    }
    if (task->result) future_cancel(task->result);
}

bool task_is_complete(const Task* task) {
    if (!task) return false;
    TaskState state = task_state_load(task);
    return state == TASK_COMPLETED || state == TASK_CANCELLED;
}

void task_destroy(Task* task) {
    if (!task) return;
    
    if (task->name) {
        free((void*)task->name);
    }
    
    if (task->result) {
        future_destroy(task->result);
    }
    
    free(task);
}
