#include "reactor.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#define CX_ASSERT(cond, msg)                                              \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "reactor assert failed: %s (%s:%d)\n",         \
                    (msg), __FILE__, __LINE__);                            \
            abort();                                                       \
        }                                                                  \
    } while (0)

#ifdef __linux__
static int cx_reactor_validate_fd(const CxReactor* r, int fd) {
    return (r && fd >= 0 && fd < r->max_fds) ? 0 : -1;
}
#endif

int cx_set_nonblocking(int fd) {
#ifdef __linux__
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
#elif defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket((SOCKET)fd, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    (void)fd;
    return -1;
#endif
}

CxReactor* cx_reactor_create(CxArena* arena) {
    if (!arena) return NULL;
#ifndef __linux__
    (void)arena;
    return NULL;
#else
    CxReactor* r = (CxReactor*)cx_arena_alloc_aligned(arena, sizeof(CxReactor), 64);
    if (!r) return NULL;
    memset(r, 0, sizeof(*r));

    r->arena = arena;
    r->max_fds = CX_REACTOR_MAX_FDS;
    r->slots = (CxEventSlot*)cx_arena_alloc_aligned(
        arena, (size_t)r->max_fds * sizeof(CxEventSlot), 64);
    if (!r->slots) return NULL;
    memset(r->slots, 0, (size_t)r->max_fds * sizeof(CxEventSlot));

    r->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (r->epfd < 0) return NULL;
    if (pthread_rwlock_init(&r->slots_lock, NULL) != 0) {
        close(r->epfd);
        return NULL;
    }
    atomic_store_explicit(&r->running, 1u, memory_order_release);
    return r;
#endif
}

void cx_reactor_destroy(CxReactor* r) {
    if (!r) return;
#ifdef __linux__
    atomic_store_explicit(&r->running, 0u, memory_order_release);
    if (r->epfd >= 0) close(r->epfd);
    pthread_rwlock_destroy(&r->slots_lock);
#else
    (void)r;
#endif
}

int cx_reactor_add(CxReactor* r, int fd, uint32_t events,
                   void* userdata,
                   void (*on_read)(CxReactor*, int, void*),
                   void (*on_write)(CxReactor*, int, void*),
                   void (*on_close)(CxReactor*, int, void*)) {
#ifndef __linux__
    (void)r; (void)fd; (void)events; (void)userdata; (void)on_read; (void)on_write; (void)on_close;
    return -1;
#else
    if (!r || cx_reactor_validate_fd(r, fd) != 0) return -1;
    CX_ASSERT(fd >= 0 && fd < r->max_fds, "fd out of bounds");
    if (cx_set_nonblocking(fd) != 0) return -1;

    uint32_t ep_events = events | EPOLLET | EPOLLONESHOT;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = ep_events;
    ev.data.fd = fd;

    pthread_rwlock_wrlock(&r->slots_lock);
    CxEventSlot* slot = &r->slots[fd];
    slot->fd = fd;
    slot->events = ep_events;
    slot->userdata = userdata;
    slot->on_read = on_read;
    slot->on_write = on_write;
    slot->on_close = on_close;
    int ctl = epoll_ctl(r->epfd, EPOLL_CTL_ADD, fd, &ev);
    pthread_rwlock_unlock(&r->slots_lock);
    return ctl;
#endif
}

int cx_reactor_rearm(CxReactor* r, int fd, uint32_t events) {
#ifndef __linux__
    (void)r; (void)fd; (void)events;
    return -1;
#else
    if (!r || cx_reactor_validate_fd(r, fd) != 0) return -1;
    CX_ASSERT(fd >= 0 && fd < r->max_fds, "fd out of bounds");
    uint32_t ep_events = events | EPOLLET | EPOLLONESHOT;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = ep_events;
    ev.data.fd = fd;

    pthread_rwlock_wrlock(&r->slots_lock);
    r->slots[fd].events = ep_events;
    int ctl = epoll_ctl(r->epfd, EPOLL_CTL_MOD, fd, &ev);
    pthread_rwlock_unlock(&r->slots_lock);
    return ctl;
#endif
}

int cx_reactor_remove(CxReactor* r, int fd) {
#ifndef __linux__
    (void)r; (void)fd;
    return -1;
#else
    if (!r || cx_reactor_validate_fd(r, fd) != 0) return -1;
    CX_ASSERT(fd >= 0 && fd < r->max_fds, "fd out of bounds");

    pthread_rwlock_wrlock(&r->slots_lock);
    struct epoll_event ignored;
    memset(&ignored, 0, sizeof(ignored));
    int ctl = epoll_ctl(r->epfd, EPOLL_CTL_DEL, fd, &ignored);
    memset(&r->slots[fd], 0, sizeof(CxEventSlot));
    r->slots[fd].fd = -1;
    pthread_rwlock_unlock(&r->slots_lock);
    return ctl;
#endif
}

void cx_reactor_run(CxReactor* r) {
    if (!r) return;
#ifdef __linux__
    struct epoll_event events[CX_REACTOR_MAX_EVENTS];
    while (atomic_load_explicit(&r->running, memory_order_acquire) != 0u) {
        int n = epoll_wait(r->epfd, events, CX_REACTOR_MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (cx_reactor_validate_fd(r, fd) != 0) continue;

            CxEventSlot slot_copy;
            memset(&slot_copy, 0, sizeof(slot_copy));
            pthread_rwlock_rdlock(&r->slots_lock);
            slot_copy = r->slots[fd];
            pthread_rwlock_unlock(&r->slots_lock);

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                if (slot_copy.on_close) slot_copy.on_close(r, fd, slot_copy.userdata);
                cx_reactor_remove(r, fd);
                continue;
            }
            if ((events[i].events & EPOLLIN) && slot_copy.on_read) {
                slot_copy.on_read(r, fd, slot_copy.userdata);
            }
            if ((events[i].events & EPOLLOUT) && slot_copy.on_write) {
                slot_copy.on_write(r, fd, slot_copy.userdata);
            }
        }
    }
#else
    (void)r;
#endif
}

void cx_reactor_stop(CxReactor* r) {
    if (!r) return;
    atomic_store_explicit(&r->running, 0u, memory_order_release);
}
