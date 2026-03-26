#include "../../include/casprix/lang_abi.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

typedef struct {
    CpxRtConfig cfg;
    atomic_uint_fast64_t next_task_id;
} CpxRtImpl;

struct CpxRtHandle {
    CpxRtImpl* impl;
};

struct CpxTaskHandle {
    uint64_t id;
    CpxTaskState state;
    int status;
};

struct CpxOpHandle {
    int reserved;
};

struct CpxTimerHandle {
    int reserved;
};

struct CpxPoolHandle {
    int reserved;
};

static int abi_rt_init(const CpxRtConfig* cfg, CpxRtHandle** out_rt) {
    if (!out_rt) return CPX_STATUS_INVALID_ARG;

    CpxRtHandle* rt = (CpxRtHandle*)calloc(1, sizeof(CpxRtHandle));
    if (!rt) return CPX_STATUS_NO_MEMORY;

    rt->impl = (CpxRtImpl*)calloc(1, sizeof(CpxRtImpl));
    if (!rt->impl) {
        free(rt);
        return CPX_STATUS_NO_MEMORY;
    }

    if (cfg) rt->impl->cfg = *cfg;
    atomic_store(&rt->impl->next_task_id, 1);
    *out_rt = rt;
    return CPX_STATUS_OK;
}

static void abi_rt_shutdown(CpxRtHandle* rt) {
    if (!rt) return;
    free(rt->impl);
    free(rt);
}

static int abi_task_spawn(CpxRtHandle* rt, const CpxTaskDesc* desc, CpxTaskHandle** out_task) {
    if (!rt || !rt->impl || !desc || !out_task || !desc->entry) return CPX_STATUS_INVALID_ARG;

    CpxTaskHandle* task = (CpxTaskHandle*)calloc(1, sizeof(CpxTaskHandle));
    if (!task) return CPX_STATUS_NO_MEMORY;

    task->id = atomic_fetch_add(&rt->impl->next_task_id, 1);
    task->state = CPX_TASK_RUNNING;
    task->status = CPX_STATUS_OK;

    desc->entry(desc->ctx);

    if (desc->cancel) {
        task->state = CPX_TASK_CANCELLED;
        task->status = CPX_STATUS_CANCELLED;
    } else {
        task->state = CPX_TASK_DONE;
        task->status = CPX_STATUS_OK;
    }

    *out_task = task;
    return CPX_STATUS_OK;
}

static int abi_task_cancel(CpxTaskHandle* task) {
    if (!task) return CPX_STATUS_INVALID_ARG;
    if (task->state == CPX_TASK_DONE) return CPX_STATUS_BAD_STATE;
    task->state = CPX_TASK_CANCELLED;
    task->status = CPX_STATUS_CANCELLED;
    return CPX_STATUS_OK;
}

static int abi_task_join_poll(CpxTaskHandle* task, CpxTaskStatus* out) {
    if (!task || !out) return CPX_STATUS_INVALID_ARG;
    out->state = task->state;
    out->status = task->status;
    free(task);
    return CPX_STATUS_OK;
}

static int abi_io_submit(CpxRtHandle* rt, const CpxIoReq* req, CpxOpHandle** out_op) {
    (void)rt;
    (void)req;
    (void)out_op;
    return CPX_STATUS_UNSUPPORTED;
}

static int abi_timer_arm(CpxRtHandle* rt, const CpxTimerReq* req, CpxTimerHandle** out_timer) {
    (void)rt;
    (void)req;
    (void)out_timer;
    return CPX_STATUS_UNSUPPORTED;
}

static int abi_pool_alloc(CpxPoolHandle* pool, size_t size, size_t align, void** out_ptr) {
    (void)pool;
    (void)align;
    if (!out_ptr || size == 0) return CPX_STATUS_INVALID_ARG;

    void* p = malloc(size);
    if (!p) return CPX_STATUS_NO_MEMORY;

    memset(p, 0, size);
    *out_ptr = p;
    return CPX_STATUS_OK;
}

static int abi_pool_free(CpxPoolHandle* pool, void* ptr) {
    (void)pool;
    if (!ptr) return CPX_STATUS_INVALID_ARG;
    free(ptr);
    return CPX_STATUS_OK;
}

static const char* abi_status_str(int code) {
    switch (code) {
        case CPX_STATUS_OK: return "ok";
        case CPX_STATUS_INVALID_ARG: return "invalid_arg";
        case CPX_STATUS_BAD_STATE: return "bad_state";
        case CPX_STATUS_NO_MEMORY: return "no_memory";
        case CPX_STATUS_QUEUE_FULL: return "queue_full";
        case CPX_STATUS_TIMEOUT: return "timeout";
        case CPX_STATUS_CANCELLED: return "cancelled";
        case CPX_STATUS_UNSUPPORTED: return "unsupported";
        case CPX_STATUS_INTERNAL: return "internal";
        default: return "unknown";
    }
}

static const CpxRuntimeApi k_api = {
    CPX_LANG_ABI_MAJOR,
    CPX_LANG_ABI_MINOR,
    CPX_LANG_FEATURE_TASKS | CPX_LANG_FEATURE_POOLS,

    abi_rt_init,
    abi_rt_shutdown,

    abi_task_spawn,
    abi_task_cancel,
    abi_task_join_poll,

    abi_io_submit,
    abi_timer_arm,

    abi_pool_alloc,
    abi_pool_free,

    abi_status_str
};

const CpxRuntimeApi* cpx_runtime_get_api(void) {
    return &k_api;
}
