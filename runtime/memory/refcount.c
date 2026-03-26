/**
 * Casperix Runtime - Rc<T> / Batch Release Implementation
 *
 * Non-atomic single-threaded reference counting and deferred release queues.
 */

#include "refcount.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* ─── Rc statistics ─── */

static RcStats g_rc_stats = {0};

/* ═══════════════════════════════════════════════════════════════════
 *  Rc<T> — Non-atomic reference counting
 * ═══════════════════════════════════════════════════════════════════ */

void* rc_alloc(size_t size) {
    return rc_alloc_with_destructor(size, NULL);
}

void* rc_alloc_with_destructor(size_t size, void (*destructor)(void*)) {
    if (size == 0) return NULL;

    size_t total = RC_HEADER_SIZE + size;
    RcHeader* header = (RcHeader*)malloc(total);
    if (!header) return NULL;

    header->strong_count = 1;
    header->weak_count   = 1;   /* +1 held by strong refs collectively */
    header->size         = (uint32_t)size;
    header->flags        = RC_FLAG_NONE;
    header->destructor   = NULL;

    if (destructor) {
        header->destructor = destructor;
        header->flags |= RC_FLAG_HAS_DESTRUCTOR;
    }

    void* obj = RC_HEADER_TO_OBJ(header);
    memset(obj, 0, size);

    g_rc_stats.total_allocations++;
    g_rc_stats.current_objects++;
    g_rc_stats.current_bytes += size;

    return obj;
}

void* rc_retain(void* obj) {
    if (!obj) return NULL;

    RcHeader* header = RC_OBJ_TO_HEADER(obj);
    assert(header->strong_count > 0 && "rc_retain on dead object");
    assert(!(header->flags & RC_FLAG_MOVED) && "rc_retain on moved object");

    header->strong_count++;  /* Non-atomic — single-thread only */
    g_rc_stats.total_retains++;

    return obj;
}

static void rc_dealloc(RcHeader* header) {
    /* Call destructor */
    if ((header->flags & RC_FLAG_HAS_DESTRUCTOR) && header->destructor) {
        void* obj = RC_HEADER_TO_OBJ(header);
        header->destructor(obj);
    }

    g_rc_stats.total_frees++;
    g_rc_stats.current_objects--;
    g_rc_stats.current_bytes -= header->size;

    if (header->weak_count <= 1) {
        free(header);
    } else {
        header->weak_count--;
        /* Zero user data to prevent use-after-free via weak refs */
        void* obj = RC_HEADER_TO_OBJ(header);
        memset(obj, 0, header->size);
    }
}

void rc_release(void* obj) {
    if (!obj) return;

    RcHeader* header = RC_OBJ_TO_HEADER(obj);
    assert(header->strong_count > 0 && "rc_release on dead object");

    g_rc_stats.total_releases++;

    header->strong_count--;  /* Non-atomic */
    if (header->strong_count == 0) {
        rc_dealloc(header);
    }
}

int32_t rc_strong_count(const void* obj) {
    if (!obj) return 0;
    return RC_OBJ_TO_HEADER((void*)obj)->strong_count;
}

void* rc_clone(const void* obj) {
    if (!obj) return NULL;
    const RcHeader* header = RC_OBJ_TO_HEADER((void*)obj);

    void* new_obj = rc_alloc(header->size);
    if (!new_obj) return NULL;

    memcpy(new_obj, obj, header->size);

    /* If there's a destructor, copy it */
    RcHeader* new_header = RC_OBJ_TO_HEADER(new_obj);
    new_header->destructor = header->destructor;
    new_header->flags      = header->flags & ~RC_FLAG_MOVED;

    return new_obj;
}

/* ═══════════════════════════════════════════════════════════════════
 *  RcWeak — Non-atomic weak references
 * ═══════════════════════════════════════════════════════════════════ */

RcWeak rc_weak_create(void* obj) {
    RcWeak weak = { NULL };
    if (!obj) return weak;

    RcHeader* header = RC_OBJ_TO_HEADER(obj);
    assert(header->strong_count > 0);

    header->weak_count++;
    weak.header = header;
    return weak;
}

void* rc_weak_upgrade(RcWeak weak) {
    if (!weak.header) return NULL;
    if (weak.header->strong_count <= 0) return NULL;

    weak.header->strong_count++;
    g_rc_stats.total_retains++;
    return RC_HEADER_TO_OBJ(weak.header);
}

void rc_weak_release(RcWeak* weak) {
    if (!weak || !weak->header) return;

    RcHeader* header = weak->header;
    weak->header = NULL;

    header->weak_count--;
    if (header->weak_count == 0 && header->strong_count <= 0) {
        free(header);
    }
}

bool rc_weak_is_alive(RcWeak weak) {
    if (!weak.header) return false;
    return weak.header->strong_count > 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Batch Release Queue
 * ═══════════════════════════════════════════════════════════════════ */

void release_queue_init(ReleaseQueue* q, bool is_arc) {
    memset(q, 0, sizeof(ReleaseQueue));
    q->is_arc = is_arc;
}

void release_queue_push(ReleaseQueue* q, void* obj) {
    if (!q || !obj) return;

    if (q->count >= RELEASE_QUEUE_CAPACITY) {
        /* Queue full — flush first */
        release_queue_flush(q);
    }

    q->objects[q->count++] = obj;
}

void release_queue_flush(ReleaseQueue* q) {
    if (!q || q->count == 0) return;

    if (q->is_arc) {
        for (int i = 0; i < q->count; i++) {
            arc_release(q->objects[i]);
        }
    } else {
        for (int i = 0; i < q->count; i++) {
            rc_release(q->objects[i]);
        }
    }

    q->count = 0;
    g_rc_stats.batch_flushes++;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Statistics
 * ═══════════════════════════════════════════════════════════════════ */

RcStats rc_get_stats(void) {
    return g_rc_stats;
}

void rc_reset_stats(void) {
    memset(&g_rc_stats, 0, sizeof(RcStats));
}

void rc_print_stats(void) {
    printf("=== Rc<T> Statistics ===\n");
    printf("Allocated:     %llu\n", (unsigned long long)g_rc_stats.total_allocations);
    printf("Freed:         %llu\n", (unsigned long long)g_rc_stats.total_frees);
    printf("Current:       %llu objects\n", (unsigned long long)g_rc_stats.current_objects);
    printf("Bytes in use:  %llu\n", (unsigned long long)g_rc_stats.current_bytes);
    printf("Retains:       %llu\n", (unsigned long long)g_rc_stats.total_retains);
    printf("Releases:      %llu\n", (unsigned long long)g_rc_stats.total_releases);
    printf("Batch flushes: %llu\n", (unsigned long long)g_rc_stats.batch_flushes);
    printf("========================\n");
}
