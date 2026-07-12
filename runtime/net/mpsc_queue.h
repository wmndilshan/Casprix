#ifndef CASPRIX_NET_MPSC_QUEUE_H
#define CASPRIX_NET_MPSC_QUEUE_H

#include "cx_arena.h"

#include <stdatomic.h>

typedef struct CxMpscNode {
    _Atomic(struct CxMpscNode*) next;
    void* value;
} CxMpscNode;

typedef struct CxMpscQueue {
    _Atomic(CxMpscNode*) head;
    CxMpscNode*          tail;
    CxArena*             node_arena;
} CxMpscQueue;

CxMpscQueue* cx_mpsc_queue_create(CxArena* arena);
int          cx_mpsc_enqueue(CxMpscQueue* q, void* value);
void*        cx_mpsc_dequeue(CxMpscQueue* q);

#endif
