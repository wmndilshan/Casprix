/*
 * Casprix Language Binding ABI (stable C interface)
 */

#ifndef CASPRIX_LANG_ABI_H
#define CASPRIX_LANG_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CPX_LANG_ABI_MAJOR 1u
#define CPX_LANG_ABI_MINOR 0u

#define CPX_LANG_FEATURE_TASKS   (1ull << 0)
#define CPX_LANG_FEATURE_IO      (1ull << 1)
#define CPX_LANG_FEATURE_TIMERS  (1ull << 2)
#define CPX_LANG_FEATURE_POOLS   (1ull << 3)

typedef enum {
    CPX_STATUS_OK = 0,
    CPX_STATUS_INVALID_ARG = 1,
    CPX_STATUS_BAD_STATE = 2,
    CPX_STATUS_NO_MEMORY = 3,
    CPX_STATUS_QUEUE_FULL = 4,
    CPX_STATUS_TIMEOUT = 5,
    CPX_STATUS_CANCELLED = 6,
    CPX_STATUS_UNSUPPORTED = 7,
    CPX_STATUS_INTERNAL = 255
} CpxStatus;

typedef struct CpxRtHandle CpxRtHandle;
typedef struct CpxTaskHandle CpxTaskHandle;
typedef struct CpxOpHandle CpxOpHandle;
typedef struct CpxTimerHandle CpxTimerHandle;
typedef struct CpxPoolHandle CpxPoolHandle;
typedef struct CpxCancelToken CpxCancelToken;

typedef struct {
    uint32_t worker_threads;
    uint32_t io_threads;
    uint32_t reserved0;
    uint32_t reserved1;
} CpxRtConfig;

typedef enum {
    CPX_TASK_PRIO_LOW = 0,
    CPX_TASK_PRIO_NORMAL = 1,
    CPX_TASK_PRIO_HIGH = 2,
    CPX_TASK_PRIO_CRITICAL = 3
} CpxTaskPriority;

typedef void (*CpxTaskEntryFn)(void* ctx);

typedef struct {
    CpxTaskEntryFn entry;
    void* ctx;
    CpxTaskPriority priority;
    uint64_t deadline_ns;
    CpxCancelToken* cancel;
} CpxTaskDesc;

typedef enum {
    CPX_TASK_PENDING = 0,
    CPX_TASK_RUNNING = 1,
    CPX_TASK_DONE = 2,
    CPX_TASK_CANCELLED = 3,
    CPX_TASK_FAILED = 4
} CpxTaskState;

typedef struct {
    CpxTaskState state;
    int status;
} CpxTaskStatus;

typedef enum {
    CPX_IO_READ = 0,
    CPX_IO_WRITE = 1,
    CPX_IO_ACCEPT = 2,
    CPX_IO_CONNECT = 3
} CpxIoOp;

typedef struct {
    CpxIoOp op;
    int fd;
    void* buf;
    uint32_t len;
    uint64_t deadline_ns;
    CpxCancelToken* cancel;
} CpxIoReq;

typedef struct {
    uint64_t delay_ns;
    CpxCancelToken* cancel;
} CpxTimerReq;

typedef struct {
    const uint8_t* ptr;
    uint32_t len;
} CpxByteView;

typedef struct {
    uint8_t* ptr;
    uint32_t len;
    uint32_t cap;
} CpxMutableBuffer;

typedef struct {
    uint32_t abi_major;
    uint32_t abi_minor;
    uint64_t feature_bits;

    int  (*rt_init)(const CpxRtConfig* cfg, CpxRtHandle** out_rt);
    void (*rt_shutdown)(CpxRtHandle* rt);

    int  (*task_spawn)(CpxRtHandle* rt, const CpxTaskDesc* desc, CpxTaskHandle** out_task);
    int  (*task_cancel)(CpxTaskHandle* task);
    int  (*task_join_poll)(CpxTaskHandle* task, CpxTaskStatus* out);

    int  (*io_submit)(CpxRtHandle* rt, const CpxIoReq* req, CpxOpHandle** out_op);
    int  (*timer_arm)(CpxRtHandle* rt, const CpxTimerReq* req, CpxTimerHandle** out_timer);

    int  (*pool_alloc)(CpxPoolHandle* pool, size_t size, size_t align, void** out_ptr);
    int  (*pool_free)(CpxPoolHandle* pool, void* ptr);

    const char* (*status_str)(int code);
} CpxRuntimeApi;

const CpxRuntimeApi* cpx_runtime_get_api(void);

#ifdef __cplusplus
}
#endif

#endif /* CASPRIX_LANG_ABI_H */
