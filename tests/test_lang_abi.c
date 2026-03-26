#include "../include/casprix/lang_abi.h"

#include <stdio.h>
#include <stdint.h>

typedef struct {
    int value;
} TestCtx;

static void test_task_entry(void* data) {
    TestCtx* ctx = (TestCtx*)data;
    ctx->value += 7;
}

int main(void) {
    const CpxRuntimeApi* api = cpx_runtime_get_api();
    if (!api) return 1;
    if (api->abi_major != CPX_LANG_ABI_MAJOR) return 2;

    CpxRtConfig cfg = {0};
    CpxRtHandle* rt = NULL;
    if (api->rt_init(&cfg, &rt) != CPX_STATUS_OK || !rt) return 3;

    void* mem = NULL;
    if (api->pool_alloc(NULL, 64, 8, &mem) != CPX_STATUS_OK || !mem) {
        api->rt_shutdown(rt);
        return 4;
    }
    if (api->pool_free(NULL, mem) != CPX_STATUS_OK) {
        api->rt_shutdown(rt);
        return 5;
    }

    TestCtx ctx = { .value = 5 };
    CpxTaskDesc desc;
    desc.entry = test_task_entry;
    desc.ctx = &ctx;
    desc.priority = CPX_TASK_PRIO_NORMAL;
    desc.deadline_ns = 0;
    desc.cancel = NULL;

    CpxTaskHandle* task = NULL;
    if (api->task_spawn(rt, &desc, &task) != CPX_STATUS_OK || !task) {
        api->rt_shutdown(rt);
        return 6;
    }

    CpxTaskStatus status;
    if (api->task_join_poll(task, &status) != CPX_STATUS_OK) {
        api->rt_shutdown(rt);
        return 7;
    }

    if (status.state != CPX_TASK_DONE || ctx.value != 12) {
        api->rt_shutdown(rt);
        return 8;
    }

    api->rt_shutdown(rt);
    printf("test_lang_abi: OK\n");
    return 0;
}
