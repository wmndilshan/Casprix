/*
 * Casprix ML Runtime — Memory Arena Implementation
 */

#include "cpx_mem_arena.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static void* os_reserve(size_t n) {
    return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
static void os_release(void* p, size_t n) {
    (void)n; VirtualFree(p, 0, MEM_RELEASE);
}
#else
#  include <sys/mman.h>
static void* os_reserve(size_t n) {
    void* p = mmap(NULL, n, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
}
static void os_release(void* p, size_t n) { munmap(p, n); }
#endif

/* ════════════════════════════════════════════════════════════════════
 * 1. CpxArena
 * ════════════════════════════════════════════════════════════════════ */

CpxArena* cpx_arena_create(size_t capacity, ArenaKind kind, int thread_id) {
    CpxArena* a = (CpxArena*)calloc(1, sizeof(CpxArena));
    if (!a) return NULL;
    a->base       = (uint8_t*)os_reserve(capacity);
    if (!a->base) { free(a); return NULL; }
    a->capacity   = capacity;
    a->committed  = capacity;
    a->offset     = 0;
    a->peak       = 0;
    a->kind       = kind;
    a->thread_id  = thread_id;
    return a;
}

void cpx_arena_destroy(CpxArena* a) {
    if (!a) return;
    if (a->base) os_release(a->base, a->capacity);
    free(a);
}

void* cpx_arena_alloc(CpxArena* a, size_t size, size_t align) {
    size_t off = (a->offset + align - 1) & ~(align - 1);
    if (off + size > a->capacity) return NULL;
    void* ptr = a->base + off;
    a->offset = off + size;
    if (a->offset > a->peak) a->peak = a->offset;
    return ptr;
}

void* cpx_arena_calloc(CpxArena* a, size_t size, size_t align) {
    void* p = cpx_arena_alloc(a, size, align);
    if (p) memset(p, 0, size);
    return p;
}

ArenaMarker cpx_arena_mark(const CpxArena* a) { return a->offset; }

void cpx_arena_reset_to(CpxArena* a, ArenaMarker mark) {
    assert(mark <= a->offset);
    a->offset = mark;
}

void cpx_arena_reset(CpxArena* a) { a->offset = 0; }

size_t cpx_arena_used(const CpxArena* a)        { return a->offset; }
size_t cpx_arena_available(const CpxArena* a)   { return a->capacity - a->offset; }
float  cpx_arena_utilization(const CpxArena* a) {
    return a->capacity ? (float)a->offset / (float)a->capacity : 0.f;
}

/* ════════════════════════════════════════════════════════════════════
 * 2. Tensor
 * ════════════════════════════════════════════════════════════════════ */

static void set_strides(CpxTensorView* v) {
    if (v->ndim == 0) return;
    v->stride[v->ndim - 1] = 1;
    for (int i = v->ndim - 2; i >= 0; i--)
        v->stride[i] = v->stride[i+1] * v->shape[i+1];
    v->is_contiguous = true;
}

CpxTensor* cpx_tensor_alloc(CpxArena* arena, CpxDtype dtype,
                               int ndim, const int* shape,
                               const char* debug_name) {
    CpxTensor* t = (CpxTensor*)cpx_arena_calloc(arena, sizeof(CpxTensor),
                                                   _Alignof(CpxTensor));
    if (!t) return NULL;
    t->view.dtype = dtype;
    t->view.ndim  = ndim;
    for (int i = 0; i < ndim; i++) t->view.shape[i] = shape[i];
    set_strides(&t->view);
    t->debug_name = debug_name;
    t->arena      = arena;
    t->alloc_mark = cpx_arena_mark(arena);

    size_t numel = cpx_tensor_numel(&t->view);
    size_t bytes = cpx_tensor_nbytes(&t->view);
    t->view.data  = cpx_arena_alloc(arena, bytes, 64);
    (void)numel;
    return t;
}

CpxTensorView cpx_tensor_slice(const CpxTensorView* src,
                                  int dim, int start, int end) {
    assert(dim >= 0 && dim < src->ndim);
    assert(start >= 0 && end <= src->shape[dim]);
    CpxTensorView v = *src;
    v.offset      += (size_t)start * (size_t)src->stride[dim];
    v.shape[dim]   = end - start;
    v.is_contiguous = false;
    return v;
}

CpxTensorView cpx_tensor_transpose(const CpxTensorView* src, int d0, int d1) {
    CpxTensorView v = *src;
    int ts = v.shape[d0];  v.shape[d0]  = v.shape[d1];  v.shape[d1]  = ts;
    int st = v.stride[d0]; v.stride[d0] = v.stride[d1]; v.stride[d1] = st;
    v.is_contiguous = false;
    return v;
}

CpxTensorView cpx_tensor_reshape(const CpxTensorView* src,
                                    int new_ndim, const int* new_shape) {
    assert(src->is_contiguous);
    CpxTensorView v = *src;
    v.ndim = new_ndim;
    for (int i = 0; i < new_ndim; i++) v.shape[i] = new_shape[i];
    set_strides(&v);
    return v;
}

CpxTensor* cpx_tensor_contiguous(const CpxTensorView* src, CpxArena* dst) {
    size_t bytes = cpx_tensor_nbytes(src);
    int shape[CPX_MAX_DIMS];
    for (int i = 0; i < src->ndim; i++) shape[i] = src->shape[i];
    CpxTensor* t = cpx_tensor_alloc(dst, src->dtype, src->ndim, shape, NULL);
    if (t && src->is_contiguous) {
        memcpy(t->view.data, (uint8_t*)src->data + src->offset * cpx_dtype_size(src->dtype), bytes);
    }
    return t;
}

/* ════════════════════════════════════════════════════════════════════
 * 3. CpxMemArena (6-tier pool)
 * ════════════════════════════════════════════════════════════════════ */

CpxMemArena* cpx_mem_arena_create(
    size_t param_bytes,
    size_t grad_bytes,
    size_t optimizer_bytes,
    size_t activation_bytes_per_thread,
    size_t temp_bytes_per_thread,
    size_t kvcache_bytes,
    int    num_threads) {

    CpxMemArena* p = (CpxMemArena*)calloc(1, sizeof(CpxMemArena));
    if (!p) return NULL;

    p->num_threads = num_threads;
    p->params    = cpx_arena_create(param_bytes,     ARENA_PARAMS,    -1);
    p->grad      = cpx_arena_create(grad_bytes,      ARENA_GRAD,      -1);
    p->optimizer = cpx_arena_create(optimizer_bytes, ARENA_OPTIMIZER, -1);
    p->kvcache   = cpx_arena_create(kvcache_bytes,   ARENA_KVCACHE,   -1);

    p->activations = (CpxArena**)malloc(num_threads * sizeof(CpxArena*));
    p->temp        = (CpxArena**)malloc(num_threads * sizeof(CpxArena*));
    if (!p->activations || !p->temp) { cpx_mem_arena_destroy(p); return NULL; }

    for (int t = 0; t < num_threads; t++) {
        p->activations[t] = cpx_arena_create(activation_bytes_per_thread,
                                               ARENA_ACTIVATION, t);
        p->temp[t]        = cpx_arena_create(temp_bytes_per_thread,
                                               ARENA_TEMP, t);
    }

    p->reserved[ARENA_PARAMS]     = param_bytes;
    p->reserved[ARENA_GRAD]       = grad_bytes;
    p->reserved[ARENA_OPTIMIZER]  = optimizer_bytes;
    p->reserved[ARENA_KVCACHE]    = kvcache_bytes;
    p->reserved[ARENA_ACTIVATION] = activation_bytes_per_thread * num_threads;
    p->reserved[ARENA_TEMP]       = temp_bytes_per_thread * num_threads;

    return p;
}

void cpx_mem_arena_destroy(CpxMemArena* pool) {
    if (!pool) return;
    cpx_arena_destroy(pool->params);
    cpx_arena_destroy(pool->grad);
    cpx_arena_destroy(pool->optimizer);
    cpx_arena_destroy(pool->kvcache);
    if (pool->activations) {
        for (int t = 0; t < pool->num_threads; t++)
            cpx_arena_destroy(pool->activations[t]);
        free(pool->activations);
    }
    if (pool->temp) {
        for (int t = 0; t < pool->num_threads; t++)
            cpx_arena_destroy(pool->temp[t]);
        free(pool->temp);
    }
    free(pool);
}

void cpx_mem_arena_reset_step(CpxMemArena* pool) {
    cpx_arena_reset(pool->grad);
    for (int t = 0; t < pool->num_threads; t++) {
        cpx_arena_reset(pool->activations[t]);
        cpx_arena_reset(pool->temp[t]);
    }
}

void cpx_mem_arena_reset_temp(CpxMemArena* pool, int thread_id) {
    cpx_arena_reset(pool->temp[thread_id]);
}

void cpx_mem_arena_print_stats(const CpxMemArena* pool) {
    static const char* names[] = {
        "PARAMS","GRAD","ACTIVATION","TEMP","OPTIMIZER","KVCACHE"
    };
    static const char* border_top  = "╔══════════════════╦══════════╦════════════╗\n";
    static const char* border_mid  = "╠══════════════════╬══════════╬════════════╣\n";
    static const char* border_bot  = "╚══════════════════╩══════════╩════════════╝\n";
    printf("%s", border_top);
    printf("║ Arena            ║ Used MiB ║ Cap MiB    ║\n");
    printf("%s", border_mid);
    CpxArena* arenas[] = {
        pool->params, pool->grad,
        pool->num_threads ? pool->activations[0] : NULL,
        pool->num_threads ? pool->temp[0] : NULL,
        pool->optimizer, pool->kvcache
    };
    for (int i = 0; i < 6; i++) {
        CpxArena* a = arenas[i];
        if (!a) continue;
        printf("║ %-16s ║%9.2f ║%11.2f ║\n",
               names[i],
               (double)a->offset / (1024.0*1024.0),
               (double)a->capacity / (1024.0*1024.0));
    }
    printf("%s", border_bot);
}

/* ════════════════════════════════════════════════════════════════════
 * 4. Activation Checkpointing
 * ════════════════════════════════════════════════════════════════════ */

CpxActivationCheckpoint* cpx_ckpt_create(int num_layers, int interval,
                                            size_t capacity_bytes) {
    CpxActivationCheckpoint* ckpt = (CpxActivationCheckpoint*)
        calloc(1, sizeof(CpxActivationCheckpoint));
    ckpt->ckpt_interval   = interval;
    ckpt->num_checkpoints = (num_layers + interval - 1) / interval;
    ckpt->ckpt_arena      = cpx_arena_create(capacity_bytes, ARENA_ACTIVATION, -1);
    ckpt->checkpoints     = (CpxTensor**)cpx_arena_calloc(
                                ckpt->ckpt_arena,
                                ckpt->num_checkpoints * sizeof(CpxTensor*),
                                sizeof(void*));
    return ckpt;
}

void cpx_ckpt_destroy(CpxActivationCheckpoint* ckpt) {
    if (!ckpt) return;
    cpx_arena_destroy(ckpt->ckpt_arena);
    free(ckpt);
}

void cpx_ckpt_save(CpxActivationCheckpoint* ckpt, int layer_idx,
                    const CpxTensorView* t) {
    int slot = layer_idx / ckpt->ckpt_interval;
    if (slot >= ckpt->num_checkpoints) return;
    /* Copy descriptor only (data lives in activation arena). */
    CpxTensor* stored = (CpxTensor*)cpx_arena_calloc(
        ckpt->ckpt_arena, sizeof(CpxTensor), _Alignof(CpxTensor));
    if (stored) {
        stored->view = *t;
        ckpt->checkpoints[slot] = stored;
    }
}

const CpxTensorView* cpx_ckpt_get(CpxActivationCheckpoint* ckpt,
                                     int layer_idx) {
    int slot = layer_idx / ckpt->ckpt_interval;
    if (slot >= ckpt->num_checkpoints) return NULL;
    CpxTensor* t = ckpt->checkpoints[slot];
    return t ? &t->view : NULL;
}

void cpx_ckpt_reset(CpxActivationCheckpoint* ckpt) {
    cpx_arena_reset(ckpt->ckpt_arena);
    size_t sz = ckpt->num_checkpoints * sizeof(CpxTensor*);
    ckpt->checkpoints = (CpxTensor**)cpx_arena_calloc(
                            ckpt->ckpt_arena, sz, sizeof(void*));
}

/* ════════════════════════════════════════════════════════════════════
 * 5. Gradient Utilities
 * ════════════════════════════════════════════════════════════════════ */

void cpx_grad_accumulate(float* grad, const float* delta, int n) {
    for (int i = 0; i < n; i++) grad[i] += delta[i];
}

void cpx_grad_scale(float* grad, float scale, int n) {
    for (int i = 0; i < n; i++) grad[i] *= scale;
}

float cpx_grad_norm(const float* grad, int n) {
    float ss = 0.f;
    for (int i = 0; i < n; i++) ss += grad[i] * grad[i];
    return sqrtf(ss);
}

void cpx_grad_clip(float* grad, int n, float max_norm, float global_norm) {
    if (global_norm > max_norm) {
        float s = max_norm / (global_norm + 1e-6f);
        for (int i = 0; i < n; i++) grad[i] *= s;
    }
}

void cpx_grad_zero_all(CpxMemArena* pool) {
    if (pool->grad) cpx_arena_reset(pool->grad);
}
