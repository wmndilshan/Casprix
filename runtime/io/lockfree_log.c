#include "lockfree_log.h"

#include "direct_io.h"
#include "fast_format.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
static DWORD WINAPI cpx_log_drain_thread(void* arg);
#else
#include <pthread.h>
#include <time.h>
static void* cpx_log_drain_thread(void* arg);
#endif

static bool cpx_is_pow2(uint32_t v) {
    return v && ((v & (v - 1u)) == 0u);
}

static int cpx_logq_pop_one(CpxLogQueue* q, CpxLogSlot** out_slot, uint64_t* out_pos) {
    for (;;) {
        uint64_t pos = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);
        CpxLogSlot* slot = &q->slots[pos & q->mask];
        const uint64_t seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
        const intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1u);
        if (dif == 0) {
            uint64_t expected = pos;
            if (atomic_compare_exchange_weak_explicit(
                    &q->dequeue_pos, &expected, pos + 1u,
                    memory_order_relaxed, memory_order_relaxed)) {
                *out_slot = slot;
                *out_pos = pos;
                return 1;
            }
        } else if (dif < 0) {
            return 0;
        }
    }
}

int cpx_logq_init(CpxLogQueue* q, uint32_t capacity_pow2, int out_fd) {
    if (!q || !cpx_is_pow2(capacity_pow2)) return -1;
    memset(q, 0, sizeof(*q));
    q->slots = (CpxLogSlot*)calloc(capacity_pow2, sizeof(CpxLogSlot));
    if (!q->slots) return -1;
    q->mask = (uint64_t)capacity_pow2 - 1u;
    q->out_fd = out_fd;
    for (uint32_t i = 0; i < capacity_pow2; ++i) {
        atomic_store_explicit(&q->slots[i].seq, (uint64_t)i, memory_order_relaxed);
    }
    atomic_store_explicit(&q->running, true, memory_order_release);
#if defined(_WIN32) || defined(_WIN64)
    HANDLE h = CreateThread(NULL, 0, cpx_log_drain_thread, q, 0, NULL);
    if (!h) {
        free(q->slots);
        q->slots = NULL;
        return -1;
    }
    q->thread_handle = (uintptr_t)h;
#else
    pthread_t t;
    if (pthread_create(&t, NULL, cpx_log_drain_thread, q) != 0) {
        free(q->slots);
        q->slots = NULL;
        return -1;
    }
    q->thread_handle = (uintptr_t)t;
#endif
    return 0;
}

void cpx_logq_shutdown(CpxLogQueue* q) {
    if (!q || !q->slots) return;
    atomic_store_explicit(&q->running, false, memory_order_release);
#if defined(_WIN32) || defined(_WIN64)
    HANDLE h = (HANDLE)q->thread_handle;
    if (h) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
#else
    pthread_t t = (pthread_t)q->thread_handle;
    if (q->thread_handle) pthread_join(t, NULL);
#endif
    free(q->slots);
    q->slots = NULL;
    q->thread_handle = 0;
}

bool cpx_logq_try_push(CpxLogQueue* q, CpxLogLevel level, const char* msg, size_t len) {
    if (!q || !q->slots || !msg) return false;
    if (len > CPX_LOG_MSG_MAX) len = CPX_LOG_MSG_MAX;
    for (;;) {
        uint64_t pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
        CpxLogSlot* slot = &q->slots[pos & q->mask];
        const uint64_t seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
        const intptr_t dif = (intptr_t)seq - (intptr_t)pos;
        if (dif == 0) {
            uint64_t expected = pos;
            if (atomic_compare_exchange_weak_explicit(
                    &q->enqueue_pos, &expected, pos + 1u,
                    memory_order_relaxed, memory_order_relaxed)) {
                slot->len = (uint16_t)len;
                slot->level = (uint8_t)level;
                memcpy(slot->msg, msg, len);
                atomic_store_explicit(&slot->seq, pos + 1u, memory_order_release);
                return true;
            }
        } else if (dif < 0) {
            atomic_fetch_add_explicit(&q->dropped, 1u, memory_order_relaxed);
            return false;
        }
    }
}

