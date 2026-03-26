// Casprix Concurrent Data Structures Library
// Thread-safe collections for parallel programming

#ifndef NUWAN_CONCURRENT_H
#define NUWAN_CONCURRENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct nuwan_concurrent_queue nuwan_concurrent_queue_t;
typedef struct nuwan_concurrent_map nuwan_concurrent_map_t;
typedef struct nuwan_barrier nuwan_barrier_t;
typedef struct nuwan_semaphore nuwan_semaphore_t;
typedef struct nuwan_rwlock nuwan_rwlock_t;

// Concurrent Queue (blocking FIFO queue)
struct nuwan_concurrent_queue {
    int64_t handle;
    int capacity;
    int size;
    void** items;
    void* mutex;
    void* not_empty_cond;
    void* not_full_cond;
};

// Concurrent Queue API
nuwan_concurrent_queue_t* nuwan_concurrent_queue_create(int max_capacity);
bool nuwan_concurrent_queue_enqueue(nuwan_concurrent_queue_t* queue, void* item);
void* nuwan_concurrent_queue_dequeue(nuwan_concurrent_queue_t* queue);  // Blocks if empty
void* nuwan_concurrent_queue_try_dequeue(nuwan_concurrent_queue_t* queue);  // Returns NULL if empty
int nuwan_concurrent_queue_get_size(nuwan_concurrent_queue_t* queue);
bool nuwan_concurrent_queue_is_empty(nuwan_concurrent_queue_t* queue);
bool nuwan_concurrent_queue_is_full(nuwan_concurrent_queue_t* queue);
void nuwan_concurrent_queue_destroy(nuwan_concurrent_queue_t* queue);

// Concurrent Map (thread-safe hash map)
typedef struct nuwan_map_entry {
    char* key;
    void* value;
    struct nuwan_map_entry* next;
} nuwan_map_entry_t;

struct nuwan_concurrent_map {
    int64_t handle;
    int capacity;
    int size;
    nuwan_map_entry_t** buckets;
    void* mutex;
};

// Concurrent Map API
nuwan_concurrent_map_t* nuwan_concurrent_map_create(void);
void nuwan_concurrent_map_put(nuwan_concurrent_map_t* map, const char* key, void* value);
void* nuwan_concurrent_map_get(nuwan_concurrent_map_t* map, const char* key);
bool nuwan_concurrent_map_remove(nuwan_concurrent_map_t* map, const char* key);
bool nuwan_concurrent_map_contains_key(nuwan_concurrent_map_t* map, const char* key);
int nuwan_concurrent_map_get_size(nuwan_concurrent_map_t* map);
void nuwan_concurrent_map_clear(nuwan_concurrent_map_t* map);
void nuwan_concurrent_map_destroy(nuwan_concurrent_map_t* map);

// Barrier (synchronization point for multiple threads)
struct nuwan_barrier {
    int64_t handle;
    int thread_count;
    int waiting_count;
    int generation;
    void* mutex;
    void* condition;
};

// Barrier API
nuwan_barrier_t* nuwan_barrier_create(int thread_count);
void nuwan_barrier_await(nuwan_barrier_t* barrier);  // Blocks until all threads arrive
void nuwan_barrier_reset(nuwan_barrier_t* barrier);
void nuwan_barrier_destroy(nuwan_barrier_t* barrier);

// Semaphore (counting semaphore for resource limiting)
struct nuwan_semaphore {
    int64_t handle;
    int permits;
    int available;
    void* mutex;
    void* condition;
};

// Semaphore API
nuwan_semaphore_t* nuwan_semaphore_create(int initial_permits);
void nuwan_semaphore_acquire(nuwan_semaphore_t* sem);  // Blocks if no permits
bool nuwan_semaphore_try_acquire(nuwan_semaphore_t* sem);  // Non-blocking
void nuwan_semaphore_release(nuwan_semaphore_t* sem);
int nuwan_semaphore_available_permits(nuwan_semaphore_t* sem);
void nuwan_semaphore_destroy(nuwan_semaphore_t* sem);

// Read-Write Lock (multiple readers, single writer)
struct nuwan_rwlock {
    int64_t handle;
    int readers;
    bool writer;
    void* mutex;
    void* read_condition;
    void* write_condition;
};

// Read-Write Lock API
nuwan_rwlock_t* nuwan_rwlock_create(void);
void nuwan_rwlock_read_lock(nuwan_rwlock_t* lock);
void nuwan_rwlock_read_unlock(nuwan_rwlock_t* lock);
void nuwan_rwlock_write_lock(nuwan_rwlock_t* lock);
void nuwan_rwlock_write_unlock(nuwan_rwlock_t* lock);
void nuwan_rwlock_destroy(nuwan_rwlock_t* lock);

// Future (one-time value container)
typedef struct nuwan_future {
    int64_t handle;
    void* value;
    bool is_ready;
    void* mutex;
    void* condition;
} nuwan_future_t;

// Future API
nuwan_future_t* nuwan_future_create(void);
void nuwan_future_set(nuwan_future_t* future, void* value);
void* nuwan_future_get(nuwan_future_t* future);  // Blocks until ready
void* nuwan_future_get_timeout(nuwan_future_t* future, int timeout_ms);  // Returns NULL on timeout
bool nuwan_future_is_done(nuwan_future_t* future);
void nuwan_future_destroy(nuwan_future_t* future);

#ifdef __cplusplus
}
#endif

#endif // NUWAN_CONCURRENT_H
