/**
 * Casperix Runtime - Lightweight Garbage Collector Implementation
 *
 * Generational mark-and-sweep GC for objects that cannot be managed
 * by ownership/ARC alone (e.g. gc-annotated classes with complex
 * reference graphs).
 *
 * Design goals:
 *   - Low latency: incremental marking to avoid long pauses
 *   - Generational: young objects collected frequently, old rarely
 *   - Concurrent scanning where possible
 *   - Minimal overhead when GC is not used
 *
 * This is the SECONDARY memory strategy.  Primary is ARC + cycle collector.
 * GC is only activated for objects explicitly marked `gc class Foo { ... }`.
 */

#include "casprix/gc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* ─── Internal constants ─── */

#define GC_YOUNG_GEN       0
#define GC_OLD_GEN         1
#define GC_INITIAL_ROOTS   64
#define GC_PROMOTE_AGE     3    /* Promote to old gen after N collections   */
#define GC_DEFAULT_LIMIT   (64 * 1024 * 1024)  /* 64 MB default heap limit */
#define GC_DEFAULT_THRESH  (256 * 1024)         /* 256 KB trigger threshold */
#define GC_GROWTH_FACTOR   1.5f

/* ─── Internal: per-object survival tracking ─── */

typedef struct gc_extra {
    uint8_t survive_count;  /* How many collections this object has survived */
} gc_extra_t;

/* Get extra tracking data (stored after type_info in header) */
static gc_extra_t* gc_get_extra(gc_object_header_t* header) {
    /* We pack the survive count into the generation field's upper bits */
    return (gc_extra_t*)((char*)header + sizeof(gc_object_header_t) - sizeof(gc_extra_t));
}

/* ─── Lifecycle ─── */

gc_context_t* nuwan_gc_init(void) {
    gc_context_t* gc = (gc_context_t*)calloc(1, sizeof(gc_context_t));
    if (!gc) return NULL;

    gc->objects       = NULL;
    gc->root_count    = 0;
    gc->root_capacity = GC_INITIAL_ROOTS;
    gc->roots = (gc_object_header_t**)calloc(gc->root_capacity,
                                              sizeof(gc_object_header_t*));
    if (!gc->roots) {
        free(gc);
        return NULL;
    }

    gc->config.heap_size_limit    = GC_DEFAULT_LIMIT;
    gc->config.gc_threshold       = GC_DEFAULT_THRESH;
    gc->config.enable_generational = true;
    gc->config.enable_incremental  = false;  /* Start without incremental */
    gc->config.growth_factor       = GC_GROWTH_FACTOR;

    gc->collecting     = false;
    gc->bytes_since_gc = 0;

    return gc;
}

void nuwan_gc_configure(gc_context_t* gc, gc_config_t* config) {
    if (!gc || !config) return;
    gc->config = *config;
}

void nuwan_gc_shutdown(gc_context_t* gc) {
    if (!gc) return;

    /* Free all remaining objects */
    gc_object_header_t* obj = gc->objects;
    while (obj) {
        gc_object_header_t* next = obj->next;
        gc->stats.total_freed   += obj->size;
        gc->stats.objects_freed++;
        free(obj);
        obj = next;
    }

    free(gc->roots);
    free(gc);
}

/* ─── Allocation ─── */

void* nuwan_gc_alloc(gc_context_t* gc, size_t size, gc_type_descriptor_t* type_desc) {
    if (!gc || size == 0) return NULL;

    /* Check if we should trigger collection */
    if (gc->bytes_since_gc >= gc->config.gc_threshold) {
        nuwan_gc_collect(gc);
    }

    /* Allocate: [gc_object_header_t][user data] */
    size_t total = GC_HEADER_SIZE + size;
    gc_object_header_t* header = (gc_object_header_t*)malloc(total);
    if (!header) return NULL;

    memset(header, 0, total);
    header->size       = size;
    header->marked     = false;
    header->generation = GC_YOUNG_GEN;
    header->type_info  = type_desc;

    /* Link into object list */
    header->next = gc->objects;
    gc->objects  = header;

    /* Update stats */
    gc->stats.total_allocated   += size;
    gc->stats.objects_allocated++;
    gc->stats.bytes_in_use      += size;
    gc->stats.num_objects++;
    gc->bytes_since_gc          += size;

    return GC_HEADER_TO_OBJECT(header);
}

void* nuwan_gc_alloc_array(gc_context_t* gc, size_t element_size, size_t count) {
    return nuwan_gc_alloc(gc, element_size * count, NULL);
}

/* ─── Root management ─── */

void nuwan_gc_add_root(gc_context_t* gc, void* ptr) {
    if (!gc || !ptr) return;

    gc_object_header_t* header = GC_OBJECT_TO_HEADER(ptr);

    /* Grow root array if needed */
    if (gc->root_count >= gc->root_capacity) {
        size_t new_cap = gc->root_capacity * 2;
        gc_object_header_t** new_roots = (gc_object_header_t**)
            realloc(gc->roots, new_cap * sizeof(gc_object_header_t*));
        if (!new_roots) return;
        gc->roots         = new_roots;
        gc->root_capacity = new_cap;
    }

    gc->roots[gc->root_count++] = header;
}

void nuwan_gc_remove_root(gc_context_t* gc, void* ptr) {
    if (!gc || !ptr) return;

    gc_object_header_t* header = GC_OBJECT_TO_HEADER(ptr);

    for (size_t i = 0; i < gc->root_count; i++) {
        if (gc->roots[i] == header) {
            /* Swap with last and shrink */
            gc->roots[i] = gc->roots[gc->root_count - 1];
            gc->root_count--;
            return;
        }
    }
}

