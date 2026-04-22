#include "cx_arena.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <malloc.h>
#endif

static size_t cx_align_up(size_t value, size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

CxArena* cx_arena_create(size_t capacity) {
    if (capacity == 0) return NULL;
    CxArena* arena = (CxArena*)calloc(1, sizeof(CxArena));
    if (!arena) return NULL;

#ifdef _WIN32
    arena->base = (uint8_t*)_aligned_malloc(capacity, 64);
    if (!arena->base) {
        free(arena);
        return NULL;
    }
#else
    if (posix_memalign((void**)&arena->base, 64, capacity) != 0) {
        free(arena);
        return NULL;
    }
#endif

    arena->capacity = capacity;
    atomic_store_explicit(&arena->offset, 0, memory_order_relaxed);
    memset(arena->base, 0, capacity);
    return arena;
}

void cx_arena_destroy(CxArena* arena) {
    if (!arena) return;
#ifdef _WIN32
    _aligned_free(arena->base);
#else
    free(arena->base);
#endif
    free(arena);
}

void* cx_arena_alloc_aligned(CxArena* arena, size_t size, size_t alignment) {
    if (!arena || size == 0 || alignment == 0) return NULL;
    assert((alignment & (alignment - 1u)) == 0 && "alignment must be power-of-two");

    size_t old = atomic_load_explicit(&arena->offset, memory_order_relaxed);
    while (1) {
        size_t aligned = cx_align_up(old, alignment);
        size_t next = aligned + size;
        if (next > arena->capacity) return NULL;
        if (atomic_compare_exchange_weak_explicit(&arena->offset, &old, next,
                                                  memory_order_acq_rel,
                                                  memory_order_relaxed)) {
            void* out = arena->base + aligned;
            memset(out, 0, size);
            return out;
        }
    }
}

void* cx_arena_alloc(CxArena* arena, size_t size) {
    return cx_arena_alloc_aligned(arena, size, 16);
}

void cx_arena_reset(CxArena* arena) {
    if (!arena) return;
    atomic_store_explicit(&arena->offset, 0, memory_order_release);
}

size_t cx_arena_used(const CxArena* arena) {
    if (!arena) return 0;
    return atomic_load_explicit(&arena->offset, memory_order_acquire);
}
