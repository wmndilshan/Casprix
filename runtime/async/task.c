/**
 * Casperix Runtime - Task Implementation
 */

#include "task.h"
#include "future.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Global task ID counter
static _Atomic(uint64_t) g_next_task_id = 1;

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
    task->task_id = __atomic_fetch_add(&g_next_task_id, 1, __ATOMIC_RELAXED);
    task->name = name ? strdup(name) : NULL;
    task->source_line = 0;
    
    return task;
}

void task_execute(Task* task) {
    if (!task || task->state != TASK_READY) {
        return;
    }
    
    task->state = TASK_RUNNING;
    
    // Execute the task function
    if (task->entry) {
        task->entry(task->data);
    }
    
    // Mark as completed
    task->state = TASK_COMPLETED;
    
    // Complete the future
    if (task->result) {
        future_complete(task->result, NULL);
    }
    
    // If there's a continuation, run it immediately when no scheduler bridge is provided.
    if (task->continuation) {
        Task* continuation = task->continuation;
        task->continuation = NULL;
        if (continuation != task && continuation->state != TASK_COMPLETED && continuation->state != TASK_CANCELLED) {
            continuation->state = TASK_READY;
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
    if (task && task->state != TASK_COMPLETED) {
        task->state = TASK_CANCELLED;
        
        if (task->result) {
            future_cancel(task->result);
        }
    }
}

bool task_is_complete(const Task* task) {
    return task && (task->state == TASK_COMPLETED || task->state == TASK_CANCELLED);
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
