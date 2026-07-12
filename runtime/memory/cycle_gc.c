// Casperix Runtime - Cycle Collector Implementation
// Bacon-Rajan synchronous cycle collection

#include "cycle_gc.h"
#include "arc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


// --- Internal: trial deletion helpers ---

static void cycle_mark_gray(void* obj);
static void cycle_scan_live(void* obj);

// Decrement trial count during mark phase
static void cycle_scan_decrement(void* field_ptr) {
    if (!field_ptr || arc_is_deallocating(field_ptr)) return;
    ArcHeader* header = arc_get_header(field_ptr);
    if (!header) return;
    if (header->color == ARC_COLOR_GREEN) {
        return;
    }

    if (header->strong_count <= 0) {
        return;
    }
    header->strong_count--;
    cycle_mark_gray(field_ptr);
}

static void cycle_mark_gray(void* obj) {
    ArcHeader* header;

    if (!obj || arc_is_deallocating(obj)) return;
    header = arc_get_header(obj);
    if (!header) return;
    if (header->color == ARC_COLOR_GRAY || header->color == ARC_COLOR_GREEN) {
        return;
    }

    header->color = ARC_COLOR_GRAY;
    if (header->scanner) {
        header->scanner(obj, cycle_scan_decrement);
    }
}

// Re-increment trial counts for objects that aren't garbage
static void cycle_scan_restore(void* field_ptr) {
    if (!field_ptr || arc_is_deallocating(field_ptr)) return;
    ArcHeader* header = arc_get_header(field_ptr);
    if (!header) return;
    if (header->color == ARC_COLOR_BLACK || header->color == ARC_COLOR_GREEN) {
        return;
    }

    header->strong_count++;
    header->color = ARC_COLOR_BLACK;
    if (header->scanner) {
        header->scanner(field_ptr, cycle_scan_restore);
    }
}

static void cycle_scan_live(void* obj) {
    ArcHeader* header;

    if (!obj || arc_is_deallocating(obj)) return;
    header = arc_get_header(obj);
    if (!header || header->color != ARC_COLOR_GRAY) return;

    if (header->strong_count > 0) {
        cycle_scan_restore(obj);
        return;
    }

    header->color = ARC_COLOR_WHITE;
    if (header->scanner) {
        header->scanner(obj, cycle_scan_live);
    }
}

// Track collected count during cycle collection
static int g_cycle_collected_count = 0;

// Collect white (garbage) objects found during scan
static void cycle_collect_white(void* obj) {
    if (!obj) return;
    ArcHeader* header = arc_get_header(obj);
    if (!header || arc_is_deallocating(obj)) return;
    if (header->color != ARC_COLOR_WHITE) return;

    // Mark as black to prevent re-collection
    header->color = ARC_COLOR_BLACK;

    // Recurse into children first (collect deepest nodes first)
    if (header->scanner) {
        header->scanner(obj, cycle_collect_white);
    }

    if (arc_cycle_collect(obj)) {
        g_cycle_collected_count++;
    }
}

// --- Lifecycle ---

CycleCollector* cycle_gc_create(void) {
    CycleGCConfig config = {
        .collection_threshold = CYCLE_GC_DEFAULT_THRESHOLD,
        .max_suspects = CYCLE_GC_DEFAULT_MAX_SUSPECTS,
        .enabled = true
    };
    return cycle_gc_create_with_config(config);
}

CycleCollector* cycle_gc_create_with_config(CycleGCConfig config) {
    CycleCollector* cc = (CycleCollector*)malloc(sizeof(CycleCollector));
    if (!cc) return NULL;

    cc->suspect_capacity = config.max_suspects > 0 ? config.max_suspects : CYCLE_GC_DEFAULT_MAX_SUSPECTS;
    cc->suspects = (void**)malloc(sizeof(void*) * (size_t)cc->suspect_capacity);
    if (!cc->suspects) {
        free(cc);
        return NULL;
    }

    cc->suspect_count = 0;
    cc->config = config;
    memset(&cc->stats, 0, sizeof(CycleGCStats));
    cc->collecting = false;

    return cc;
}

void cycle_gc_destroy(CycleCollector* cc) {
    if (!cc) return;
    free(cc->suspects);
    free(cc);
}

// --- Suspect management ---

