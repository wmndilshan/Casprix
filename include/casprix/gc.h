// Casprix Garbage Collector
// Mark-and-sweep garbage collection with generational support

#ifndef NUWAN_GC_H
#define NUWAN_GC_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Object header for GC tracking
typedef struct gc_object_header {
    size_t size;                    // Object size in bytes
    bool marked;                    // Mark bit for GC
    uint8_t generation;             // Generation (0=young, 1=old)
    uint8_t survive_count;          // Number of collections survived (for promotion)
    struct gc_object_header* next;  // Next object in allocation list
    void* type_info;                // Type information for scanning
} gc_object_header_t;

// GC statistics
typedef struct {
    size_t total_allocated;         // Total bytes allocated
    size_t total_freed;             // Total bytes freed
    size_t objects_allocated;       // Number of objects allocated
    size_t objects_freed;           // Number of objects freed
    size_t collections_performed;   // Number of GC cycles
    size_t bytes_in_use;            // Current memory usage
    size_t num_objects;             // Current object count
} gc_stats_t;

// GC configuration
typedef struct {
    size_t heap_size_limit;         // Max heap size (0 = unlimited)
    size_t gc_threshold;            // Trigger GC when this much allocated
    bool enable_generational;       // Enable generational collection
    bool enable_incremental;        // Enable incremental collection
    float growth_factor;            // Heap growth factor (1.5 = 150%)
} gc_config_t;

#define GC_MAX_TYPES 128

/* Forward declaration so gc_type_descriptor_t can reference gc_context_t. */
typedef struct gc_context gc_context_t;

// Type descriptor for scanning objects
typedef struct {
    const char* name;
    size_t size;
    void (*scan_func)(void* obj, gc_context_t* gc);
} gc_type_descriptor_t;

// GC context
struct gc_context {
    gc_object_header_t* objects;    // List of all allocated objects
    gc_object_header_t** roots;     // Root set (stack, globals)
    size_t root_count;
    size_t root_capacity;
    gc_stats_t stats;
    gc_config_t config;
    bool collecting;                // True if GC is running
    size_t bytes_since_gc;          // Bytes allocated since last GC
    // Per-context type registry (replaces the old process-global g_gc_types).
    gc_type_descriptor_t* type_table;
    int type_count;
};

// Initialize garbage collector
gc_context_t* nuwan_gc_init(void);

// Configure GC
void nuwan_gc_configure(gc_context_t* gc, gc_config_t* config);

// Allocate memory through GC
void* nuwan_gc_alloc(gc_context_t* gc, size_t size, gc_type_descriptor_t* type_desc);

// Allocate array
void* nuwan_gc_alloc_array(gc_context_t* gc, size_t element_size, size_t count);

// Add/remove roots (GC roots are objects that must not be collected)
void nuwan_gc_add_root(gc_context_t* gc, void* ptr);
void nuwan_gc_remove_root(gc_context_t* gc, void* ptr);

// Run garbage collection
void nuwan_gc_collect(gc_context_t* gc);

// Force full collection
void nuwan_gc_collect_full(gc_context_t* gc);

// Get GC statistics
gc_stats_t nuwan_gc_get_stats(gc_context_t* gc);

// Print GC statistics
void nuwan_gc_print_stats(gc_context_t* gc);

// Shutdown GC and free all memory
void nuwan_gc_shutdown(gc_context_t* gc);

// Helper macros
#define GC_HEADER_SIZE sizeof(gc_object_header_t)
#define GC_OBJECT_TO_HEADER(ptr) ((gc_object_header_t*)((char*)(ptr) - GC_HEADER_SIZE))
#define GC_HEADER_TO_OBJECT(header) ((void*)((char*)(header) + GC_HEADER_SIZE))

// Register type descriptor with a specific GC context (no global state).
void nuwan_gc_register_type(gc_context_t* gc, const char* name, size_t size,
                            void (*scan_func)(void*, gc_context_t*));

// Default scan functions for common types
void nuwan_gc_scan_class(void* obj, gc_context_t* gc);
void nuwan_gc_scan_array(void* obj, gc_context_t* gc);
void nuwan_gc_scan_string(void* obj, gc_context_t* gc);

#ifdef __cplusplus
}
#endif

#endif // NUWAN_GC_H
