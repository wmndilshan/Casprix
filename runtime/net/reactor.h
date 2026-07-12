#ifndef CASPRIX_REACTOR_H
#define CASPRIX_REACTOR_H

#include "cx_arena.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CX_REACTOR_MAX_FDS (1 << 20)
#define CX_REACTOR_MAX_EVENTS 1024

typedef struct CxReactor CxReactor;

typedef struct CxEventSlot {
    int    fd;
    uint32_t events;
    void* userdata;
    void (*on_read)(CxReactor*, int fd, void* userdata);
    void (*on_write)(CxReactor*, int fd, void* userdata);
    void (*on_close)(CxReactor*, int fd, void* userdata);
} CxEventSlot;

typedef struct CxReactor {
    int              epfd;
    int              max_fds;
    CxEventSlot*     slots;
    CxArena*         arena;
    pthread_rwlock_t slots_lock;
    _Atomic uint32_t running;
} CxReactor;

int        cx_set_nonblocking(int fd);
CxReactor* cx_reactor_create(CxArena* arena);
void       cx_reactor_destroy(CxReactor* r);
int        cx_reactor_add(CxReactor* r, int fd, uint32_t events,
                          void* userdata,
                          void (*on_read)(CxReactor*, int, void*),
                          void (*on_write)(CxReactor*, int, void*),
                          void (*on_close)(CxReactor*, int, void*));
int        cx_reactor_rearm(CxReactor* r, int fd, uint32_t events);
int        cx_reactor_remove(CxReactor* r, int fd);
void       cx_reactor_run(CxReactor* r);
void       cx_reactor_stop(CxReactor* r);

/* Backward-compatible aliases used by existing tests/helpers. */
typedef CxReactor CpxReactor;
#define cpx_reactor_create(arena) cx_reactor_create(arena)
#define cpx_reactor_destroy(r) cx_reactor_destroy(r)

#ifdef __cplusplus
}
#endif
#endif
