#include "direct_io.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#else
#include <unistd.h>
#endif

#if defined(__x86_64__) && !defined(_WIN32) && !defined(_WIN64)
static inline long cpx_sys_write(long fd, const void* buf, size_t len) {
    register long rax __asm__("rax") = 1L; /* SYS_write */
    register long rdi __asm__("rdi") = fd;
    register long rsi __asm__("rsi") = (long)(uintptr_t)buf;
    register long rdx __asm__("rdx") = (long)len;
    __asm__ volatile("syscall"
                     : "+r"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx)
                     : "rcx", "r11", "memory");
    return rax;
}

static inline long cpx_sys_writev(long fd, const CpxIoVec* iov, int iovcnt) {
    register long rax __asm__("rax") = 20L; /* SYS_writev */
    register long rdi __asm__("rdi") = fd;
    register long rsi __asm__("rsi") = (long)(uintptr_t)iov;
    register long rdx __asm__("rdx") = (long)iovcnt;
    __asm__ volatile("syscall"
                     : "+r"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx)
                     : "rcx", "r11", "memory");
    return rax;
}
#endif

int cpx_io_write_all(CpxIoHandle handle, const void* buf, size_t len) {
    if (!buf || len == 0) return 0;
#if defined(_WIN32) || defined(_WIN64)
    const uint8_t* p = (const uint8_t*)buf;
    size_t total = 0;
    while (total < len) {
        DWORD chunk = (DWORD)((len - total) > 0x7fffffffU ? 0x7fffffffU : (len - total));
        DWORD written = 0;
        if (!WriteFile(handle, p + total, chunk, &written, NULL)) return -1;
        if (written == 0) return -1;
        total += (size_t)written;
    }
    return 0;
#else
    int fd = (int)handle;
    const uint8_t* p = (const uint8_t*)buf;
    size_t total = 0;
    while (total < len) {
#if defined(__x86_64__)
        long n = cpx_sys_write((long)fd, p + total, len - total);
        if (n < 0) {
            if (n == -EINTR) continue;
            return -1;
        }
#else
        ssize_t n = write(fd, p + total, len - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
#endif
        if (n == 0) return -1;
        total += (size_t)n;
    }
    return 0;
#endif
}

int cpx_io_write_all_fd(int fd, const void* buf, size_t len) {
#if defined(_WIN32) || defined(_WIN64)
    HANDLE h = INVALID_HANDLE_VALUE;
    if (fd == 1) h = GetStdHandle(STD_OUTPUT_HANDLE);
    else if (fd == 2) h = GetStdHandle(STD_ERROR_HANDLE);
    else h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE || h == NULL) return -1;
    return cpx_io_write_all(h, buf, len);
#else
    return cpx_io_write_all(fd, buf, len);
#endif
}

int cpx_io_writev_all_fd(int fd, const CpxIoVec* iov, int iovcnt) {
    if (!iov || iovcnt <= 0) return 0;
#if defined(_WIN32) || defined(_WIN64)
    for (int i = 0; i < iovcnt; ++i) {
        if (cpx_io_write_all_fd(fd, iov[i].iov_base, iov[i].iov_len) != 0) return -1;
    }
    return 0;
#else
    int cur = 0;
    size_t skip = 0;
    while (cur < iovcnt) {
        CpxIoVec tmp[32];
        int n = 0;
        for (int i = cur; i < iovcnt && n < 32; ++i, ++n) tmp[n] = iov[i];
        if (skip > 0 && n > 0) {
            tmp[0].iov_base = (void*)((const uint8_t*)tmp[0].iov_base + skip);
            tmp[0].iov_len -= skip;
        }
#if defined(__x86_64__)
        long w = cpx_sys_writev((long)fd, tmp, n);
        if (w < 0) {
            if (w == -EINTR) continue;
            return -1;
        }
#else
        ssize_t w = writev(fd, (const struct iovec*)tmp, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
#endif
        if (w == 0) return -1;
        size_t consumed = (size_t)w;
        while (cur < iovcnt && consumed > 0) {
            size_t rem = iov[cur].iov_len - skip;
            if (consumed < rem) {
                skip += consumed;
                consumed = 0;
            } else {
                consumed -= rem;
                cur++;
                skip = 0;
            }
        }
    }
    return 0;
#endif
}
