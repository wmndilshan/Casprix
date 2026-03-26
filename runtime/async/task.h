/**
 * Casperix Runtime - Task System
 * Lightweight tasks for async/await concurrency
 */

#ifndef CASPERIX_TASK_H
#define CASPERIX_TASK_H

#include <stdint.h>
#include <stdbool.h>

// Task states
typedef enum {
    TASK_READY,       // Ready to run
    TASK_RUNNING,     // Currently executing
    TASK_SUSPENDED,   // Waiting on I/O or other task
    TASK_COMPLETED,   // Finished execution
    TASK_CANCELLED    // Cancelled by user
} TaskState;

// Task priority levels
typedef enum {
    TASK_PRIORITY_LOW = 0,
    TASK_PRIORITY_NORMAL = 1,
    TASK_PRIORITY_HIGH = 2,
    TASK_PRIORITY_CRITICAL = 3
} TaskPriority;

// Forward declarations
typedef struct Task Task;
typedef struct Future Future;

// Task entry point function
typedef void (*TaskFunc)(void* data);

// Task structure
struct Task {
    TaskFunc entry;           // Entry point function
    void* data;               // User data
    TaskState state;          // Current state
    TaskPriority priority;    // Priority level
    
    Future* result;           // Future holding result
    Task* continuation;       // Task to run after completion
    
    int worker_id;            // Worker thread running this task (-1 if none)
    uint64_t task_id;         // Unique task ID
    
    // Debugging
    const char* name;         // Optional task name
    int source_line;          // Source code line
};

// Create a new task
Task* task_create(TaskFunc entry, void* data, const char* name);

// Execute a task (internal, called by scheduler)
void task_execute(Task* task);

// Set task priority
void task_set_priority(Task* task, TaskPriority priority);

// Cancel a task
void task_cancel(Task* task);

// Check if task is complete
bool task_is_complete(const Task* task);

// Destroy a task
void task_destroy(Task* task);

#endif // CASPERIX_TASK_H
