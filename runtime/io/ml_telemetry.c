#include "ml_telemetry.h"

#include "fast_format.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
static DWORD WINAPI cpx_tlm_drain_worker(void* arg);
#else
#include <pthread.h>
#include <time.h>
static void* cpx_tlm_drain_worker(void* arg);
#endif

typedef struct {
    CpxTelemetryQueue* q;
} CpxTlmDrainCtx;

static bool cpx_is_pow2_u32(uint32_t x) {
    return x && ((x & (x - 1u)) == 0u);
}

static void cpx_tlm_format_line(char* out, size_t cap, const CpxTelemetryEvent* ev, size_t* out_len) {
    CpxFmtBuffer b;
    cpx_fmt_init(&b, out, cap);
    cpx_fmt_append_mem(&b, "step=", 5);
    cpx_fmt_append_u64(&b, ev->step);
    cpx_fmt_append_mem(&b, " worker=", 8);
    cpx_fmt_append_u64(&b, ev->worker_id);
    cpx_fmt_append_mem(&b, " loss=", 6);
    cpx_fmt_append_f64_6(&b, (double)ev->loss);
    cpx_fmt_append_mem(&b, " grad=", 6);
    cpx_fmt_append_f64_6(&b, (double)ev->grad_norm);
    cpx_fmt_append_mem(&b, " lr=", 4);
    cpx_fmt_append_f64_6(&b, (double)ev->lr);
    if (ev->text_len > 0) {
        cpx_fmt_append_mem(&b, " msg=", 5);
        cpx_fmt_append_mem(&b, ev->text, ev->text_len);
    }
    cpx_fmt_putc(&b, '\n');
    *out_len = b.len;
}

int cpx_tlm_init(CpxTelemetryQueue* q, uint32_t shard_count, uint32_t shard_capacity_pow2, int out_fd) {
    if (!q || shard_count == 0 || !cpx_is_pow2_u32(shard_capacity_pow2)) return -1;
    memset(q, 0, sizeof(*q));
    q->shards = (CpxTelemetryShard*)calloc(shard_count, sizeof(CpxTelemetryShard));
    if (!q->shards) return -1;
    q->shard_count = shard_count;
    q->out_fd = out_fd;
    for (uint32_t i = 0; i < shard_count; ++i) {
        CpxTelemetryShard* s = &q->shards[i];
        s->slots = (CpxTelemetryEvent*)calloc(shard_capacity_pow2, sizeof(CpxTelemetryEvent));
        if (!s->slots) {
            for (uint32_t j = 0; j < i; ++j) free(q->shards[j].slots);
            free(q->shards);
            q->shards = NULL;
            return -1;
        }
        s->cap = shard_capacity_pow2;
        s->mask = shard_capacity_pow2 - 1u;
        atomic_store_explicit(&s->write_idx, 0u, memory_order_relaxed);
        atomic_store_explicit(&s->read_idx, 0u, memory_order_relaxed);
    }
    atomic_store_explicit(&q->running, true, memory_order_release);
#if defined(_WIN32) || defined(_WIN64)
    HANDLE h = CreateThread(NULL, 0, cpx_tlm_drain_worker, q, 0, NULL);
    if (!h) {
        for (uint32_t i = 0; i < shard_count; ++i) free(q->shards[i].slots);
        free(q->shards);
        q->shards = NULL;
        return -1;
    }
    q->drain_thread = (uintptr_t)h;
#else
    pthread_t t;
    if (pthread_create(&t, NULL, cpx_tlm_drain_worker, q) != 0) {
        for (uint32_t i = 0; i < shard_count; ++i) free(q->shards[i].slots);
        free(q->shards);
        q->shards = NULL;
        return -1;
    }
    q->drain_thread = (uintptr_t)t;
#endif
    return 0;
}