uint64_t cpx_logq_dropped(const CpxLogQueue* q) {
    if (!q) return 0;
    return atomic_load_explicit((const _Atomic uint64_t*)&q->dropped, memory_order_relaxed);
}

static void cpx_log_drain_once(CpxLogQueue* q) {
    CpxLogSlot* picked[32];
    uint64_t picked_pos[32];
    CpxIoVec iov[128]; /* level + time + msg + newline = 4 vectors per log */
    char ts_bufs[32][32];
    int n = 0;
    
    while (n < 32) {
        CpxLogSlot* slot = NULL;
        uint64_t pos = 0;
        if (!cpx_logq_pop_one(q, &slot, &pos)) break;
        picked[n] = slot;
        picked_pos[n] = pos;
        
        /* Format timestamp for this log batch */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        size_t ts_len = 0;
        
        /* Fast formatting for the timestamp: [sec.nsec] */
        ts_bufs[n][0] = '[';
        CpxFmtBuffer fb;
        cpx_fmt_init(&fb, ts_bufs[n] + 1, sizeof(ts_bufs[n]) - 1);
        cpx_fmt_append_u64(&fb, (uint64_t)ts.tv_sec);
        cpx_fmt_putc(&fb, '.');
        
        /* Pad nsec to 9 digits */
        uint64_t nsec = (uint64_t)ts.tv_nsec;
        char nsec_buf[9];
        for (int i = 8; i >= 0; --i) {
            nsec_buf[i] = (char)('0' + (nsec % 10));
            nsec /= 10;
        }
        cpx_fmt_append_mem(&fb, nsec_buf, 9);
        cpx_fmt_append_cstr(&fb, "] ");
        ts_len = fb.len + 1;

        const char* level_str = "[INFO]  ";
        if (slot->level == CPX_LOG_WARN) level_str = "[WARN]  ";
        else if (slot->level == CPX_LOG_ERROR) level_str = "[ERROR] ";
        else if (slot->level == CPX_LOG_DEBUG) level_str = "[DEBUG] ";

        iov[n * 4 + 0].iov_base = (void*)level_str;
        iov[n * 4 + 0].iov_len = 8;
        iov[n * 4 + 1].iov_base = ts_bufs[n];
        iov[n * 4 + 1].iov_len = ts_len;
        iov[n * 4 + 2].iov_base = (void*)slot->msg;
        iov[n * 4 + 2].iov_len = slot->len;
        iov[n * 4 + 3].iov_base = (void*)"\n";
        iov[n * 4 + 3].iov_len = 1;
        n++;
    }
    
    if (n == 0) return;
    (void)cpx_io_writev_all_fd(q->out_fd, iov, n * 4);
    
    for (int i = 0; i < n; ++i) {
        CpxLogSlot* slot = picked[i];
        atomic_store_explicit(&slot->seq, picked_pos[i] + q->mask + 1u, memory_order_release);
    }
}

#if defined(_WIN32) || defined(_WIN64)
static DWORD WINAPI cpx_log_drain_thread(void* arg) {
    CpxLogQueue* q = (CpxLogQueue*)arg;
    while (atomic_load_explicit(&q->running, memory_order_acquire)) {
        cpx_log_drain_once(q);
        Sleep(1);
    }
    cpx_log_drain_once(q);
    return 0;
}
#else
static void* cpx_log_drain_thread(void* arg) {
    CpxLogQueue* q = (CpxLogQueue*)arg;
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000L;
    while (atomic_load_explicit(&q->running, memory_order_acquire)) {
        cpx_log_drain_once(q);
        nanosleep(&ts, NULL);
    }
    cpx_log_drain_once(q);
    return NULL;
}
#endif
