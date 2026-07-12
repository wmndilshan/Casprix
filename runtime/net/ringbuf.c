#include "ringbuf.h"

#include <assert.h>
#include <string.h>

#define CX_ASSERT(cond, msg) assert((cond) && (msg))

static int cx_is_pow2(size_t v) { return v != 0 && (v & (v - 1u)) == 0; }

CxRingBuffer* cx_ringbuf_create(CxArena* arena, size_t capacity_pow2) {
    if (!arena || !cx_is_pow2(capacity_pow2)) return NULL;
    CxRingBuffer* rb = (CxRingBuffer*)cx_arena_alloc_aligned(arena, sizeof(CxRingBuffer), 64);
    if (!rb) return NULL;
    memset(rb, 0, sizeof(*rb));
    rb->buf = (uint8_t*)cx_arena_alloc_aligned(arena, capacity_pow2, 4096);
    if (!rb->buf) return NULL;
    rb->capacity = capacity_pow2;
    rb->mask = capacity_pow2 - 1u;
    atomic_store_explicit(&rb->write_idx, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->read_idx, 0, memory_order_relaxed);
    return rb;
}

size_t cx_ringbuf_available_read(const CxRingBuffer* rb) {
    if (!rb) return 0;
    uint64_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
    uint64_t r = atomic_load_explicit(&rb->read_idx, memory_order_acquire);
    return (size_t)(w - r);
}

size_t cx_ringbuf_available_write(const CxRingBuffer* rb) {
    if (!rb) return 0;
    return rb->capacity - cx_ringbuf_available_read(rb);
}

size_t cx_ringbuf_write(CxRingBuffer* rb, const void* data, size_t len) {
    if (!rb || !data || len == 0) return 0;
    uint64_t r = atomic_load_explicit(&rb->read_idx, memory_order_acquire);
    uint64_t w = atomic_load_explicit(&rb->write_idx, memory_order_relaxed);
    uint64_t used = w - r;
    CX_ASSERT(used <= rb->capacity, "ring buffer overflow");

    size_t free_space = rb->capacity - (size_t)used;
    size_t n = (len < free_space) ? len : free_space;
    size_t first = n;
    size_t write_pos = (size_t)(w & rb->mask);
    size_t to_end = rb->capacity - write_pos;
    if (first > to_end) first = to_end;
    memcpy(rb->buf + write_pos, data, first);
    if (n > first) memcpy(rb->buf, (const uint8_t*)data + first, n - first);
    atomic_store_explicit(&rb->write_idx, w + n, memory_order_release);
    return n;
}

size_t cx_ringbuf_read(CxRingBuffer* rb, void* out, size_t len) {
    if (!rb || !out || len == 0) return 0;
    uint64_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
    uint64_t r = atomic_load_explicit(&rb->read_idx, memory_order_relaxed);
    uint64_t used = w - r;
    if (used == 0) return 0;

    size_t n = (len < (size_t)used) ? len : (size_t)used;
    size_t read_pos = (size_t)(r & rb->mask);
    size_t first = n;
    size_t to_end = rb->capacity - read_pos;
    if (first > to_end) first = to_end;
    memcpy(out, rb->buf + read_pos, first);
    if (n > first) memcpy((uint8_t*)out + first, rb->buf, n - first);
    atomic_store_explicit(&rb->read_idx, r + n, memory_order_release);
    return n;
}