void cpx_tlm_shutdown(CpxTelemetryQueue* q) {
    if (!q || !q->shards) return;
    atomic_store_explicit(&q->running, false, memory_order_release);
#if defined(_WIN32) || defined(_WIN64)
    HANDLE h = (HANDLE)q->drain_thread;
    if (h) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
#else
    if (q->drain_thread) {
        pthread_t t = (pthread_t)q->drain_thread;
        pthread_join(t, NULL);
    }
#endif
    for (uint32_t i = 0; i < q->shard_count; ++i) free(q->shards[i].slots);
    free(q->shards);
    q->shards = NULL;
    q->drain_thread = 0;
}

bool cpx_tlm_try_push(CpxTelemetryQueue* q, uint32_t shard_id, const CpxTelemetryEvent* ev) {
    if (!q || !q->shards || !ev || shard_id >= q->shard_count) return false;
    CpxTelemetryShard* s = &q->shards[shard_id];
    const uint32_t w = atomic_load_explicit(&s->write_idx, memory_order_relaxed);
    const uint32_t r = atomic_load_explicit(&s->read_idx, memory_order_acquire);
    if ((uint32_t)(w - r) >= s->cap) {
        atomic_fetch_add_explicit(&s->dropped, 1u, memory_order_relaxed);
        return false;
    }
    CpxTelemetryEvent* dst = &s->slots[w & s->mask];
    *dst = *ev;
    if (dst->text_len > CPX_TLM_TEXT_MAX) dst->text_len = CPX_TLM_TEXT_MAX;
    atomic_store_explicit(&s->write_idx, w + 1u, memory_order_release);
    return true;
}

uint64_t cpx_tlm_dropped_total(const CpxTelemetryQueue* q) {
    if (!q || !q->shards) return 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < q->shard_count; ++i) {
        total += atomic_load_explicit(&q->shards[i].dropped, memory_order_relaxed);
    }
    return total;
}

static void cpx_tlm_drain_once(CpxTelemetryQueue* q) {
    /* Use a stack-allocated batch buffer to minimize syscall frequency. */
    char lines[16][256];
    size_t lens[16];
    CpxIoVec iov[16];
    int batch_count = 0;

    for (uint32_t i = 0; i < q->shard_count; ++i) {
        CpxTelemetryShard* s = &q->shards[i];
        uint32_t r = atomic_load_explicit(&s->read_idx, memory_order_relaxed);
        const uint32_t w = atomic_load_explicit(&s->write_idx, memory_order_acquire);
        
        while (r != w && batch_count < 16) {
            CpxTelemetryEvent* ev = &s->slots[r & s->mask];
            cpx_tlm_format_line(lines[batch_count], sizeof(lines[batch_count]), ev, &lens[batch_count]);
            iov[batch_count].iov_base = lines[batch_count];
            iov[batch_count].iov_len = lens[batch_count];
            
            batch_count++;
            r++;
            /* Update read index after copying but before flush to allow shard reuse ASAP */
            atomic_store_explicit(&s->read_idx, r, memory_order_release);
            
            if (batch_count >= 16) {
                (void)cpx_io_writev_all_fd(q->out_fd, iov, batch_count);
                batch_count = 0;
            }
        }
    }
    
    if (batch_count > 0) {
        (void)cpx_io_writev_all_fd(q->out_fd, iov, batch_count);
    }
}

#if defined(_WIN32) || defined(_WIN64)
static DWORD WINAPI cpx_tlm_drain_worker(void* arg) {
    CpxTelemetryQueue* q = (CpxTelemetryQueue*)arg;
    while (atomic_load_explicit(&q->running, memory_order_acquire)) {
        cpx_tlm_drain_once(q);
        Sleep(1);
    }
    cpx_tlm_drain_once(q);
    return 0;
}
#else
static void* cpx_tlm_drain_worker(void* arg) {
    CpxTelemetryQueue* q = (CpxTelemetryQueue*)arg;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
    while (atomic_load_explicit(&q->running, memory_order_acquire)) {
        cpx_tlm_drain_once(q);
        nanosleep(&ts, NULL);
    }
    cpx_tlm_drain_once(q);
    return NULL;
}
#endif
