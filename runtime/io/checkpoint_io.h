#ifndef CASPRIX_CHECKPOINT_IO_H
#define CASPRIX_CHECKPOINT_IO_H

#include <stddef.h>
#include <stdint.h>

#include "direct_io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      fd;
    uint32_t flags;
} CpxCheckpointWriter;

enum {
    CPX_CKPT_F_FSYNC_ON_CLOSE = 1u << 0,
    CPX_CKPT_F_CACHE_DROP_HINT = 1u << 1,
};

int cpx_ckpt_open(CpxCheckpointWriter* w, const char* path, uint32_t flags);
int cpx_ckpt_write(CpxCheckpointWriter* w, const void* data, size_t len);
int cpx_ckpt_writev(CpxCheckpointWriter* w, const CpxIoVec* iov, int iovcnt);
int cpx_ckpt_close(CpxCheckpointWriter* w);

#ifdef __cplusplus
}
#endif

#endif /* CASPRIX_CHECKPOINT_IO_H */
