#include "mpsc_queue.h"

#include <sched.h>
#include <string.h>

CxMpscQueue* cx_mpsc_queue_create(CxArena* arena) {
    if (!arena) return NULL;
    CxMpscQueue* q = (CxMpscQueue*)cx_arena_alloc_aligned(arena, sizeof(CxMpscQueue), 64);
    if (!q) return NULL;
    memset(q, 0, sizeof(*q));
    q->node_arena = arena;

    CxMpscNode* stub = (CxMpscNode*)cx_arena_alloc_aligned(arena, sizeof(CxMpscNode), 64);
    if (!stub) return NULL;
    atomic_store_explicit(&stub->next, NULL, memory_order_relaxed);
    stub->value = NULL;

    atomic_store_explicit(&q->head, stub, memory_order_release);
    q->tail = stub;
    return q;
}

int cx_mpsc_enqueue(CxMpscQueue* q, void* value) {
    if (!q) return -1;
    CxMpscNode* node = (CxMpscNode*)cx_arena_alloc_aligned(
        q->node_arena, sizeof(CxMpscNode), 64);
    if (!node) return -1;
    node->value = value;
    atomic_store_explicit(&node->next, NULL, memory_order_relaxed);

    CxMpscNode* prev = atomic_exchange_explicit(&q->head, node, memory_order_acq_rel);
    atomic_store_explicit(&prev->next, node, memory_order_release);
    return 0;
}

void* cx_mpsc_dequeue(CxMpscQueue* q) {
    if (!q) return NULL;
    CxMpscNode* tail = q->tail;
    CxMpscNode* next = atomic_load_explicit(&tail->next, memory_order_acquire);
    int backoff = 0;
    while (!next && backoff < 8) {
        sched_yield();
        next = atomic_load_explicit(&tail->next, memory_order_acquire);
        backoff++;
    }
    if (!next) return NULL;

    q->tail = next;
    return next->value;
}
