#include "scheduler_bridge.h"

#include <string.h>

CxSchedulerBridge* cx_scheduler_bridge_create(CxArena* arena) {
    if (!arena) return NULL;
    CxSchedulerBridge* b = (CxSchedulerBridge*)cx_arena_alloc_aligned(
        arena, sizeof(CxSchedulerBridge), 64);
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));
    b->ready_q = cx_mpsc_queue_create(arena);
    if (!b->ready_q) return NULL;
    return b;
}

int cx_scheduler_bridge_push_ready(CxSchedulerBridge* b, void* coroutine_or_task) {
    if (!b) return -1;
    return cx_mpsc_enqueue(b->ready_q, coroutine_or_task);
}

void* cx_scheduler_bridge_pop_ready(CxSchedulerBridge* b) {
    if (!b) return NULL;
    return cx_mpsc_dequeue(b->ready_q);
}
