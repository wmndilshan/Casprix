/**
 * Portable reactor: poll() (POSIX) / WSAPoll (Windows)
 */

#include "reactor.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef struct pollfd cpx_pollfd;
#define cpx_poll WSAPoll
#else
#include <poll.h>
typedef struct pollfd cpx_pollfd;
#define cpx_poll poll
#endif

typedef struct {
    cpx_pollfd* fds;
    void**      userdata;
    int         count;
    int         capacity;
} ReactorPollCtx;

static int poll_init(void* ctx, int max_events) {
    ReactorPollCtx* c = (ReactorPollCtx*)ctx;
    if (!c || max_events <= 0) return -1;
    c->capacity = max_events;
    c->fds      = (cpx_pollfd*)calloc((size_t)max_events, sizeof(cpx_pollfd));
    c->userdata = (void**)calloc((size_t)max_events, sizeof(void*));
    return (c->fds && c->userdata) ? 0 : -1;
}

static int find_fd(ReactorPollCtx* c, int fd) {
    for (int i = 0; i < c->count; i++) {
        if (c->fds[i].fd == fd) return i;
    }
    return -1;
}

static short events_to_poll(uint32_t ev) {
    short o = 0;
    if (ev & CPX_REACT_READ)  o |= (short)POLLIN;
    if (ev & CPX_REACT_WRITE) o |= (short)POLLOUT;
    return o;
}

static int poll_add(void* ctx, int fd, uint32_t events, void* userdata) {
    ReactorPollCtx* c = (ReactorPollCtx*)ctx;
    if (!c || fd < 0 || c->count >= c->capacity) return -1;
    if (find_fd(c, fd) >= 0) return -1;
    c->fds[c->count].fd      = fd;
    c->fds[c->count].events  = events_to_poll(events);
    c->fds[c->count].revents = 0;
    c->userdata[c->count]    = userdata;
    c->count++;
    return 0;
}

static int poll_mod(void* ctx, int fd, uint32_t events) {
    ReactorPollCtx* c = (ReactorPollCtx*)ctx;
    int i = find_fd(c, fd);
    if (i < 0) return -1;
    c->fds[i].events  = events_to_poll(events);
    c->fds[i].revents = 0;
    return 0;
}

static int poll_del(void* ctx, int fd) {
    ReactorPollCtx* c = (ReactorPollCtx*)ctx;
    int i = find_fd(c, fd);
    if (i < 0) return -1;
    if (i < c->count - 1) {
        c->fds[i]      = c->fds[c->count - 1];
        c->userdata[i] = c->userdata[c->count - 1];
    }
    c->count--;
    return 0;
}

static int poll_wait(void* ctx, CpxIOEvent* out, int max_out, int timeout_ms) {
    ReactorPollCtx* c = (ReactorPollCtx*)ctx;
    if (!c || !out || max_out <= 0 || c->count == 0) return 0;

    int pr = cpx_poll(c->fds, (unsigned long)c->count, timeout_ms);
    if (pr <= 0) return pr;

    int nout = 0;
    for (int i = 0; i < c->count && nout < max_out; i++) {
        if (c->fds[i].revents == 0) continue;
        out[nout].fd       = c->fds[i].fd;
        out[nout].events   = 0;
        if (c->fds[i].revents & POLLIN)  out[nout].events |= CPX_REACT_READ;
        if (c->fds[i].revents & POLLOUT) out[nout].events |= CPX_REACT_WRITE;
        out[nout].userdata = c->userdata[i];
        nout++;
    }
    return nout;
}

static void poll_destroy(void* ctx) {
    ReactorPollCtx* c = (ReactorPollCtx*)ctx;
    if (!c) return;
    free(c->fds);
    free(c->userdata);
    free(c);
}

static const CpxReactorVTable g_poll_vtable = {
    .init    = poll_init,
    .add     = poll_add,
    .mod     = poll_mod,
    .del     = poll_del,
    .wait    = poll_wait,
    .destroy = poll_destroy,
};

CpxReactor* cpx_reactor_create(void) {
    CpxReactor* r = (CpxReactor*)calloc(1, sizeof(CpxReactor));
    if (!r) return NULL;
    ReactorPollCtx* ctx = (ReactorPollCtx*)calloc(1, sizeof(ReactorPollCtx));
    if (!ctx) {
        free(r);
        return NULL;
    }
    r->vtable = &g_poll_vtable;
    r->ctx    = ctx;
    if (r->vtable->init(r->ctx, 256) != 0) {
        free(ctx);
        free(r);
        return NULL;
    }
    return r;
}

void cpx_reactor_destroy(CpxReactor* r) {
    if (!r || !r->vtable || !r->ctx) return;
    r->vtable->destroy(r->ctx);
    free(r);
}

int cpx_reactor_add(CpxReactor* r, int fd, uint32_t events, void* userdata) {
    if (!r || !r->vtable) return -1;
    return r->vtable->add(r->ctx, fd, events, userdata);
}

int cpx_reactor_mod(CpxReactor* r, int fd, uint32_t events) {
    if (!r || !r->vtable) return -1;
    return r->vtable->mod(r->ctx, fd, events);
}

int cpx_reactor_del(CpxReactor* r, int fd) {
    if (!r || !r->vtable) return -1;
    return r->vtable->del(r->ctx, fd);
}

int cpx_reactor_wait(CpxReactor* r, CpxIOEvent* out_events, int max_out, int timeout_ms) {
    if (!r || !r->vtable) return -1;
    return r->vtable->wait(r->ctx, out_events, max_out, timeout_ms);
}
