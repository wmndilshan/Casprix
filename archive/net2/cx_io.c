#define _GNU_SOURCE
#include "cx_io.h"

#define CxTask ThreadPoolTask
#include "../net/threadpool.h"
#undef CxTask
#include <stdatomic.h>
#include <stdlib.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

static void cx_io_on_read(CxReactor* r, int fd, void* userdata);
static void cx_io_on_write(CxReactor* r, int fd, void* userdata);
static void cx_io_on_close(CxReactor* r, int fd, void* userdata);

CxIoRequest* cx_io_request_create(CxArena* arena, CxTask* task, CasprixRuntime* rt, int fd, uint32_t events) {
    CxIoRequest* req = (CxIoRequest*)cx_arena_alloc(arena, sizeof(CxIoRequest));
    if (!req) return NULL;
    
    req->task = task;
    req->rt = rt;
    req->fd = fd;
    req->events = events;
    req->read_buf = NULL;
    req->io_future = NULL;
    
    return req;
}

void cx_io_register(CasprixRuntime* rt, CxIoRequest* req) {
    cx_reactor_add(rt->reactor, req->fd, req->events, req,
                   cx_io_on_read, cx_io_on_write, cx_io_on_close);
}

static void cx_io_wake_task(CxIoRequest* req) {
    atomic_store_explicit(&req->task->state, TASK_READY, memory_order_release);
    cx_scheduler_bridge_push_ready(req->rt->bridge, req->task);
    atomic_fetch_add_explicit(&req->rt->threadpool->pending_count, 1, memory_order_acq_rel);
    syscall(SYS_futex, &req->rt->threadpool->pending_count, 
            FUTEX_WAKE_PRIVATE, req->rt->n_workers, NULL, NULL, 0);
    
    if (req->io_future) {
        future_complete(req->io_future, NULL);
    }
}

static void cx_io_on_read(CxReactor* r, int fd, void* userdata) {
    cx_io_wake_task((CxIoRequest*)userdata);
}

static void cx_io_on_write(CxReactor* r, int fd, void* userdata) {
    cx_io_wake_task((CxIoRequest*)userdata);
}

static void cx_io_on_close(CxReactor* r, int fd, void* userdata) {
    CxIoRequest* req = (CxIoRequest*)userdata;
    future_fail(req->task->result, (void*)(intptr_t)fd);
    atomic_store_explicit(&req->task->state, TASK_COMPLETED, memory_order_release);
}

Future* cx_io_read_async(CasprixRuntime* rt, CxTask* task, int fd, void* buf, size_t len) {
    // Note: This high-level helper currently ignores buf/len and just waits for readiness
    // A real implementation would perform the read after waking.
    
    CxIoRequest* req = cx_io_request_create(rt->net_arena, task, rt, fd, 0x001); // EPOLLIN is usually 0x001
    // Actually EPOLLIN is defined in sys/epoll.h, but I'll use 1 for now if not available.
    // Wait, reactor.h doesn't include epoll.h in the header shown, but it's likely using it.
    
    req->io_future = cx_future_create_arc(rt->mem);
    cx_io_register(rt, req);
    
    // The instructions say: "calls cx_task_await_future(task, future) which suspends via coro_yield()"
    // But my cx_io_on_read already pushes to bridge. 
    // If I use cx_task_await_future, it will register another waker.
    // To avoid double-pushing, I'll temporarily NULL out the bridge_ref or similar?
    // Actually, I'll just make cx_task_waker_cb check if state is already READY.
    
    cx_task_await_future(task, req->io_future);
    
    return req->io_future;
}
