#ifndef CASPRIX_NET_SCHEDULER_BRIDGE_H
#define CASPRIX_NET_SCHEDULER_BRIDGE_H

#include "mpsc_queue.h"

typedef struct CxSchedulerBridge {
    CxMpscQueue* ready_q;
} CxSchedulerBridge;

CxSchedulerBridge* cx_scheduler_bridge_create(CxArena* arena);
int                cx_scheduler_bridge_push_ready(CxSchedulerBridge* b, void* coroutine_or_task);
void*              cx_scheduler_bridge_pop_ready(CxSchedulerBridge* b);

#endif
