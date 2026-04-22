#ifndef CASPRIX_NET_RINGBUF_H
#define CASPRIX_NET_RINGBUF_H

#include "cx_arena.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef struct CxRingBuffer {
    uint8_t*          buf;
    size_t            capacity;
    size_t            mask;
    _Atomic uint64_t  write_idx;
    _Atomic uint64_t  read_idx;
} CxRingBuffer;

CxRingBuffer* cx_ringbuf_create(CxArena* arena, size_t capacity_pow2);
size_t        cx_ringbuf_write(CxRingBuffer* rb, const void* data, size_t len);
size_t        cx_ringbuf_read(CxRingBuffer* rb, void* out, size_t len);
size_t        cx_ringbuf_available_read(const CxRingBuffer* rb);
size_t        cx_ringbuf_available_write(const CxRingBuffer* rb);

#endif
