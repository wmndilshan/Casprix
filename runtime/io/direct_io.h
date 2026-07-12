#ifndef CASPRIX_DIRECT_IO_H
#define CASPRIX_DIRECT_IO_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
typedef HANDLE CpxIoHandle;
typedef struct {
    const void* iov_base;
    size_t      iov_len;
} CpxIoVec;
#else
#include <sys/uio.h>
typedef int CpxIoHandle;
typedef struct iovec CpxIoVec;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Writes exactly len bytes unless a hard error occurs. */
int cpx_io_write_all(CpxIoHandle handle, const void* buf, size_t len);

/* POSIX fd helper (stdout=1, stderr=2). */
int cpx_io_write_all_fd(int fd, const void* buf, size_t len);

/* Vectored write helper for batched log draining. */
int cpx_io_writev_all_fd(int fd, const CpxIoVec* iov, int iovcnt);

#ifdef __cplusplus
}
#endif

#endif /* CASPRIX_DIRECT_IO_H */
