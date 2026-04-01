// Casperix Runtime - ARC Implementation
// Thread-safe automatic reference counting

#include "arc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

// Platform-specific atomics
#ifdef _MSC_VER
    #include <intrin.h>
    #define ATOMIC_INC(x)  _InterlockedIncrement((volatile long*)(x))
    #define ATOMIC_DEC(x)  _InterlockedDecrement((volatile long*)(x))
    #define ATOMIC_LOAD(x) (*(volatile long*)(x))
#elif defined(__GNUC__) || defined(__clang__)
    #define ATOMIC_INC(x)  __sync_add_and_fetch((x), 1)
    #define ATOMIC_DEC(x)  __sync_sub_and_fetch((x), 1)
    #define ATOMIC_LOAD(x) __sync_add_and_fetch((x), 0)
#else
    // Fallback: non-atomic (single-threaded only)
    #define ATOMIC_INC(x)  (++(*(x)))
    #define ATOMIC_DEC(x)  (--(*(x)))
    #define ATOMIC_LOAD(x) (*(x))
#endif

// Global ARC statistics
static ArcStats g_arc_stats = {0};

// --- Internal helpers ---

static inline ArcHeader* get_header(void* obj) {
    assert(obj != NULL);
    return ARC_OBJ_TO_HEADER(obj);
}

static inline const ArcHeader* get_header_const(const void* obj) {
    assert(obj != NULL);
    return (const ArcHeader*)((const char*)(obj) - ARC_HEADER_SIZE);
}

static void arc_dealloc(ArcHeader* header) {
    // Call destructor if registered
    if ((header->flags & ARC_FLAG_HAS_DESTRUCTOR) && header->destructor) {
        void* obj = ARC_HEADER_TO_OBJ(header);
        header->destructor(obj);
    }

    g_arc_stats.total_frees++;
    g_arc_stats.current_objects--;
    g_arc_stats.current_bytes -= header->size;

    // If no weak references exist, free immediately
    if (header->weak_count <= 1) {
        free(header);
    } else {
        // Weak references still exist - decrement the weak count
        // that the strong refs were holding, but keep the header alive
        ATOMIC_DEC(&header->weak_count);
        // Zero out the user data to prevent use-after-free
        void* obj = ARC_HEADER_TO_OBJ(header);
        memset(obj, 0, header->size);
    }
}

// --- Core ARC API ---

void* arc_alloc(size_t size) {
    return arc_alloc_full(size, NULL, NULL);
}

void* arc_alloc_with_destructor(size_t size, arc_destructor_fn destructor) {
    return arc_alloc_full(size, destructor, NULL);
}

void* arc_alloc_full(size_t size, arc_destructor_fn destructor, arc_scan_fn scanner) {
    if (size == 0) return NULL;

    size_t total = ARC_HEADER_SIZE + size;
    ArcHeader* header = (ArcHeader*)malloc(total);
    if (!header) return NULL;

    // Initialize header
    header->strong_count = 1;
    header->weak_count = 1;  // +1 held by strong refs collectively
    header->flags = ARC_FLAG_NONE;
    header->size = (uint32_t)size;
    header->color = ARC_COLOR_BLACK;
    header->destructor = NULL;
    header->scanner = NULL;

    if (destructor) {
        header->destructor = destructor;
        header->flags |= ARC_FLAG_HAS_DESTRUCTOR;
    }

    if (scanner) {
        header->scanner = scanner;
    }

    // Zero-initialize user data
    void* obj = ARC_HEADER_TO_OBJ(header);
    memset(obj, 0, size);

    // Update stats
    g_arc_stats.total_allocations++;
    g_arc_stats.current_objects++;
    g_arc_stats.current_bytes += size;

    return obj;
}

void* arc_retain(void* obj) {
    if (!obj) return NULL;

    ArcHeader* header = get_header(obj);
    assert(header->strong_count > 0 && "arc_retain on dead object");
    assert(!(header->flags & ARC_FLAG_MOVED) && "arc_retain on moved object");

    ATOMIC_INC(&header->strong_count);
    g_arc_stats.total_retains++;

    return obj;
}

void arc_release(void* obj) {
    if (!obj) return;

    ArcHeader* header = get_header(obj);
    assert(header->strong_count > 0 && "arc_release on dead object");

    g_arc_stats.total_releases++;

    int32_t new_count = ATOMIC_DEC(&header->strong_count);
    if (new_count == 0) {
        // Last strong reference dropped - deallocate
        arc_dealloc(header);
    } else if (new_count < 0) {
        // Over-release detected
        fprintf(stderr, "ARC ERROR: over-release detected (strong_count went negative)\n");
        abort();
    }
}