/* ─── Mark phase ─── */

static void gc_mark_object(gc_context_t* gc, gc_object_header_t* header) {
    if (!header || header->marked) return;

    header->marked = true;

    /* Scan children via type descriptor */
    gc_type_descriptor_t* type = (gc_type_descriptor_t*)header->type_info;
    if (type && type->scan_func) {
        void* obj = GC_HEADER_TO_OBJECT(header);
        type->scan_func(obj, gc);
    }
}

static void gc_mark_roots(gc_context_t* gc) {
    for (size_t i = 0; i < gc->root_count; i++) {
        gc_mark_object(gc, gc->roots[i]);
    }
}

/* ─── Sweep phase ─── */

static void gc_sweep(gc_context_t* gc, bool full_collection) {
    gc_object_header_t** prev = &gc->objects;
    gc_object_header_t*  obj  = gc->objects;

    while (obj) {
        gc_object_header_t* next = obj->next;

        /* Skip old-gen objects in young-only collections */
        if (!full_collection && gc->config.enable_generational &&
            obj->generation == GC_OLD_GEN) {
            if (obj->marked) obj->marked = false;  /* Reset for next cycle */
            prev = &obj->next;
            obj  = next;
            continue;
        }

        if (obj->marked) {
            /* Alive — reset mark and possibly promote */
            obj->marked = false;

            if (gc->config.enable_generational &&
                obj->generation == GC_YOUNG_GEN) {
                /* Simple promotion heuristic: survived N collections → old */
                /* We use a static counter approximation */
                static int promotion_counter = 0;
                promotion_counter++;
                if (promotion_counter >= GC_PROMOTE_AGE) {
                    obj->generation = GC_OLD_GEN;
                    promotion_counter = 0;
                }
            }

            prev = &obj->next;
        } else {
            /* Dead — unlink and free */
            *prev = next;

            gc->stats.total_freed   += obj->size;
            gc->stats.objects_freed++;
            gc->stats.bytes_in_use  -= obj->size;
            gc->stats.num_objects--;

            free(obj);
        }

        obj = next;
    }
}

/* ─── Collection ─── */

void nuwan_gc_collect(gc_context_t* gc) {
    if (!gc || gc->collecting) return;

    gc->collecting = true;
    gc->stats.collections_performed++;

    /* Mark all reachable objects from roots */
    gc_mark_roots(gc);

    /* Sweep: young generation only (minor collection) */
    gc_sweep(gc, false);

    /* Adjust threshold using growth factor */
    if (gc->stats.bytes_in_use > gc->config.gc_threshold / 2) {
        gc->config.gc_threshold = (size_t)(
            (double)gc->stats.bytes_in_use * gc->config.growth_factor);
    }

    gc->bytes_since_gc = 0;
    gc->collecting = false;
}

void nuwan_gc_collect_full(gc_context_t* gc) {
    if (!gc || gc->collecting) return;

    gc->collecting = true;
    gc->stats.collections_performed++;

    /* Mark from roots */
    gc_mark_roots(gc);

    /* Full sweep: includes old generation */
    gc_sweep(gc, true);

    gc->bytes_since_gc = 0;
    gc->collecting = false;
}

/* ─── Statistics ─── */

gc_stats_t nuwan_gc_get_stats(gc_context_t* gc) {
    gc_stats_t empty = {0};
    return gc ? gc->stats : empty;
}

void nuwan_gc_print_stats(gc_context_t* gc) {
    if (!gc) return;
    printf("=== GC Statistics ===\n");
    printf("Collections:      %llu\n", (unsigned long long)gc->stats.collections_performed);
    printf("Objects allocated: %llu\n", (unsigned long long)gc->stats.objects_allocated);
    printf("Objects freed:     %llu\n", (unsigned long long)gc->stats.objects_freed);
    printf("Objects alive:     %llu\n", (unsigned long long)gc->stats.num_objects);
    printf("Bytes allocated:   %llu\n", (unsigned long long)gc->stats.total_allocated);
    printf("Bytes freed:       %llu\n", (unsigned long long)gc->stats.total_freed);
    printf("Bytes in use:      %llu\n", (unsigned long long)gc->stats.bytes_in_use);
    printf("=====================\n");
}

/* ─── Type registration (global table) ─── */

#define MAX_GC_TYPES 128
static gc_type_descriptor_t g_gc_types[MAX_GC_TYPES];
static int g_gc_type_count = 0;

void nuwan_gc_register_type(const char* name, size_t size,
                            void (*scan_func)(void*, gc_context_t*)) {
    if (g_gc_type_count >= MAX_GC_TYPES) return;
    gc_type_descriptor_t* td = &g_gc_types[g_gc_type_count++];
    td->name      = name;
    td->size      = size;
    td->scan_func = scan_func;
}

/* ─── Default scan functions ─── */

void nuwan_gc_scan_class(void* obj, gc_context_t* gc) {
    /* Generic class scanner — subclasses should register their own.
       This serves as a no-op fallback. */
    (void)obj;
    (void)gc;
}

void nuwan_gc_scan_array(void* obj, gc_context_t* gc) {
    /* Arrays of primitives don't need scanning.
       Arrays of references would be scanned by a type-specific scanner. */
    (void)obj;
    (void)gc;
}

void nuwan_gc_scan_string(void* obj, gc_context_t* gc) {
    /* Strings never contain references — no-op */
    (void)obj;
    (void)gc;
}
