/*
 * Casprix ML Runtime — Top-Level Runtime Lifecycle
 */

#include "cpx_runtime.h"

#include <stdlib.h>
#include <stdio.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#endif

static int detect_physical_cores(void) {
#if defined(_WIN32)
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* buffer = NULL;
    DWORD length = 0;

    GetLogicalProcessorInformationEx(RelationProcessorCore, buffer, &length);
    buffer = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)malloc(length);
    if (!buffer) goto fallback;
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, buffer, &length)) {
        free(buffer);
        goto fallback;
    }

    int cores = 0;
    DWORD offset = 0;
    while (offset < length) {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* entry =
            (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)((unsigned char*)buffer + offset);
        if (entry->Relationship == RelationProcessorCore) cores++;
        offset += entry->Size;
    }

    free(buffer);
    if (cores > 0) return cores;

fallback:
    {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return (int)info.dwNumberOfProcessors;
    }
#else
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (int)count : 1;
#endif
}

struct CpxRuntime {
    CpxRuntimeConfig cfg;
    CpxCpuInfo cpu;
    CpxScheduler* sched;
    CpxMemArena* mem;
    CpxTensorEngine engine;
};

CpxRuntime* cpx_runtime_create(const CpxRuntimeConfig* cfg) {
    CpxRuntime* rt = (CpxRuntime*)calloc(1, sizeof(CpxRuntime));
    if (!rt) return NULL;

    if (cfg) {
        rt->cfg = *cfg;
    } else {
        CpxRuntimeConfig defaults = CPX_RUNTIME_CONFIG_DEFAULT;
        rt->cfg = defaults;
    }

    if (rt->cfg.num_threads <= 0) {
        rt->cfg.num_threads = detect_physical_cores();
    }

    cpx_cpu_info(&rt->cpu);
    if (!rt->cfg.avx2) rt->cpu.avx2 = false;
    if (!rt->cfg.avx512) rt->cpu.avx512f = false;

    rt->mem = cpx_mem_arena_create(
        rt->cfg.param_pool_mb << 20,
        rt->cfg.grad_pool_mb << 20,
        rt->cfg.param_pool_mb << 20,
        rt->cfg.act_pool_mb << 20,
        rt->cfg.temp_pool_mb << 20,
        0,
        rt->cfg.num_threads);
    if (!rt->mem) {
        fprintf(stderr, "[cpx_runtime] failed to create memory arenas\n");
        free(rt);
        return NULL;
    }

    rt->sched = cpx_scheduler_create(rt->cfg.num_threads, rt->cfg.pin_threads);
    if (!rt->sched) {
        fprintf(stderr, "[cpx_runtime] failed to create scheduler\n");
        cpx_mem_arena_destroy(rt->mem);
        free(rt);
        return NULL;
    }

    cpx_tensor_engine_init(&rt->engine, rt->sched, rt->mem, &rt->cpu,
                           rt->cfg.enable_profiling);
    return rt;
}

void cpx_runtime_destroy(CpxRuntime* rt) {
    if (!rt) return;
    cpx_tensor_engine_destroy(&rt->engine);
    cpx_scheduler_destroy(rt->sched);
    cpx_mem_arena_destroy(rt->mem);
    free(rt);
}

const CpxCpuInfo* cpx_runtime_cpu_info(CpxRuntime* rt) {
    return rt ? &rt->cpu : NULL;
}

CpxScheduler* cpx_runtime_scheduler(CpxRuntime* rt) {
    return rt ? rt->sched : NULL;
}

CpxMemArena* cpx_runtime_param_arena(CpxRuntime* rt) {
    return rt ? rt->mem : NULL;
}

CpxTensorEngine* cpx_runtime_engine(CpxRuntime* rt) {
    return rt ? &rt->engine : NULL;
}
