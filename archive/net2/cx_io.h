#ifndef CASPRIX_NET2_IO_H
#define CASPRIX_NET2_IO_H

#include "cx_task.h"
#include "cx_runtime.h"
#include "../net/ringbuf.h"
#include <stdint.h>

typedef struct CxIoRequest {
    CxTask*            task;       // Suspended task waiting on this fd
    CasprixRuntime*    rt;
    int                fd;
    uint32_t           events;     // EPOLLIN | EPOLLOUT
    CxRingBuffer*      read_buf;   // optional pre-allocated ring buffer
    Future*            io_future;  // Future for this specific IO operation
} CxIoRequest;

CxIoRequest* cx_io_request_create(CxArena* arena, CxTask* task, CasprixRuntime* rt, int fd, uint32_t events);
void         cx_io_register(CasprixRuntime* rt, CxIoRequest* req);
Future*      cx_io_read_async(CasprixRuntime* rt, CxTask* task, int fd, void* buf, size_t len);

#endif
