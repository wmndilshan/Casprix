#include "checkpoint_io.h"

#include <errno.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/fs.h>
#endif
#endif

#if defined(__x86_64__) && !defined(_WIN32) && !defined(_WIN64)
static inline long cpx_sys_openat(long dfd, const char* path, long flags, long mode) {
    register long rax __asm__("rax") = 257L; /* SYS_openat */
    register long rdi __asm__("rdi") = dfd;
    register long rsi __asm__("rsi") = (long)(uintptr_t)path;
    register long rdx __asm__("rdx") = flags;
    register long r10 __asm__("r10") = mode;
    __asm__ volatile("syscall"
                     : "+r"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)
                     : "rcx", "r11", "memory");
    return rax;
}
static inline long cpx_sys_fsync(long fd) {
    register long rax __asm__("rax") = 74L; /* SYS_fsync */
    register long rdi __asm__("rdi") = fd;
    __asm__ volatile("syscall" : "+r"(rax) : "r"(rdi) : "rcx", "r11", "memory");
    return rax;
}
static inline long cpx_sys_close(long fd) {
    register long rax __asm__("rax") = 3L; /* SYS_close */
    register long rdi __asm__("rdi") = fd;
    __asm__ volatile("syscall" : "+r"(rax) : "r"(rdi) : "rcx", "r11", "memory");
    return rax;
}
#endif

int cpx_ckpt_open(CpxCheckpointWriter* w, const char* path, uint32_t flags) {
    if (!w || !path) return -1;
    memset(w, 0, sizeof(*w));
    w->fd = -1;
    w->flags = flags;
#if defined(_WIN32) || defined(_WIN64)
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int fd = _open_osfhandle((intptr_t)h, 0);
    if (fd < 0) {
        CloseHandle(h);
        return -1;
    }
    w->fd = fd;
#else
    const long open_flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
#if defined(__x86_64__)
    long fd = cpx_sys_openat(-100L /* AT_FDCWD */, path, open_flags, 0644L);
    if (fd < 0) return -1;
    w->fd = (int)fd;
#else
    int fd = open(path, (int)open_flags, 0644);
    if (fd < 0) return -1;
    w->fd = fd;
#endif
#endif
    return 0;
}

int cpx_ckpt_write(CpxCheckpointWriter* w, const void* data, size_t len) {
    if (!w || w->fd < 0) return -1;
    return cpx_io_write_all_fd(w->fd, data, len);
}

int cpx_ckpt_writev(CpxCheckpointWriter* w, const CpxIoVec* iov, int iovcnt) {
    if (!w || w->fd < 0) return -1;
    return cpx_io_writev_all_fd(w->fd, iov, iovcnt);
}

int cpx_ckpt_close(CpxCheckpointWriter* w) {
    if (!w || w->fd < 0) return -1;
    int rc = 0;
#if defined(_WIN32) || defined(_WIN64)
    if (w->flags & CPX_CKPT_F_FSYNC_ON_CLOSE) {
        HANDLE h = (HANDLE)_get_osfhandle(w->fd);
        if (h == INVALID_HANDLE_VALUE || !FlushFileBuffers(h)) rc = -1;
    }
    if (_close(w->fd) != 0) rc = -1;
#else
    if (w->flags & CPX_CKPT_F_FSYNC_ON_CLOSE) {
#if defined(__x86_64__)
        if (cpx_sys_fsync((long)w->fd) < 0) rc = -1;
#else
        if (fsync(w->fd) != 0) rc = -1;
#endif
    }
#if defined(__linux__)
    if (w->flags & CPX_CKPT_F_CACHE_DROP_HINT) {
        (void)posix_fadvise(w->fd, 0, 0, POSIX_FADV_DONTNEED);
    }
#endif
#if defined(__x86_64__)
    if (cpx_sys_close((long)w->fd) < 0) rc = -1;
#else
    if (close(w->fd) != 0) rc = -1;
#endif
#endif
    w->fd = -1;
    return rc;
}
