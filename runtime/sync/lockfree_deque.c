// Casperix Runtime - Lock-Free Work-Stealing Deque Implementation
// Based on the Chase-Lev algorithm with dynamic circular array
//
// Reference: "Dynamic Circular Work-Stealing Deque" by Chase and Lev (SPAA 2005)

#include "lockfree_deque.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifdef _WIN32
    #include <windows.h>
    #define ATOMIC_LOAD(ptr) InterlockedCompareExchange64((volatile LONG64*)(ptr), 0, 0)
    #define ATOMIC_STORE(ptr, val) InterlockedExchange64((volatile LONG64*)(ptr), (val))
    #define ATOMIC_CAS(ptr, expected, desired) \
        (InterlockedCompareExchange64((volatile LONG64*)(ptr), (desired), (expected)) == (expected))
    #define MEMORY_FENCE() MemoryBarrier()
#else
    #define ATOMIC_LOAD(ptr) __atomic_load_n(ptr, __ATOMIC_SEQ_CST)
    #define ATOMIC_STORE(ptr, val) __atomic_store_n(ptr, val, __ATOMIC_SEQ_CST)
    #define ATOMIC_CAS(ptr, expected, desired) \
        __atomic_compare_exchange_n(ptr, &(expected), desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
    #define MEMORY_FENCE() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

// Circular array for holding work items
typedef struct CircularArray {
    size_t capacity;       // Must be power of 2
    WorkItem* items;      // Array of work items
} CircularArray;

struct LockFreeDeque {
    volatile int64_t top;      // Owner pushes/pops here
    volatile int64_t bottom;   // Thieves steal here
    CircularArray* array;      // Current array (can grow)
    volatile CircularArray* old_array;  // For safe memory reclamation
};

// --- CircularArray Operations ---

static CircularArray* array_create(size_t capacity) {
    // Ensure capacity is power of 2
    assert((capacity & (capacity - 1)) == 0);
    
    CircularArray* arr = (CircularArray*)malloc(sizeof(CircularArray));
    if (!arr) return NULL;
    
    arr->capacity = capacity;
    arr->items = (WorkItem*)calloc(capacity, sizeof(WorkItem));
    if (!arr->items) {
        free(arr);
        return NULL;
    }
    
    return arr;
}

static void array_destroy(CircularArray* arr) {
    if (!arr) return;
    free(arr->items);
    free(arr);
}

static inline WorkItem array_get(CircularArray* arr, int64_t index) {
    return arr->items[index % arr->capacity];
}

static inline void array_put(CircularArray* arr, int64_t index, WorkItem item) {
    arr->items[index % arr->capacity] = item;
}

// --- Deque Implementation ---

LockFreeDeque* deque_create(size_t initial_capacity) {
    // Ensure power of 2
    if ((initial_capacity & (initial_capacity - 1)) != 0) {
        // Round up to next power of 2
        initial_capacity--;
        initial_capacity |= initial_capacity >> 1;
        initial_capacity |= initial_capacity >> 2;
        initial_capacity |= initial_capacity >> 4;
        initial_capacity |= initial_capacity >> 8;
        initial_capacity |= initial_capacity >> 16;
        initial_capacity++;
    }
    
    if (initial_capacity < 16) {
        initial_capacity = 16;
    }
    
    LockFreeDeque* deque = (LockFreeDeque*)malloc(sizeof(LockFreeDeque));
    if (!deque) return NULL;
    
    deque->array = array_create(initial_capacity);
    if (!deque->array) {
        free(deque);
        return NULL;
    }
    
    deque->top = 0;
    deque->bottom = 0;
    deque->old_array = NULL;
    
    return deque;
}

void deque_destroy(LockFreeDeque* deque) {
    if (!deque) return;
    
    if (deque->array) {
        array_destroy((CircularArray*)deque->array);
    }
    if (deque->old_array) {
        array_destroy((CircularArray*)deque->old_array);
    }
    
    free(deque);
}

// --- Owner Operations ---

bool deque_push(LockFreeDeque* deque, WorkItem item) {
    if (!deque || !item) return false;
    
    int64_t b = ATOMIC_LOAD(&deque->bottom);
    int64_t t = ATOMIC_LOAD(&deque->top);
    CircularArray* arr = deque->array;
    
    // Check if array is full
    if (b - t >= (int64_t)arr->capacity) {
        // Need to grow - for now, just fail
        // TODO: Implement dynamic growing
        return false;
    }
    
    // Push item
    array_put(arr, b, item);
    MEMORY_FENCE();
    ATOMIC_STORE(&deque->bottom, b + 1);
    
    return true;
}

WorkItem deque_pop(LockFreeDeque* deque) {
    if (!deque) return NULL;
    
    int64_t b = ATOMIC_LOAD(&deque->bottom) - 1;
    CircularArray* arr = deque->array;
    
    ATOMIC_STORE(&deque->bottom, b);
    MEMORY_FENCE();
    
    int64_t t = ATOMIC_LOAD(&deque->top);
    
    WorkItem item = NULL;
    
    if (t <= b) {
        // Non-empty queue
        item = array_get(arr, b);
        
        if (t == b) {
            // Last item - race with steal
            if (!ATOMIC_CAS(&deque->top, t, t + 1)) {
                // Lost race to steal
                item = NULL;
            }
            ATOMIC_STORE(&deque->bottom, b + 1);
        }
    } else {
        // Empty queue
        ATOMIC_STORE(&deque->bottom, b + 1);
    }
    
    return item;
}

// --- Thief Operations ---

WorkItem deque_steal(LockFreeDeque* deque) {
    if (!deque) return NULL;
    
    int64_t t = ATOMIC_LOAD(&deque->top);
    MEMORY_FENCE();
    int64_t b = ATOMIC_LOAD(&deque->bottom);
    
    WorkItem item = NULL;
    
    if (t < b) {
        // Non-empty queue
        CircularArray* arr = deque->array;
        item = array_get(arr, t);
        
        // Try to claim it
        if (!ATOMIC_CAS(&deque->top, t, t + 1)) {
            // Lost race
            return NULL;
        }
    }
    
    return item;
}

// --- State Query ---

size_t deque_size(const LockFreeDeque* deque) {
    if (!deque) return 0;
    
    int64_t b = ATOMIC_LOAD(&deque->bottom);
    int64_t t = ATOMIC_LOAD(&deque->top);
    
    int64_t size = b - t;
    return size > 0 ? (size_t)size : 0;
}

bool deque_is_empty(const LockFreeDeque* deque) {
    return deque_size(deque) == 0;
}

size_t deque_capacity(const LockFreeDeque* deque) {
    if (!deque || !deque->array) return 0;
    return deque->array->capacity;
}
