#ifndef CASPRIX_LOCKFREE_LOG_H
#define CASPRIX_LOCKFREE_LOG_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CPX_LOG_DEBUG = 0,
    CPX_LOG_INFO  = 1,
    CPX_LOG_WARN  = 2,
    CPX_LOG_ERROR = 3,
} CpxLogLevel;

#ifndef CPX_LOG_MSG_MAX
#define CPX_LOG_MSG_MAX 240
#endif

typedef struct {
    _Atomic uint64_t seq;
    uint16_t         len;
    uint8_t          level;
    uint8_t          reserved;
    char             msg[CPX_LOG_MSG_MAX];
} CpxLogSlot;

typedef struct {
    CpxLogSlot*      slots;
    uint64_t         mask;
    _Atomic uint64_t enqueue_pos;
    _Atomic uint64_t dequeue_pos;
    _Atomic uint64_t dropped;
    _Atomic bool     running;
    int              out_fd;
    uintptr_t        thread_handle;
} CpxLogQueue;

int  cpx_logq_init(CpxLogQueue* q, uint32_t capacity_pow2, int out_fd);
void cpx_logq_shutdown(CpxLogQueue* q);
bool cpx_logq_try_push(CpxLogQueue* q, CpxLogLevel level, const char* msg, size_t len);
uint64_t cpx_logq_dropped(const CpxLogQueue* q);

#endif /* CASPRIX_LOCKFREE_LOG_H */