int32_t arc_strong_count(const void* obj) {
    if (!obj) return 0;
    // Cast away const for atomic load - read-only operation is safe
    ArcHeader* header = (ArcHeader*)get_header_const(obj);
    return ATOMIC_LOAD(&header->strong_count);
}

// --- Weak References ---

ArcWeak arc_weak_create(void* obj) {
    ArcWeak weak = { NULL };
    if (!obj) return weak;

    ArcHeader* header = get_header(obj);
    assert(header->strong_count > 0 && "arc_weak_create on dead object");

    ATOMIC_INC(&header->weak_count);
    header->flags |= ARC_FLAG_HAS_WEAK;

    weak.header = header;
    g_arc_stats.weak_refs_created++;

    return weak;
}

void* arc_weak_upgrade(ArcWeak weak) {
    if (!weak.header) {
        g_arc_stats.weak_upgrade_fails++;
        return NULL;
    }

    // Try to increment strong count, but only if it's currently > 0
    // This is a simplified version; a fully lock-free implementation
    // would use CAS loop
    int32_t count = ATOMIC_LOAD(&weak.header->strong_count);
    if (count <= 0) {
        g_arc_stats.weak_upgrade_fails++;
        return NULL;
    }

    ATOMIC_INC(&weak.header->strong_count);
    g_arc_stats.total_retains++;
    g_arc_stats.weak_upgrades++;

    return ARC_HEADER_TO_OBJ(weak.header);
}

void arc_weak_release(ArcWeak* weak) {
    if (!weak || !weak->header) return;

    ArcHeader* header = weak->header;
    weak->header = NULL;

    int32_t new_weak = ATOMIC_DEC(&header->weak_count);
    if (new_weak == 0 && ATOMIC_LOAD(&header->strong_count) <= 0) {
        /* Both strong and weak counts are zero - free the header.
           Use ATOMIC_LOAD for strong_count to avoid a TOCTOU race
           in multithreaded contexts (Bug #6 fix). */
        free(header);
    }
}

bool arc_weak_is_alive(ArcWeak weak) {
    if (!weak.header) return false;
    return ATOMIC_LOAD(&weak.header->strong_count) > 0;
}

// --- Object Info ---

ArcHeader* arc_get_header(void* obj) {
    if (!obj) return NULL;
    return get_header(obj);
}

const ArcHeader* arc_get_header_const(const void* obj) {
    if (!obj) return NULL;
    return get_header_const(obj);
}

bool arc_is_moved(const void* obj) {
    if (!obj) return false;
    return (get_header_const(obj)->flags & ARC_FLAG_MOVED) != 0;
}

void arc_mark_moved(void* obj) {
    if (!obj) return;
    get_header(obj)->flags |= ARC_FLAG_MOVED;
}

void arc_mark_acyclic(void* obj) {
    if (!obj) return;
    ArcHeader* header = get_header(obj);
    header->flags |= ARC_FLAG_ACYCLIC;
    header->color = ARC_COLOR_GREEN;
}

// --- Statistics ---

ArcStats arc_get_stats(void) {
    return g_arc_stats;
}

void arc_reset_stats(void) {
    memset(&g_arc_stats, 0, sizeof(ArcStats));
}

/* Called by the cycle collector when it frees ARC-managed objects directly
   (bypassing arc_release) so that global stats stay accurate. */
void arc_notify_freed(size_t size) {
    g_arc_stats.total_frees++;
    if (g_arc_stats.current_objects > 0) g_arc_stats.current_objects--;
    if (g_arc_stats.current_bytes >= size) g_arc_stats.current_bytes -= size;
}

void arc_print_stats(void) {
    printf("=== ARC Statistics ===\n");
    printf("Total allocations:   %llu\n", (unsigned long long)g_arc_stats.total_allocations);
    printf("Total frees:         %llu\n", (unsigned long long)g_arc_stats.total_frees);
    printf("Current objects:     %llu\n", (unsigned long long)g_arc_stats.current_objects);
    printf("Current bytes:       %llu\n", (unsigned long long)g_arc_stats.current_bytes);
    printf("Total retains:       %llu\n", (unsigned long long)g_arc_stats.total_retains);
    printf("Total releases:      %llu\n", (unsigned long long)g_arc_stats.total_releases);
    printf("Weak refs created:   %llu\n", (unsigned long long)g_arc_stats.weak_refs_created);
    printf("Weak upgrades (ok):  %llu\n", (unsigned long long)g_arc_stats.weak_upgrades);
    printf("Weak upgrades (fail):%llu\n", (unsigned long long)g_arc_stats.weak_upgrade_fails);
    printf("=======================\n");
}
