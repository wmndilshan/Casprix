#ifndef CASPRIX_NET_IOURING_H
#define CASPRIX_NET_IOURING_H

#include "cx_arena.h"

#include <stdint.h>

typedef struct CxIoUring {
    int      fd;
    uint32_t sq_entries;
    uint32_t cq_entries;
    int      available;
} CxIoUring;

int        cx_iouring_kernel_supported(void);
CxIoUring* cx_iouring_create(CxArena* arena, uint32_t entries);
void       cx_iouring_destroy(CxIoUring* uring);

#endif
