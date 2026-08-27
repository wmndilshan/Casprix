// Casperix Runtime - Unified Memory Manager Implementation

#include "memory.h"
#include "tlocal_heap.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// MinGW printf compatibility: use %llu with casts for size_t
#define FMT_SIZE "%llu"
#define SZ(x) ((unsigned long long)(x))

static bool add_sizes(size_t lhs, size_t rhs, size_t* out) {
    if (!out || lhs > SIZE_MAX - rhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

static bool mul_sizes(size_t lhs, size_t rhs, size_t* out) {
    if (!out) {
        return false;
    }
    if (lhs != 0 && rhs > SIZE_MAX / lhs) {
        return false;
    }
    *out = lhs * rhs;
    return true;
}

// --- Lifecycle ---

MemoryManager* mem_init(void) {
    CycleGCConfig default_config = {
        .collection_threshold = CYCLE_GC_DEFAULT_THRESHOLD,
        .max_suspects = CYCLE_GC_DEFAULT_MAX_SUSPECTS,
        .enabled = true
    };
    return mem_init_with_config(default_config);
}

MemoryManager* mem_init_with_config(CycleGCConfig cycle_config) {
    MemoryManager* mm = (MemoryManager*)malloc(sizeof(MemoryManager));
    if (!mm) return NULL;

    mm->compiler_arena = arena_create();
    if (!mm->compiler_arena) {
        free(mm);
        return NULL;
    }

    mm->cycle_collector = cycle_gc_create_with_config(cycle_config);
    if (!mm->cycle_collector) {
        arena_destroy(mm->compiler_arena);
        free(mm);
        return NULL;
    }

    mm->total_arc_objects = 0;
    mm->total_arena_bytes = 0;
    mm->total_stack_allocs = 0;

    // Initialize borrow checker in debug mode
    borrow_checker_init();

    // Reset ARC stats for clean start
    arc_reset_stats();

    return mm;
}

void mem_shutdown(MemoryManager* mm) {
    if (!mm) return;

    // Run final cycle collection
    if (mm->cycle_collector) {
        cycle_gc_collect(mm->cycle_collector);
    }

    // Check for leaks
    ArcStats arc_stats = arc_get_stats();
    if (arc_stats.current_objects > 0) {
        fprintf(stderr, "\n=== MEMORY LEAK REPORT ===\n");
        fprintf(stderr, "Leaked objects: " FMT_SIZE "\n", SZ(arc_stats.current_objects));
        fprintf(stderr, "Leaked bytes:   " FMT_SIZE "\n", SZ(arc_stats.current_bytes));
        fprintf(stderr, "==========================\n\n");
    }

    // Shutdown subsystems
    borrow_checker_shutdown();

    // Free global slab pool — was silently leaked on every run
    tlocal_heap_shutdown_global();

    if (mm->cycle_collector) {
        cycle_gc_destroy(mm->cycle_collector);
    }

    if (mm->compiler_arena) {
        arena_destroy(mm->compiler_arena);
    }

    free(mm);
}

// --- Compiler arena allocation ---

void* mem_arena_alloc(MemoryManager* mm, size_t size) {
    if (!mm || !mm->compiler_arena) return NULL;
    void* ptr = arena_alloc(mm->compiler_arena, size);
    if (ptr) mm->total_arena_bytes += size;
    return ptr;
}

void* mem_arena_calloc(MemoryManager* mm, size_t count, size_t size) {
    if (!mm || !mm->compiler_arena) return NULL;
    void* ptr = arena_calloc(mm->compiler_arena, count, size);
    if (ptr) mm->total_arena_bytes += count * size;
    return ptr;
}

char* mem_arena_strdup(MemoryManager* mm, const char* str) {
    if (!mm || !mm->compiler_arena || !str) return NULL;
    char* dup = arena_strdup(mm->compiler_arena, str);
    if (dup) mm->total_arena_bytes += strlen(str) + 1;
    return dup;
}

void mem_arena_reset(MemoryManager* mm) {
    if (!mm || !mm->compiler_arena) return;
    arena_reset(mm->compiler_arena);
}

// --- ARC allocation ---

void* mem_arc_alloc(MemoryManager* mm, size_t size) {
    if (!mm) return arc_alloc(size);  // Fallback to direct ARC
    mm->total_arc_objects++;
    return arc_alloc(size);
}

void* mem_arc_alloc_with_destructor(MemoryManager* mm, size_t size,
                                    arc_destructor_fn destructor) {
    if (!mm) return arc_alloc_with_destructor(size, destructor);
    mm->total_arc_objects++;
    return arc_alloc_with_destructor(size, destructor);
}

void* mem_arc_alloc_full(MemoryManager* mm, size_t size,
                         arc_destructor_fn destructor,
                         arc_scan_fn scanner) {
    if (!mm) return arc_alloc_full(size, destructor, scanner);
    mm->total_arc_objects++;
    return arc_alloc_full(size, destructor, scanner);
}

void* mem_arc_retain(MemoryManager* mm, void* obj) {
    (void)mm;
    return arc_retain(obj);
}

void mem_arc_release(MemoryManager* mm, void* obj) {
    if (!obj) return;

    ArcHeader* header = arc_get_header(obj);

    // Check if this might create a cycle suspect
    // We need to check BEFORE releasing, because release might free
    bool has_scanner = header && header->scanner != NULL;
    bool might_survive = header && arc_strong_count(obj) > 1;
    bool survived = arc_release_survived(obj);

    // If the object survived release and can contain refs, add as suspect
    if (survived && has_scanner && might_survive && mm && mm->cycle_collector) {
        cycle_gc_add_suspect(mm->cycle_collector, obj);
        cycle_gc_collect_if_needed(mm->cycle_collector);
    }
}

// --- String allocation ---

// String layout: [length: size_t][data: char[]][null terminator]
typedef struct {
    size_t length;
    size_t capacity;
    char data[];  // Flexible array member
} StringRepr;

static void string_destructor(void* obj) {
    // StringRepr is inline, nothing extra to free
    (void)obj;
}

void* mem_alloc_string(MemoryManager* mm, const char* data, size_t length) {
    size_t alloc_size;

    if (!add_sizes(sizeof(StringRepr), length, &alloc_size) ||
        !add_sizes(alloc_size, 1, &alloc_size)) {
        return NULL;
    }
    void* obj = mem_arc_alloc_with_destructor(mm, alloc_size, string_destructor);
    if (!obj) return NULL;

    StringRepr* str = (StringRepr*)obj;
    str->length = length;
    str->capacity = length;
    if (data) {
        memcpy(str->data, data, length);
    }
    str->data[length] = '\0';

    // Strings can never form cycles
    arc_mark_acyclic(obj);

    return obj;
}

void* mem_alloc_strbuf(MemoryManager* mm, size_t initial_capacity) {
    size_t alloc_size;

    if (initial_capacity < 16) initial_capacity = 16;

    if (!add_sizes(sizeof(StringRepr), initial_capacity, &alloc_size) ||
        !add_sizes(alloc_size, 1, &alloc_size)) {
        return NULL;
    }
    void* obj = mem_arc_alloc_with_destructor(mm, alloc_size, string_destructor);
    if (!obj) return NULL;

    StringRepr* str = (StringRepr*)obj;
    str->length = 0;
    str->capacity = initial_capacity;
    str->data[0] = '\0';

    arc_mark_acyclic(obj);

    return obj;
}

// --- Array allocation ---

// Array layout: [length: size_t][capacity: size_t][elements...]
typedef struct {
    size_t length;
    size_t capacity;
    size_t element_size;
    char data[];  // Flexible array member
} ArrayRepr;

static void array_destructor(void* obj) {
    (void)obj;
}

void* mem_alloc_array(MemoryManager* mm, size_t element_size, size_t count,
                      arc_scan_fn scanner) {
    size_t data_size;
    size_t alloc_size;

    if (!mul_sizes(element_size, count, &data_size) ||
        !add_sizes(sizeof(ArrayRepr), data_size, &alloc_size)) {
        return NULL;
    }

    void* obj;
    if (scanner) {
        obj = mem_arc_alloc_full(mm, alloc_size, array_destructor, scanner);
    } else {
        obj = mem_arc_alloc_with_destructor(mm, alloc_size, array_destructor);
        // Arrays of primitives can't form cycles
        arc_mark_acyclic(obj);
    }

    if (!obj) return NULL;

    ArrayRepr* arr = (ArrayRepr*)obj;
    arr->length = count;
    arr->capacity = count;
    arr->element_size = element_size;

    return obj;
}

// --- Cycle collection ---

void mem_collect_cycles(MemoryManager* mm) {
    if (!mm || !mm->cycle_collector) return;
    cycle_gc_collect_if_needed(mm->cycle_collector);
}

void mem_force_collect_cycles(MemoryManager* mm) {
    if (!mm || !mm->cycle_collector) return;
    cycle_gc_collect(mm->cycle_collector);
}

// --- Diagnostics ---

MemoryStats mem_get_stats(MemoryManager* mm) {
    MemoryStats stats;
    memset(&stats, 0, sizeof(MemoryStats));

    stats.arc = arc_get_stats();

    if (mm) {
        if (mm->cycle_collector) {
            stats.cycle_gc = cycle_gc_get_stats(mm->cycle_collector);
        }
        if (mm->compiler_arena) {
            arena_stats(mm->compiler_arena,
                       &stats.arena_allocated,
                       &stats.arena_used,
                       &stats.arena_blocks);
        }
    }

    return stats;
}

void mem_print_stats(MemoryManager* mm) {
    MemoryStats stats = mem_get_stats(mm);

    printf("=== Casperix Memory Manager Stats ===\n");
    printf("ARC Objects:\n");
    printf("  Allocated:     " FMT_SIZE "\n", SZ(stats.arc.total_allocations));
    printf("  Freed:         " FMT_SIZE "\n", SZ(stats.arc.total_frees));
    printf("  Current:       " FMT_SIZE "\n", SZ(stats.arc.current_objects));
    printf("  Bytes in use:  " FMT_SIZE "\n", SZ(stats.arc.current_bytes));
    printf("  Retains:       " FMT_SIZE "\n", SZ(stats.arc.total_retains));
    printf("  Releases:      " FMT_SIZE "\n", SZ(stats.arc.total_releases));
    printf("Cycle Collector:\n");
    printf("  Collections:   " FMT_SIZE "\n", SZ(stats.cycle_gc.collections_run));
    printf("  Cycles found:  " FMT_SIZE "\n", SZ(stats.cycle_gc.cycles_found));
    printf("  Objects freed: " FMT_SIZE "\n", SZ(stats.cycle_gc.objects_collected));
    printf("Compiler Arena:\n");
    printf("  Allocated:     " FMT_SIZE "\n", SZ(stats.arena_allocated));
    printf("  Used:          " FMT_SIZE "\n", SZ(stats.arena_used));
    printf("  Blocks:        " FMT_SIZE "\n", SZ(stats.arena_blocks));
    printf("=====================================\n");
}

void mem_print_leak_report(MemoryManager* mm) {
    (void)mm;
    ArcStats stats = arc_get_stats();

    if (stats.current_objects == 0) {
        printf("No memory leaks detected.\n");
        return;
    }

    printf("\n=== MEMORY LEAK REPORT ===\n");
    printf("Leaked objects: " FMT_SIZE "\n", SZ(stats.current_objects));
    printf("Leaked bytes:   " FMT_SIZE "\n", SZ(stats.current_bytes));
    printf("\nBreakdown:\n");
    printf("  Total allocated:  " FMT_SIZE " objects\n", SZ(stats.total_allocations));
    printf("  Total freed:      " FMT_SIZE " objects\n", SZ(stats.total_frees));
    printf("  Retains:          " FMT_SIZE "\n", SZ(stats.total_retains));
    printf("  Releases:         " FMT_SIZE "\n", SZ(stats.total_releases));
    printf("  Balance:          %+lld (should be 0)\n",
           (long long)(stats.total_retains - stats.total_releases));
    printf("===========================\n\n");
}
