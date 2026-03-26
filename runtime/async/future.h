/**
 * Casperix Runtime - Future/Promise Implementation
 * Represents the eventual result of an async operation
 */

#ifndef CASPERIX_FUTURE_H
#define CASPERIX_FUTURE_H

#include <stdbool.h>
#include <stdint.h>

// Future states
typedef enum {
    FUTURE_PENDING,    // Not yet complete
    FUTURE_COMPLETED,  // Successfully completed
    FUTURE_FAILED,     // Completed with error
    FUTURE_CANCELLED   // Cancelled
} FutureState;

// Callback type for future completion
typedef void (*FutureCallback)(void* result, void* user_data);

// Future structure
typedef struct Future {
    FutureState state;
    void* result;           // Result value
    void* error;            // Error value if failed
    
    FutureCallback* callbacks;     // Array of callbacks
    void** callback_data;          // User data for callbacks
    int callback_count;
    int callback_capacity;
    
    // Synchronization
    void* mutex;            // Platform-specific mutex
    void* cond;             // Platform-specific condition variable
} Future;

// Create a new future
Future* future_create(void);

// Complete a future with a result
void future_complete(Future* future, void* result);

// Fail a future with an error
void future_fail(Future* future, void* error);

// Cancel a future
void future_cancel(Future* future);

// Add a callback to be called when future completes
void future_then(Future* future, FutureCallback callback, void* user_data);

// Wait for future to complete (blocking)
void* future_wait(Future* future);

// Check if future is ready
bool future_is_ready(const Future* future);

// Destroy a future
void future_destroy(Future* future);

#endif // CASPERIX_FUTURE_H
