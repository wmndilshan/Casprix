// Casperix Runtime - Cycle Collector
// Lightweight cycle detection for ARC-managed objects
//
// Uses the Bacon-Rajan synchronous cycle collection algorithm:
//   1. When an ARC object's refcount decrements but doesn't reach zero,
//      and the object has a scanner (can contain references), it becomes
//      a "suspect" (potential cycle root)
//   2. When the suspect buffer reaches a threshold, collection runs:
//      a. Mark roots (purple suspects)
//      b. Scan: trial-decrement strong counts through reference graph
//      c. Collect: anything that reached zero during trial is garbage
//      d. Restore: undo trial decrements for live objects
//
// Objects marked ARC_FLAG_ACYCLIC are never added as suspects.
// The collector only runs on objects that have an arc_scan_fn registered.

#ifndef CASPERIX_CYCLE_GC_H
#define CASPERIX_CYCLE_GC_H

#include "arc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Cycle collector configuration
typedef struct {
    int collection_threshold;   // Collect when this many suspects buffered
    int max_suspects;           // Max buffer size before forced collection
    bool enabled;               // Enable/disable cycle collection
} CycleGCConfig;

// Cycle collector statistics
typedef struct {
    size_t collections_run;     // Number of collection cycles
    size_t suspects_buffered;   // Total suspects ever buffered
    size_t cycles_found;        // Total cycles detected
    size_t objects_collected;   // Total objects freed by cycle collector
    size_t bytes_collected;     // Total bytes freed
} CycleGCStats;

// Cycle collector context
typedef struct CycleCollector {
    void** suspects;            // Buffer of suspect object pointers (user data ptrs)
    int suspect_count;          // Current number of suspects
    int suspect_capacity;       // Buffer capacity
    CycleGCConfig config;       // Configuration
    CycleGCStats stats;         // Statistics
    bool collecting;            // Guard against re-entrant collection
} CycleCollector;

// --- Lifecycle ---

// Create and initialize cycle collector with default settings
CycleCollector* cycle_gc_create(void);

// Create with custom configuration
CycleCollector* cycle_gc_create_with_config(CycleGCConfig config);

// Destroy cycle collector
void cycle_gc_destroy(CycleCollector* cc);

// --- Suspect management ---

// Add an object as a potential cycle root
// Called by arc_release when count decrements but stays > 0
// Only adds objects that have a scanner function
void cycle_gc_add_suspect(CycleCollector* cc, void* obj);

// --- Collection ---

// Run cycle collection if threshold reached
// Returns number of objects collected
int cycle_gc_collect_if_needed(CycleCollector* cc);

// Force a collection cycle regardless of threshold
// Returns number of objects collected
int cycle_gc_collect(CycleCollector* cc);

// --- Configuration ---

// Set collection threshold
void cycle_gc_set_threshold(CycleCollector* cc, int threshold);

// Enable or disable cycle collection
void cycle_gc_set_enabled(CycleCollector* cc, bool enabled);

// --- Statistics ---

// Get current statistics
CycleGCStats cycle_gc_get_stats(CycleCollector* cc);

// Print statistics
void cycle_gc_print_stats(CycleCollector* cc);

// Default configuration values
#define CYCLE_GC_DEFAULT_THRESHOLD    100
#define CYCLE_GC_DEFAULT_MAX_SUSPECTS 4096

#ifdef __cplusplus
}
#endif

#endif // CASPERIX_CYCLE_GC_H
