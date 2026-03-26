// Casperix Runtime - Windows Fiber-based Coroutines
// Implementation of coroutine API using Windows Fibers
//
// Fibers are lightweight threads that are scheduled cooperatively
// Perfect for implementing stackful coroutines

#include "coroutine.h"
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

// --- Internal Structures ---

typedef enum {
    CORO_STATE_READY,      // Created but not started
    CORO_STATE_RUNNING,    // Currently executing
    CORO_STATE_SUSPENDED,  // Suspended, can be resumed
    CORO_STATE_FINISHED    // Completed execution
} CoroutineState;

struct Coroutine {
    LPVOID fiber;              // Windows fiber handle
    CoroutineEntry entry;      // Entry point function
    void* context;             // User context
    size_t stack_size;         // Stack size in bytes
    CoroutineState state;      // Current state
    
    struct Coroutine* parent;  // Parent coroutine (caller)
    bool is_main;              // True if this is the main thread coroutine
    
    uint64_t id;               // Unique ID for debugging
};

// --- Thread-Local Storage ---

#ifdef _MSC_VER
    #define THREAD_LOCAL __declspec(thread)
#else
    #define THREAD_LOCAL __thread
#endif

static THREAD_LOCAL Coroutine* tls_current_coro = NULL;
static THREAD_LOCAL Coroutine* tls_main_coro = NULL;
static THREAD_LOCAL bool tls_initialized = false;

// --- Statistics ---

static CoroutineStats g_stats = {0};

// --- Fiber Entry Wrapper ---

static VOID CALLBACK fiber_entry_wrapper(LPVOID param) {
    Coroutine* coro = (Coroutine*)param;
    
    // Update state
    coro->state = CORO_STATE_RUNNING;
    tls_current_coro = coro;
    
    // Call user entry point
    if (coro->entry) {
        coro->entry(coro->context);
    }
    
    // Mark as finished
    coro->state = CORO_STATE_FINISHED;
    
    // Determine where to switch back
    Coroutine* return_to = coro->parent ? coro->parent : tls_main_coro;
    
    // Update current coroutine BEFORE switching
    if (return_to) {
        tls_current_coro = return_to;
        return_to->state = CORO_STATE_RUNNING;
        SwitchToFiber(return_to->fiber);
    }
}

// --- Lifecycle Implementation ---

Coroutine* coro_create(CoroutineEntry entry, void* context, size_t stack_size) {
    // Ensure coroutine system is initialized
    if (!tls_initialized) {
        coro_thread_init();
    }
    
    // Validate stack size
    if (stack_size == 0) {
        stack_size = CORO_DEFAULT_STACK_SIZE;
    }
    if (stack_size < CORO_MIN_STACK_SIZE) {
        stack_size = CORO_MIN_STACK_SIZE;
    }
    if (stack_size > CORO_MAX_STACK_SIZE) {
        stack_size = CORO_MAX_STACK_SIZE;
    }
    
    // Allocate coroutine structure
    Coroutine* coro = (Coroutine*)calloc(1, sizeof(Coroutine));
    if (!coro) return NULL;
    
    // Create fiber
    coro->fiber = CreateFiber(stack_size, fiber_entry_wrapper, coro);
    if (!coro->fiber) {
        free(coro);
        return NULL;
    }
    
    // Initialize fields
    coro->entry = entry;
    coro->context = context;
    coro->stack_size = stack_size;
    coro->state = CORO_STATE_READY;
    coro->parent = tls_current_coro;  // Current coro becomes parent
    coro->is_main = false;
    coro->id = g_stats.total_created;
    
    // Update stats
    g_stats.total_created++;
    g_stats.current_alive++;
    g_stats.total_stack_bytes += stack_size;
    
    return coro;
}

void coro_destroy(Coroutine* coro) {
    if (!coro) return;
    
    // Don't destroy the currently running coroutine (unless it's finished)
    if (coro == tls_current_coro && coro->state != CORO_STATE_FINISHED) {
        assert(false && "Cannot destroy currently running coroutine");
    }
    
    // Don't destroy the main coroutine directly
    assert(!coro->is_main);
    
    // Delete fiber
    if (coro->fiber && coro->state != CORO_STATE_RUNNING) {
        DeleteFiber(coro->fiber);
    }
    
    // Update stats
    g_stats.total_destroyed++;
    g_stats.current_alive--;
    g_stats.total_stack_bytes -= coro->stack_size;
    
    free(coro);
}

