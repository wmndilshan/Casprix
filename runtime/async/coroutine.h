// Casperix Runtime - Coroutine Abstraction
// Platform-agnostic coroutine API for async/await implementation
//
// Supports stackful coroutines with platform-specific backends:
// - Windows: Fibers
// - POSIX: ucontext (or custom assembly)
//
// Each coroutine has its own stack (default 64KB) and can be suspended/resumed

#ifndef CASPERIX_COROUTINE_H
#define CASPERIX_COROUTINE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Default stack size for coroutines (64KB)
#define CORO_DEFAULT_STACK_SIZE (64 * 1024)

// Minimum stack size (16KB)
#define CORO_MIN_STACK_SIZE (16 * 1024)

// Maximum stack size (1MB)
#define CORO_MAX_STACK_SIZE (1024 * 1024)

// Forward declaration
typedef struct Coroutine Coroutine;

// Coroutine entry point function
// Context passed to the entry point
typedef void (*CoroutineEntry)(void* context);

// --- Lifecycle API ---

// Create a new coroutine with the given entry point and stack size
// Returns NULL on failure
Coroutine* coro_create(CoroutineEntry entry, void* context, size_t stack_size);

// Destroy a coroutine and free its resources
// WARNING: Do not destroy a currently running coroutine
void coro_destroy(Coroutine* coro);

// --- Control Flow ---

// Switch execution from the current coroutine to the target coroutine
// The current coroutine is suspended and can be resumed later
// Returns true on success, false if already in target coroutine
bool coro_switch(Coroutine* from, Coroutine* to);

// Yield control back to the caller coroutine
// Convenience wrapper for coro_switch(current, parent)
void coro_yield(void);

// --- State Query ---

// Get the currently executing coroutine
// Returns NULL if not running in a coroutine context
Coroutine* coro_current(void);

// Check if a coroutine has completed execution
bool coro_is_finished(const Coroutine* coro);

// Get the coroutine that created this coroutine (parent)
Coroutine* coro_get_parent(const Coroutine* coro);

// --- Thread-Local Management ---

// Initialize coroutine system for the current thread
// Must be called before using coroutines on a thread
// Returns the main coroutine (representing the thread's original stack)
Coroutine* coro_thread_init(void);

// Clean up coroutine system for the current thread
void coro_thread_cleanup(void);

// Get the main coroutine for the current thread
Coroutine* coro_get_main(void);

// --- Statistics ---

typedef struct {
    size_t total_created;      // Total coroutines created
    size_t total_destroyed;    // Total coroutines destroyed
    size_t current_alive;      // Currently alive coroutines
    size_t total_switches;     // Total context switches
    size_t total_stack_bytes;  // Total stack memory allocated
} CoroutineStats;

CoroutineStats coro_get_stats(void);
void coro_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif // CASPERIX_COROUTINE_H
