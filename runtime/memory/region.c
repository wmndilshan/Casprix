/**
 * Casperix Runtime - Region Allocator Implementation
 *
 * Lock-free bump allocator with per-region destructor chains.
 * Thread-local region stack enables zero-argument allocation
 * from the current region context.
 */

#include "region.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* ─── Thread-local region stack ─── */

#ifdef _MSC_VER
    __declspec(thread) static RegionStack tls_region_stack = { {NULL}, 0 };
#elif defined(__GNUC__) || defined(__clang__)
    static __thread RegionStack tls_region_stack = { {NULL}, 0 };
#else
    static RegionStack tls_region_stack = { {NULL}, 0 };
#endif

/* ─── Internal helpers ─── */

static size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static RegionBlock* region_block_create(size_t min_capacity) {
    size_t cap = min_capacity > REGION_DEFAULT_SIZE
                 ? min_capacity : REGION_DEFAULT_SIZE;

    RegionBlock* block = (RegionBlock*)malloc(sizeof(RegionBlock));
    if (!block) return NULL;

    block->memory   = (uint8_t*)malloc(cap);
    if (!block->memory) {
        free(block);
        return NULL;
    }

    block->capacity = cap;
    block->used     = 0;
    block->next     = NULL;
    return block;
}

static void region_block_destroy(RegionBlock* block) {
    while (block) {
        RegionBlock* next = block->next;
        free(block->memory);
        free(block);
        block = next;
    }
}

/* ─── Region lifecycle ─── */

Region* region_create(const char* name, size_t initial_capacity) {
    Region* r = (Region*)malloc(sizeof(Region));
    if (!r) return NULL;

    size_t cap = initial_capacity > 0 ? initial_capacity : REGION_DEFAULT_SIZE;

    r->first_block     = region_block_create(cap);
    if (!r->first_block) {
        free(r);
        return NULL;
    }

    r->current_block   = r->first_block;
    r->name            = name;
    r->destructors     = NULL;
    r->total_allocated = cap;
    r->total_used      = 0;
    r->num_allocs      = 0;
    r->block_count     = 1;

    return r;
}

void region_destroy(Region* region) {
    if (!region) return;

    /* Phase 1: Run destructors in LIFO order */
    RegionDtor* dtor = region->destructors;
    while (dtor) {
        RegionDtor* next = dtor->next;
        if (dtor->dtor && dtor->obj) {
            dtor->dtor(dtor->obj);
        }
        /* RegionDtor nodes are themselves region-allocated,
           so they'll be freed with the blocks below.
           But if they were malloc'd, free them. */
        free(dtor);
        dtor = next;
    }

    /* Phase 2: Free all memory blocks */
    region_block_destroy(region->first_block);

    free(region);
}

/* ─── Region stack ─── */

void region_push(Region* region) {
    assert(tls_region_stack.depth < REGION_MAX_NESTING &&
           "Region nesting limit exceeded");
    tls_region_stack.stack[tls_region_stack.depth++] = region;
}

void region_pop(void) {
    assert(tls_region_stack.depth > 0 && "Region stack underflow");
    tls_region_stack.depth--;
    Region* r = tls_region_stack.stack[tls_region_stack.depth];
    tls_region_stack.stack[tls_region_stack.depth] = NULL;
    region_destroy(r);
}

Region* region_current(void) {
    if (tls_region_stack.depth <= 0) return NULL;
    return tls_region_stack.stack[tls_region_stack.depth - 1];
}

/* ─── Allocation ─── */

void* region_alloc(Region* region, size_t size) {
    if (!region || size == 0) return NULL;

    size = align_up(size, REGION_ALIGNMENT);

    RegionBlock* block = region->current_block;

    /* Fast path: bump within current block */
    if (block->used + size <= block->capacity) {
        void* ptr = block->memory + block->used;
        block->used += size;
        region->total_used += size;
        region->num_allocs++;
        memset(ptr, 0, size);
        return ptr;
    }

    /* Slow path: allocate a new block */
    RegionBlock* new_block = region_block_create(size);
    if (!new_block) return NULL;

    block->next          = new_block;
    region->current_block = new_block;
    region->total_allocated += new_block->capacity;
    region->block_count++;

    void* ptr = new_block->memory;
    new_block->used = size;
    region->total_used += size;
    region->num_allocs++;
    memset(ptr, 0, size);
    return ptr;
}

void* region_alloc_with_dtor(Region* region, size_t size,
                              region_dtor_fn dtor) {
    void* obj = region_alloc(region, size);
    if (!obj || !dtor) return obj;

    /* Register destructor (malloc'd, since the region memory
       will be freed *after* destructors run) */
    RegionDtor* rec = (RegionDtor*)malloc(sizeof(RegionDtor));
    if (!rec) return obj;  /* Object allocated but dtor won't run — best effort */

    rec->obj  = obj;
    rec->dtor = dtor;
    rec->next = region->destructors;  /* LIFO push */
    region->destructors = rec;

    return obj;
}

void* region_alloc_current(size_t size) {
    Region* r = region_current();
    if (r) return region_alloc(r, size);
    /* Bug #5 fix: The original code fell back to malloc() here, returning a
       raw heap pointer that callers cannot distinguish from a region pointer
       and therefore cannot free — silent leak.  Return NULL instead to force
       the caller to push a region before allocating. */
    fprintf(stderr, "region_alloc_current: no active region on stack "
                    "(size=%llu) — push a region first\n",
                    (unsigned long long)size);
    return NULL;
}

char* region_strdup(Region* region, const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str) + 1;
    char* dup = (char*)region_alloc(region, len);
    if (dup) memcpy(dup, str, len);
    return dup;
}

/* ─── Queries ─── */

bool region_owns(Region* region, const void* ptr) {
    if (!region || !ptr) return false;
    const uint8_t* p = (const uint8_t*)ptr;

    RegionBlock* block = region->first_block;
    while (block) {
        if (p >= block->memory && p < block->memory + block->used) {
            return true;
        }
        block = block->next;
    }
    return false;
}

size_t region_bytes_used(Region* region) {
    return region ? region->total_used : 0;
}

/* ─── Debug ─── */

void region_print_stats(Region* region) {
    if (!region) return;
    printf("=== Region '%s' Statistics ===\n",
           region->name ? region->name : "<unnamed>");
    printf("  Blocks:      %llu\n", (unsigned long long)region->block_count);
    printf("  Allocated:   %llu bytes\n", (unsigned long long)region->total_allocated);
    printf("  Used:        %llu bytes\n", (unsigned long long)region->total_used);
    printf("  Allocations: %llu\n", (unsigned long long)region->num_allocs);
    printf("  Utilization: %.1f%%\n",
           region->total_allocated > 0
               ? 100.0 * (double)region->total_used / (double)region->total_allocated
               : 0.0);
    printf("==============================\n");
}