// --- Control Flow Implementation ---

bool coro_switch(Coroutine* from, Coroutine* to) {
    if (!from || !to) return false;
    if (from == to) return false;  // Already in target
    
    // Update states
    if (from->state == CORO_STATE_RUNNING) {
        from->state = CORO_STATE_SUSPENDED;
    }
    
    if (to->state == CORO_STATE_READY || to->state == CORO_STATE_SUSPENDED) {
        to->state = CORO_STATE_RUNNING;
    }
    
    // Update current coroutine
    tls_current_coro = to;
    
    // Update stats
    g_stats.total_switches++;
    
    // Perform the switch
    SwitchToFiber(to->fiber);
    
    // When we return here, we've been switched back
    return true;
}

void coro_yield(void) {
    Coroutine* current = tls_current_coro;
    if (!current) return;
    
    // Yield to parent
    if (current->parent) {
        coro_switch(current, current->parent);
    } else if (tls_main_coro && current != tls_main_coro) {
        // No explicit parent, yield to main
        coro_switch(current, tls_main_coro);
    }
}

// --- State Query Implementation ---

Coroutine* coro_current(void) {
    return tls_current_coro;
}

bool coro_is_finished(const Coroutine* coro) {
    return coro ? coro->state == CORO_STATE_FINISHED : false;
}

Coroutine* coro_get_parent(const Coroutine* coro) {
    return coro ? coro->parent : NULL;
}

// --- Thread-Local Management ---

Coroutine* coro_thread_init(void) {
    if (tls_initialized) {
        return tls_main_coro;
    }
    
    // Convert current thread to a fiber
    LPVOID main_fiber = ConvertThreadToFiber(NULL);
    if (!main_fiber) {
        // Maybe already a fiber?
        main_fiber = GetCurrentFiber();
    }
    
    // Create main coroutine structure
    Coroutine* main_coro = (Coroutine*)calloc(1, sizeof(Coroutine));
    if (!main_coro) return NULL;
    
    main_coro->fiber = main_fiber;
    main_coro->entry = NULL;
    main_coro->context = NULL;
    main_coro->stack_size = 0;  // Uses thread's default stack
    main_coro->state = CORO_STATE_RUNNING;
    main_coro->parent = NULL;
    main_coro->is_main = true;
    main_coro->id = g_stats.total_created;
    
    // Set thread-local state
    tls_main_coro = main_coro;
    tls_current_coro = main_coro;
    tls_initialized = true;
    
    // Update stats
    g_stats.total_created++;
    g_stats.current_alive++;
    
    return main_coro;
}

void coro_thread_cleanup(void) {
    if (!tls_initialized) return;
    
    // Note: Don't delete the main fiber - it represents the thread itself
    // Just clean up our tracking structure
    if (tls_main_coro) {
        g_stats.current_alive--;
        free(tls_main_coro);
        tls_main_coro = NULL;
    }
    
    tls_current_coro = NULL;
    tls_initialized = false;
}

Coroutine* coro_get_main(void) {
    if (!tls_initialized) {
        coro_thread_init();
    }
    return tls_main_coro;
}

// --- Statistics ---

CoroutineStats coro_get_stats(void) {
    return g_stats;
}

void coro_print_stats(void) {
    printf("\n=== Coroutine Statistics ===\n");
    printf("Total created:      %llu\n", (unsigned long long)g_stats.total_created);
    printf("Total destroyed:    %llu\n", (unsigned long long)g_stats.total_destroyed);
    printf("Currently alive:    %llu\n", (unsigned long long)g_stats.current_alive);
    printf("Total switches:     %llu\n", (unsigned long long)g_stats.total_switches);
    printf("Total stack bytes:  %llu (%.2f MB)\n", 
           (unsigned long long)g_stats.total_stack_bytes,
           g_stats.total_stack_bytes / (1024.0 * 1024.0));
    printf("============================\n");
}
