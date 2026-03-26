/**
 * Casperix Runtime - Rc<T> and Arc<T> Smart Pointer Wrappers
 *
 * Language-level reference-counted smart pointer types that wrap the
 * low-level ARC subsystem with ergonomic APIs.
 *
 *   Rc<T>  — single-threaded reference counting (no atomics)
 *   Arc<T> — atomic reference counting (thread-safe)
 *   Weak<T> — weak reference (does not prevent deallocation)
 *
 * Optimizations:
 *   - Deferred decrements via batch release queue
 *   - Elided retain/release for temporary values
 *   - Non-atomic Rc for single-threaded contexts
 */

#ifndef CASPERIX_REFCOUNT_H
#define CASPERIX_REFCOUNT_H

#include "arc.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  Rc<T> — Non-atomic reference counting (single-thread only)
 *  Lower overhead than Arc when thread safety is not required.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct RcHeader {
    int32_t  strong_count;         /* Non-atomic strong count           */
    int32_t  weak_count;           /* Non-atomic weak count             */
    uint32_t size;                 /* User data size                    */
    uint32_t flags;                /* RcFlag bits                       */
    void   (*destructor)(void*);   /* Optional destructor               */
} RcHeader;

#define RC_HEADER_SIZE  sizeof(RcHeader)
#define RC_OBJ_TO_HEADER(ptr)  ((RcHeader*)((char*)(ptr) - RC_HEADER_SIZE))
#define RC_HEADER_TO_OBJ(hdr)  ((void*)((char*)(hdr) + RC_HEADER_SIZE))

/* Flags */
#define RC_FLAG_NONE          0x00
#define RC_FLAG_MOVED         0x01
#define RC_FLAG_HAS_DESTRUCTOR 0x02

/* Allocate a new Rc-managed object */
void* rc_alloc(size_t size);
void* rc_alloc_with_destructor(size_t size, void (*destructor)(void*));

/* Retain (increment strong count) — NOT thread-safe */
void* rc_retain(void* obj);

/* Release (decrement strong count) — NOT thread-safe */
void  rc_release(void* obj);

/* Get strong count */
int32_t rc_strong_count(const void* obj);

/* Clone: deep-copy the managed data into a new Rc */
void* rc_clone(const void* obj);

/* ═══════════════════════════════════════════════════════════════════
 *  Weak<T> for Rc — non-atomic weak reference
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct RcWeak {
    RcHeader* header;
} RcWeak;

RcWeak rc_weak_create(void* obj);
void*  rc_weak_upgrade(RcWeak weak);
void   rc_weak_release(RcWeak* weak);
bool   rc_weak_is_alive(RcWeak weak);

/* ═══════════════════════════════════════════════════════════════════
 *  Arc<T> — re-exports from arc.h with wrapper API
 *  Thread-safe atomic reference counting.
 * ═══════════════════════════════════════════════════════════════════ */

/* Arc uses the existing ARC subsystem directly.
   These are thin convenience wrappers / aliases. */

static inline void* arc_new(size_t size) {
    return arc_alloc(size);
}

static inline void* arc_new_with_destructor(size_t size,
                                             arc_destructor_fn dtor) {
    return arc_alloc_with_destructor(size, dtor);
}

/* Already provided by arc.h:
   arc_retain(), arc_release(), arc_strong_count(),
   arc_weak_create(), arc_weak_upgrade(), arc_weak_release() */

/* ═══════════════════════════════════════════════════════════════════
 *  Batch Release Queue — deferred reference count decrements
 *
 *  Instead of immediately decrementing (which may cascade destructors),
 *  objects can be enqueued and released in a batch during a safe point.
 * ═══════════════════════════════════════════════════════════════════ */

#define RELEASE_QUEUE_CAPACITY 256

typedef struct ReleaseQueue {
    void*  objects[RELEASE_QUEUE_CAPACITY];
    int    count;
    bool   is_arc;   /* true → use arc_release, false → use rc_release */
} ReleaseQueue;

void release_queue_init(ReleaseQueue* q, bool is_arc);
void release_queue_push(ReleaseQueue* q, void* obj);
void release_queue_flush(ReleaseQueue* q);

/* ═══════════════════════════════════════════════════════════════════
 *  Rc/Arc Statistics
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct RcStats {
    size_t total_allocations;
    size_t total_frees;
    size_t current_objects;
    size_t current_bytes;
    size_t total_retains;
    size_t total_releases;
    size_t batch_flushes;
} RcStats;

RcStats rc_get_stats(void);
void    rc_reset_stats(void);
void    rc_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* CASPERIX_REFCOUNT_H */
