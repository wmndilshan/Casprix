#include "iouring.h"

#include <stdio.h>
#include <string.h>

#ifdef __linux__
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#ifdef __linux__
static int cx_parse_kernel(const char* rel, int* major, int* minor) {
    if (!rel || !major || !minor) return -1;
    *major = 0;
    *minor = 0;
    return sscanf(rel, "%d.%d", major, minor) == 2 ? 0 : -1;
}
#endif

int cx_iouring_kernel_supported(void) {
#ifndef __linux__
    return 0;
#else
    struct utsname un;
    if (uname(&un) != 0) return 0;
    int major = 0;
    int minor = 0;
    if (cx_parse_kernel(un.release, &major, &minor) != 0) return 0;
    if (major > 5) return 1;
    return (major == 5 && minor >= 1) ? 1 : 0;
#endif
}

CxIoUring* cx_iouring_create(CxArena* arena, uint32_t entries) {
    if (!arena) return NULL;
    CxIoUring* out = (CxIoUring*)cx_arena_alloc_aligned(arena, sizeof(CxIoUring), 64);
    if (!out) return NULL;
    memset(out, 0, sizeof(*out));
    out->fd = -1;
    if (!cx_iouring_kernel_supported()) return out;

#ifdef __linux__
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    int fd = (int)syscall(SYS_io_uring_setup, entries, &p);
    if (fd < 0) return out;
    if ((p.sq_entries & (p.sq_entries - 1u)) != 0u) {
        close(fd);
        return out;
    }
    if ((p.cq_entries & (p.cq_entries - 1u)) != 0u) {
        close(fd);
        return out;
    }
    out->fd = fd;
    out->sq_entries = p.sq_entries;
    out->cq_entries = p.cq_entries;
    out->available = 1;
#else
    (void)entries;
#endif
    return out;
}

void cx_iouring_destroy(CxIoUring* uring) {
    if (!uring) return;
#ifdef __linux__
    if (uring->fd >= 0) close(uring->fd);
#endif
}