void cycle_gc_add_suspect(CycleCollector* cc, void* obj) {
    if (!cc || !obj || !cc->config.enabled) return;

    ArcHeader* header = arc_get_header(obj);
    if (!header) return;

    // Skip acyclic objects
    if (header->flags & ARC_FLAG_ACYCLIC) return;
    if (arc_is_deallocating(obj)) return;
    if (header->strong_count <= 0) return;

    // Skip objects without scanner (they can't form cycles)
    if (!header->scanner) return;

    // Skip already buffered objects
    if (header->flags & ARC_FLAG_BUFFERED) return;

    // Mark as purple (potential cycle root) and buffered
    header->color = ARC_COLOR_PURPLE;
    header->flags |= ARC_FLAG_BUFFERED;

    // Add to buffer
    if (cc->suspect_count >= cc->suspect_capacity) {
        // Buffer full - force collection
        cycle_gc_collect(cc);

        // If still full after collection, grow buffer
        if (cc->suspect_count >= cc->suspect_capacity) {
            int new_cap = cc->suspect_capacity * 2;
            void** new_buf = (void**)realloc(cc->suspects, sizeof(void*) * (size_t)new_cap);
            if (!new_buf) return;  // Can't grow, drop suspect
            cc->suspects = new_buf;
            cc->suspect_capacity = new_cap;
        }
    }

    cc->suspects[cc->suspect_count++] = obj;
    cc->stats.suspects_buffered++;
}

// --- Collection ---

int cycle_gc_collect_if_needed(CycleCollector* cc) {
    if (!cc || !cc->config.enabled) return 0;
    if (cc->suspect_count < cc->config.collection_threshold) return 0;
    return cycle_gc_collect(cc);
}

int cycle_gc_collect(CycleCollector* cc) {
    if (!cc || cc->collecting) return 0;
    cc->collecting = true;
    cc->stats.collections_run++;

    int total_collected = 0;

    // Phase 1: Mark roots
    // For each purple suspect, do trial deletion
    for (int i = 0; i < cc->suspect_count; i++) {
        void* obj = cc->suspects[i];
        if (!obj) continue;

        ArcHeader* header = arc_get_header(obj);
        if (!header) continue;

        if (!arc_is_deallocating(obj) &&
            header->color == ARC_COLOR_PURPLE && header->strong_count > 0) {
            // Trial deletion: decrement reference counts of children
            header->color = ARC_COLOR_GRAY;
            if (header->scanner) {
                header->scanner(obj, cycle_scan_decrement);
            }
        } else {
            // Not a valid root anymore - clear buffered flag
            header->flags &= ~ARC_FLAG_BUFFERED;
            // If it was already freed or color changed, skip
            if (header->color == ARC_COLOR_BLACK && header->strong_count == 0) {
                // Already dead
            }
            cc->suspects[i] = NULL;
        }
    }

    // Phase 2: Scan - check which objects survived trial deletion
    for (int i = 0; i < cc->suspect_count; i++) {
        void* obj = cc->suspects[i];
        if (!obj) continue;

        ArcHeader* header = arc_get_header(obj);
        if (!header) continue;

        cycle_scan_live(obj);
        if (header->color == ARC_COLOR_BLACK) {
            header->flags &= ~ARC_FLAG_BUFFERED;
            cc->suspects[i] = NULL;
        }
    }

    // Phase 3: Collect white (garbage) objects
    for (int i = 0; i < cc->suspect_count; i++) {
        void* obj = cc->suspects[i];
        if (!obj) continue;

        ArcHeader* header = arc_get_header(obj);
        if (!header) continue;

        if (header->color == ARC_COLOR_WHITE || header->strong_count == 0) {
            cc->stats.cycles_found++;
            g_cycle_collected_count = 0;
            cc->stats.bytes_collected += header->size;
            cycle_collect_white(obj);
            total_collected += g_cycle_collected_count;
            cc->stats.objects_collected += (size_t)g_cycle_collected_count;
        }

        cc->suspects[i] = NULL;
    }

    // Compact suspect buffer
    cc->suspect_count = 0;

    cc->collecting = false;
    return total_collected;
}

// --- Configuration ---

void cycle_gc_set_threshold(CycleCollector* cc, int threshold) {
    if (!cc) return;
    cc->config.collection_threshold = threshold;
}

void cycle_gc_set_enabled(CycleCollector* cc, bool enabled) {
    if (!cc) return;
    cc->config.enabled = enabled;
}

// --- Statistics ---

CycleGCStats cycle_gc_get_stats(CycleCollector* cc) {
    CycleGCStats empty = {0};
    if (!cc) return empty;
    return cc->stats;
}

void cycle_gc_print_stats(CycleCollector* cc) {
    if (!cc) return;
    printf("=== Cycle Collector Statistics ===\n");
    printf("Collections run:     %llu\n", (unsigned long long)cc->stats.collections_run);
    printf("Suspects buffered:   %llu\n", (unsigned long long)cc->stats.suspects_buffered);
    printf("Cycles found:        %llu\n", (unsigned long long)cc->stats.cycles_found);
    printf("Objects collected:   %llu\n", (unsigned long long)cc->stats.objects_collected);
    printf("Current suspects:    %d\n", cc->suspect_count);
    printf("Buffer capacity:     %d\n", cc->suspect_capacity);
    printf("==================================\n");
}
