/**
 * Casperix Runtime - Thread-Local Heap Allocator
 *
 * Lock-free, per-thread bump allocator with a global slab cache.
 */

#include "tlocal_heap.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─── Platform TLS ─── */
#ifdef _MSC_VER
  #define TLOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
  #define TLOCAL __thread
#else
  #error "No thread-local storage support for this compiler"
#endif

/* ─── Platform atomics (for the global slab pool) ─── */
#ifdef _MSC_VER
  #include <windows.h>
  #define ATOMIC_LOAD(p)       InterlockedCompareExchange((volatile LONG*)(p), 0, 0)
  #define ATOMIC_INC(p)        InterlockedIncrement((volatile LONG*)(p))
  #define ATOMIC_DEC(p)        InterlockedDecrement((volatile LONG*)(p))
  #define SPIN_LOCK(p)         while (InterlockedCompareExchange((volatile LONG*)(p), 1, 0) != 0) { YieldProcessor(); }
  #define SPIN_UNLOCK(p)       InterlockedExchange((volatile LONG*)(p), 0)
#else
  #define ATOMIC_LOAD(p)       __atomic_load_n(p, __ATOMIC_SEQ_CST)
  #define ATOMIC_INC(p)        __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST)
  #define ATOMIC_DEC(p)        __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST)
  #define SPIN_LOCK(p)         while (__atomic_exchange_n(p, 1, __ATOMIC_ACQUIRE)) { /* spin */ }
  #define SPIN_UNLOCK(p)       __atomic_store_n(p, 0, __ATOMIC_RELEASE)
#endif

/* ─── Alignment helper ─── */
static inline size_t align_up(size_t val, size_t align) {
    return (val + align - 1) & ~(align - 1);
}

/* ─── Global slab free-list ─── */
static struct {
    TLocalSlab*     free_list;
    int             free_count;
    volatile long   lock;
    int             initialized;
} g_slab_pool = { NULL, 0, 0, 0 };

/* ─── Thread-local state ─── */
static TLOCAL TLocalHeap t_heap = { NULL, 0, 0, 0 };

/* ─── Slab creation / destruction ─── */

static TLocalSlab* slab_create(void) {
    TLocalSlab* slab = (TLocalSlab*)malloc(sizeof(TLocalSlab));
    if (!slab) return NULL;

    slab->memory   = (uint8_t*)malloc(TLOCAL_SLAB_SIZE);
    if (!slab->memory) {
        free(slab);
        return NULL;
    }
    slab->capacity = TLOCAL_SLAB_SIZE;
    slab->used     = 0;
    slab->next     = NULL;
    return slab;
}

static void slab_destroy(TLocalSlab* slab) {
    if (slab) {
        free(slab->memory);
        free(slab);
    }
}

/* ─── Global pool: acquire / release ─── */

static TLocalSlab* pool_acquire_slab(void) {
    TLocalSlab* slab = NULL;

    SPIN_LOCK(&g_slab_pool.lock);
    if (g_slab_pool.free_list) {
        slab = g_slab_pool.free_list;
        g_slab_pool.free_list = slab->next;
        g_slab_pool.free_count--;
        slab->next = NULL;
        slab->used = 0;   /* reset for reuse */
    }
    SPIN_UNLOCK(&g_slab_pool.lock);

    if (!slab) {
        slab = slab_create();
    }
    return slab;
}

static void pool_release_slab(TLocalSlab* slab) {
    if (!slab) return;

    SPIN_LOCK(&g_slab_pool.lock);
    if (g_slab_pool.free_count < TLOCAL_MAX_FREE_SLABS) {
        slab->next = g_slab_pool.free_list;
        g_slab_pool.free_list = slab;
        g_slab_pool.free_count++;
        SPIN_UNLOCK(&g_slab_pool.lock);
    } else {
        SPIN_UNLOCK(&g_slab_pool.lock);
        slab_destroy(slab);
    }
}

/* ─── Ensure current thread has a slab ─── */

static void ensure_slab(void) {
    if (!t_heap.current_slab) {
        t_heap.current_slab = pool_acquire_slab();
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════ */

void tlocal_heap_init_global(void) {
    if (g_slab_pool.initialized) return;

    g_slab_pool.free_list  = NULL;
    g_slab_pool.free_count = 0;
    g_slab_pool.lock       = 0;
    g_slab_pool.initialized = 1;
}

void tlocal_heap_shutdown_global(void) {
    SPIN_LOCK(&g_slab_pool.lock);
    TLocalSlab* cur = g_slab_pool.free_list;
    while (cur) {
        TLocalSlab* next = cur->next;
        slab_destroy(cur);
        cur = next;
    }
    g_slab_pool.free_list  = NULL;
    g_slab_pool.free_count = 0;
    g_slab_pool.initialized = 0;
    SPIN_UNLOCK(&g_slab_pool.lock);
}

void* tlocal_alloc(size_t size) {
    if (size == 0) return NULL;

    t_heap.num_allocs++;

    /* Large allocation → fall back to malloc */
    if (size > TLOCAL_MAX_ALLOC_SIZE) {
        t_heap.total_fallbacks++;
        void* ptr = malloc(size);
        if (ptr) memset(ptr, 0, size);
        return ptr;
    }

    ensure_slab();
    TLocalSlab* slab = t_heap.current_slab;
    if (!slab) {
        /* Cannot get a slab — fall back */
        t_heap.total_fallbacks++;
        void* ptr = malloc(size);
        if (ptr) memset(ptr, 0, size);
        return ptr;
    }

    size_t aligned = align_up(size, TLOCAL_ALIGNMENT);

    /* If slab is full, get a fresh one */
    if (slab->used + aligned > slab->capacity) {
        pool_release_slab(slab);
        slab = pool_acquire_slab();
        t_heap.current_slab = slab;
        if (!slab) {
            t_heap.total_fallbacks++;
            void* ptr = malloc(size);
            if (ptr) memset(ptr, 0, size);
            return ptr;
        }
    }

    void* ptr = slab->memory + slab->used;
    slab->used += aligned;
    t_heap.total_allocated += aligned;

    memset(ptr, 0, aligned);
    return ptr;
}

void tlocal_reset(void) {
    if (t_heap.current_slab) {
        t_heap.current_slab->used = 0;
    }
}

void tlocal_thread_cleanup(void) {
    if (t_heap.current_slab) {
        pool_release_slab(t_heap.current_slab);
        t_heap.current_slab    = NULL;
        t_heap.total_allocated = 0;
        t_heap.total_fallbacks = 0;
        t_heap.num_allocs      = 0;
    }
}

TLocalHeap* tlocal_get_heap(void) {
    return &t_heap;
}

void tlocal_print_stats(void) {
    printf("=== Thread-Local Heap Statistics ===\n");
    printf("  Allocations:    %llu\n", (unsigned long long)t_heap.num_allocs);
    printf("  Slab bytes:     %llu / %d\n",
           (unsigned long long)(t_heap.current_slab ? t_heap.current_slab->used : 0),
           TLOCAL_SLAB_SIZE);
    printf("  Total from slab:%llu bytes\n", (unsigned long long)t_heap.total_allocated);
    printf("  Fallback allocs:%llu\n", (unsigned long long)t_heap.total_fallbacks);

    SPIN_LOCK(&g_slab_pool.lock);
    printf("  Pooled slabs:   %d / %d\n",
           g_slab_pool.free_count, TLOCAL_MAX_FREE_SLABS);
    SPIN_UNLOCK(&g_slab_pool.lock);
}
