#ifndef CASPRIX_REACTOR_H
#define CASPRIX_REACTOR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CPX_REACT_READ  1u
#define CPX_REACT_WRITE 2u

typedef struct {
    int      fd;
    uint32_t events;
    void*    userdata;
} CpxIOEvent;

typedef struct {
    int  (*init)(void* ctx, int max_events);
    int  (*add)(void* ctx, int fd, uint32_t events, void* userdata);
    int  (*mod)(void* ctx, int fd, uint32_t events);
    int  (*del)(void* ctx, int fd);
    int  (*wait)(void* ctx, CpxIOEvent* out_events, int max_out, int timeout_ms);
    void (*destroy)(void* ctx);
} CpxReactorVTable;

typedef struct {
    const CpxReactorVTable* vtable;
    void*                   ctx;
} CpxReactor;

CpxReactor* cpx_reactor_create(void);
void        cpx_reactor_destroy(CpxReactor* r);

int cpx_reactor_add(CpxReactor* r, int fd, uint32_t events, void* userdata);
int cpx_reactor_mod(CpxReactor* r, int fd, uint32_t events);
int cpx_reactor_del(CpxReactor* r, int fd);
int cpx_reactor_wait(CpxReactor* r, CpxIOEvent* out_events, int max_out, int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
