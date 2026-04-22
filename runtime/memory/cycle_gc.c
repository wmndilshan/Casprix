// Casperix Runtime - Cycle Collector Implementation
// Bacon-Rajan synchronous cycle collection

#include "cycle_gc.h"
#include "arc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


/* ── Atomics for trial deletion ──────────────────────────────────────────────
 * The cycle collector performs a "trial deletion" phase that temporarily
 * decrements ref-counts to detect internal-only references.  This MUST use
 * the same atomic primitives as the mutator's arc_retain/arc_release so that
 * concurrent threads don't observe a torn ref-count.
 *
 * We re-use the ATOMIC_* macros from arc.h.
 * ─────────────────────────────────────────────────────────────────────────── */

static void cycle_scan_decrement(void* field_ptr) {
    if (!field_ptr) return;
    ArcHeader* header = arc_get_header(field_ptr);
    if (!header) return;

    int32_t after = ATOMIC_DEC(&header->strong_count);
    if (after == 0) {
        header->color = ARC_COLOR_WHITE;
        if (header->scanner)
            header->scanner(field_ptr, cycle_scan_decrement);
    } else {
        header->color = ARC_COLOR_GRAY;
    }
}

static void cycle_scan_restore(void* field_ptr) {
    if (!field_ptr) return;
    ArcHeader* header = arc_get_header(field_ptr);
    if (!header) return;

    ATOMIC_INC(&header->strong_count);
    if (header->color != ARC_COLOR_GREEN) {
        header->color = ARC_COLOR_BLACK;
        if (header->scanner)
            header->scanner(field_ptr, cycle_scan_restore);
    }
}

/* Accumulator for objects freed by a single cycle_collect_white traversal.
   Valid only inside cycle_gc_collect, which is non-reentrant (cc->collecting). */
static int s_white_count;

static void cycle_collect_white(void* obj) {
    if (!obj) return;
    ArcHeader* header = arc_get_header(obj);
    if (!header || header->color != ARC_COLOR_WHITE) return;

    header->color = ARC_COLOR_BLACK;
    header->flags &= ~ARC_FLAG_BUFFERED;

    if (header->scanner)
        header->scanner(obj, cycle_collect_white);

    if ((header->flags & ARC_FLAG_HAS_DESTRUCTOR) && header->destructor)
        header->destructor(obj);

    /* Update ARC-level stats so leak reporter stays accurate —
       cycle_collect_white bypasses arc_release, so we must notify manually. */
    arc_notify_freed(header->size);

    free(header);
    s_white_count++;
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

        if (header->color == ARC_COLOR_PURPLE && header->strong_count > 0) {
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

        if (header->strong_count > 0) {
            // Object is reachable from outside the cycle - restore
            header->color = ARC_COLOR_BLACK;
            if (header->scanner) {
                header->scanner(obj, cycle_scan_restore);
            }
            header->flags &= ~ARC_FLAG_BUFFERED;
            cc->suspects[i] = NULL;
        }
        // Objects with strong_count == 0 after trial deletion are in cycles
    }

    // Phase 3: Collect white (garbage) objects
    for (int i = 0; i < cc->suspect_count; i++) {
        void* obj = cc->suspects[i];
        if (!obj) continue;

        ArcHeader* header = arc_get_header(obj);
        if (!header) continue;

        if (header->color == ARC_COLOR_WHITE || header->strong_count == 0) {
            cc->stats.cycles_found++;
            s_white_count = 0;
            cycle_collect_white(obj);
            total_collected += s_white_count;
            cc->stats.objects_collected += (size_t)s_white_count;
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
