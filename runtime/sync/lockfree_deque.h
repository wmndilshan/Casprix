// Casperix Runtime - Lock-Free Deque
// Implementation of Chase-Lev work-stealing deque
//
// A work-stealing deque supports:
// - Owner thread: push/pop from one end (LIFO, for cache locality)
// - Thief threads: steal from other end (FIFO, for load balancing)
//
// This is the key data structure for the work-stealing scheduler

#ifndef CASPERIX_LOCKFREE_DEQUE_H
#define CASPERIX_LOCKFREE_DEQUE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration
typedef struct LockFreeDeque LockFreeDeque;

// Work item (generic pointer)
typedef void* WorkItem;

// --- Lifecycle ---

// Create a new work-stealing deque with given initial capacity
// Capacity must be a power of 2
LockFreeDeque* deque_create(size_t initial_capacity);

// Destroy the deque and free all memory
// WARNING: Must be called only when no other threads are accessing it
void deque_destroy(LockFreeDeque* deque);

// --- Owner Operations (Single-Threaded) ---

// Push an item onto the top (owner end) of the deque
// Returns true on success, false if deque needs to grow (not implemented yet)
bool deque_push(LockFreeDeque* deque, WorkItem item);

// Pop an item from the top (owner end) of the deque
// Returns NULL if deque is empty
WorkItem deque_pop(LockFreeDeque* deque);

// --- Thief Operations (Multi-Threaded) ---

// Steal an item from the bottom (thief end) of the deque
// Returns NULL if deque is empty or if a race was lost
WorkItem deque_steal(LockFreeDeque* deque);

// --- State Query ---

// Get approximate size (may be stale due to concurrent access)
size_t deque_size(const LockFreeDeque* deque);

// Check if deque is empty (may be stale)
bool deque_is_empty(const LockFreeDeque* deque);

// Get capacity
size_t deque_capacity(const LockFreeDeque* deque);

#ifdef __cplusplus
}
#endif

#endif // CASPERIX_LOCKFREE_DEQUE_H
