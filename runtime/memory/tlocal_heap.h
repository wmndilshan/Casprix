/**
 * Casperix Runtime - Thread-Local Heap Allocator
 *
 * Each thread gets its own fast bump-allocated heap for small objects.
 * This eliminates lock contention on the global allocator for the
 * common case of short-lived allocations.
 *
 * Design:
 *   - Thread-local 256 KB slab per thread
 *   - Bump allocation for objects <= 4 KB
 *   - Falls back to global malloc for larger objects
 *   - Lock-free: each thread owns its slab exclusively
 *   - Slabs are returned to a global free-list on thread exit
 */

#ifndef CASPERIX_TLOCAL_HEAP_H
#define CASPERIX_TLOCAL_HEAP_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Configuration ─── */

#define TLOCAL_SLAB_SIZE        (256 * 1024)   /* 256 KB per thread      */
#define TLOCAL_MAX_ALLOC_SIZE   (4 * 1024)     /* Max object for slab    */
#define TLOCAL_ALIGNMENT        16             /* 16-byte alignment      */
#define TLOCAL_MAX_FREE_SLABS   32             /* Max cached free slabs  */

/* ─── Slab ─── */

typedef struct TLocalSlab {
    uint8_t*            memory;      /* Backing storage                  */
    size_t              capacity;    /* Total bytes                      */
    size_t              used;        /* Bump pointer offset              */
    struct TLocalSlab*  next;        /* Free-list linkage                */
} TLocalSlab;

/* ─── Thread-local heap context ─── */

typedef struct {
    TLocalSlab* current_slab;       /* Active slab for this thread      */
    size_t      total_allocated;    /* Bytes allocated from slabs       */
    size_t      total_fallbacks;    /* Allocations that fell to malloc  */
    size_t      num_allocs;         /* Total allocation count           */
} TLocalHeap;

/* ─── API ─── */

/**
 * Initialize the thread-local heap system (call once at startup).
 * Sets up the global free slab pool.
 */
void tlocal_heap_init_global(void);

/**
 * Shut down the thread-local heap system.
 * Frees all cached slabs.
 */
void tlocal_heap_shutdown_global(void);

/**
 * Allocate memory from the calling thread's local heap.
 * - Objects <= TLOCAL_MAX_ALLOC_SIZE use bump allocation (O(1), no locks)
 * - Larger objects fall back to malloc
 * Memory is zeroed.
 */
void* tlocal_alloc(size_t size);

/**
 * Reset the current thread's slab (reuse memory).
 * Only safe if all allocations from this slab are dead.
 */
void tlocal_reset(void);

/**
 * Release the current thread's slab back to the global pool.
 * Call this on thread exit.
 */
void tlocal_thread_cleanup(void);

/**
 * Get statistics for the current thread's heap.
 */
TLocalHeap* tlocal_get_heap(void);

/**
 * Print thread-local heap statistics.
 */
void tlocal_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* CASPERIX_TLOCAL_HEAP_H */
