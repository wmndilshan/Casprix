/**
 * Casperix Runtime - Region Allocator
 *
 * Provides language-level region blocks where all allocations inside a
 * region are freed together at region exit.  Uses a bump allocator for
 * O(1) allocation with minimal overhead.
 *
 * Usage in Casprix:
 *
 *   region frame {
 *       let temp = Vector()
 *       let buf  = Buffer(1024)
 *       // ... all allocations freed here automatically
 *   }
 *
 * Regions can be nested.  A child region's lifetime is bounded by the
 * parent.  The runtime maintains a thread-local region stack.
 */

#ifndef CASPERIX_REGION_H
#define CASPERIX_REGION_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Configuration ─── */

#define REGION_DEFAULT_SIZE      (64 * 1024)   /* 64 KB initial block   */
#define REGION_ALIGNMENT         16            /* 16-byte alignment     */
#define REGION_MAX_NESTING       32            /* Max nested regions    */

/* ─── Region block (linked-list chunk) ─── */

typedef struct RegionBlock {
    uint8_t*           memory;      /* Raw backing memory                */
    size_t             capacity;    /* Block capacity in bytes           */
    size_t             used;        /* Current bump offset               */
    struct RegionBlock* next;       /* Next overflow block               */
} RegionBlock;

/* ─── Destructor registration for region objects ─── */

typedef void (*region_dtor_fn)(void* obj);

typedef struct RegionDtor {
    void*           obj;            /* Pointer to object                 */
    region_dtor_fn  dtor;           /* Destructor to call                */
    struct RegionDtor* next;        /* Linked list                       */
} RegionDtor;

/* ─── Region handle ─── */

typedef struct Region {
    const char*     name;           /* Debug name (e.g. "frame")         */
    RegionBlock*    first_block;    /* First memory block                */
    RegionBlock*    current_block;  /* Active block for bump allocation  */
    RegionDtor*     destructors;    /* LIFO destructor chain             */

    /* Stats */
    size_t          total_allocated;
    size_t          total_used;
    size_t          num_allocs;
    size_t          block_count;
} Region;

/* ─── Region Stack (thread-local) ─── */

typedef struct {
    Region* stack[REGION_MAX_NESTING];
    int     depth;
} RegionStack;

/* ─── API: Region lifecycle ─── */

/**
 * Create a new region with optional name and initial capacity.
 * Pass 0 for default capacity.
 */
Region* region_create(const char* name, size_t initial_capacity);

/**
 * Destroy region — calls all registered destructors in LIFO order,
 * then frees all blocks.  This is the "scope exit" operation.
 */
void region_destroy(Region* region);

/**
 * Push region onto thread-local stack (called at region block entry).
 */
void region_push(Region* region);

/**
 * Pop region from thread-local stack (called at region block exit).
 * Also destroys the region.
 */
void region_pop(void);

/**
 * Get the currently active region (top of stack), or NULL if none.
 */
Region* region_current(void);

/* ─── API: Allocation ─── */

/**
 * Allocate memory from the given region (O(1) bump allocation).
 * Memory is zeroed.
 */
void* region_alloc(Region* region, size_t size);

/**
 * Allocate from region with a destructor that runs on region_destroy().
 */
void* region_alloc_with_dtor(Region* region, size_t size,
                              region_dtor_fn dtor);

/**
 * Allocate from the currently active region.
 * Falls back to malloc if no active region.
 */
void* region_alloc_current(size_t size);

/**
 * Duplicate a string into the region.
 */
char* region_strdup(Region* region, const char* str);

/* ─── API: Queries ─── */

/**
 * Check whether an address belongs to a region's memory.
 */
bool region_owns(Region* region, const void* ptr);

/**
 * Get total bytes used by the region.
 */
size_t region_bytes_used(Region* region);

/* ─── API: Debug ─── */

void region_print_stats(Region* region);

#ifdef __cplusplus
}
#endif

#endif /* CASPERIX_REGION_H */
