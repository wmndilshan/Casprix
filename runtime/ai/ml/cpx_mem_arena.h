/*
 * Casprix ML Runtime — Multi-Tier Memory Arena
 *
 * ════════════════════════════════════════════════════════════════════
 * MEMORY LIFECYCLE MODEL
 * ════════════════════════════════════════════════════════════════════
 *
 * Training lifecycle:
 *
 *   PARAMS arena   (permanent)
 *     Allocated once at model load.  Never freed during training.
 *     Layout:  [param_0 | pad | param_1 | pad | ...] — 64-byte aligned.
 *     Gradients live in a parallel arena with identical layout for
 *     direct index correspondence (grad[i] mirrors param[i]).
 *
 *   ACTIVATION arena (per forward pass, reset each step)
 *     Stack-discipline: push on forward, pop on backward.
 *     Activation checkpointing: only checkpoint tensors are kept;
 *     others are re-computed during backward via a saved execution plan.
 *     Per-thread private arenas eliminate false sharing.
 *
 *   GRAD arena  (per backward pass, reset after optimizer step)
 *     Gradient accumulation buffers.  In gradient-checkpointed mode,
 *     this arena grows during backward; non-checkpointed intermediates
 *     are freed immediately after their consumer is computed.
 *
 *   TEMP arena  (per operator, reset after each fused kernel)
 *     Packing buffers for GEMM (A-panel, B-panel), attention tile
 *     accumulators, transposed sub-tensors.  Size: ~2×KC×MC×4 bytes.
 *     Thread-local: each worker thread has its own TEMP arena.
 *
 *   OPTIMIZER arena (persistent, same lifetime as PARAMS)
 *     Adam first/second moments, AdamW weight decay mask, etc.
 *
 * Inference lifecycle:
 *
 *   PARAMS arena   — weights loaded once, read-only during decode
 *   KV-CACHE arena — per-sequence, grows monotonically with tokens,
 *                    freed when sequence ends
 *   TEMP arena     — single decode step, reset per token
 *
 * ════════════════════════════════════════════════════════════════════
 * FRAGMENTATION AVOIDANCE
 * ════════════════════════════════════════════════════════════════════
 *
 * Because each arena has strictly monotonic lifetime, there is zero
 * fragmentation within an arena.  The only fragmentation risk is
 * between arenas — mitigated by the fixed partitioning below.
 *
 * For dynamic-shape models (variable sequence lengths):
 *   The activation arena is over-provisioned for max_seq_len.
 *   Short sequences waste memory but never fragment.
 *
 * ════════════════════════════════════════════════════════════════════
 * ZERO-COPY TENSOR SLICING
 * ════════════════════════════════════════════════════════════════════
 *
 * CpxTensorView is a descriptor (shape + stride + data pointer) that
 * points into an existing allocation.  No data is copied.
 * Strides enable:
 *   - Transposition (swap strides, no data movement)
 *   - Head-splitting: view [B,S,H*D] as [B,S,H,D] by adjusting shape
 *   - Batch slice: view one batch element without copy
 */

#ifndef CPX_MEM_ARENA_H
#define CPX_MEM_ARENA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════════
 * 1. ARENA KINDS
 * ════════════════════════════════════════════════════════════════════ */

typedef enum {
    ARENA_PARAMS,       /* model weights — permanent                   */
    ARENA_GRAD,         /* gradient buffers — reset per step           */
    ARENA_ACTIVATION,   /* forward activations — stack discipline      */
    ARENA_TEMP,         /* per-kernel scratch — reset per op           */
    ARENA_OPTIMIZER,    /* optimizer state (Adam m/v) — permanent      */
    ARENA_KVCACHE,      /* KV-cache — grows per token, freed per seq   */
    ARENA_KIND_COUNT
} ArenaKind;

/* ════════════════════════════════════════════════════════════════════
 * 2. BUMP ARENA
 *
 * A slab of virtual memory pre-allocated with mmap (Linux) /
 * VirtualAlloc (Windows).  Allocation is a single pointer bump.
 * Deallocation is either a mark/reset (for stack-discipline arenas)
 * or a full arena_reset() (for step-scoped arenas).
 *
 * Thread safety: each thread gets its own private arena for
 * ACTIVATION and TEMP (no locking on the hot path).  PARAMS and
 * GRAD arenas are written only during init/update and are read-only
 * during the forward pass.
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t* base;          /* start of committed region               */
    size_t   committed;     /* bytes currently committed               */
    size_t   capacity;      /* total reserved virtual address space    */
    size_t   offset;        /* current bump pointer (bytes from base)  */
    size_t   peak;          /* high-water mark for profiling           */
    ArenaKind kind;
    int      thread_id;     /* -1 = shared                             */
} CpxArena;

/* Create an arena.  capacity bytes are reserved (not committed upfront
 * on systems supporting overcommit).  The first page is committed. */
CpxArena* cpx_arena_create(size_t capacity, ArenaKind kind, int thread_id);
void      cpx_arena_destroy(CpxArena* a);

/* Allocate `size` bytes, aligned to `align` (must be power of 2).
 * Returns NULL only if the arena is exhausted — treated as fatal. */
void* cpx_arena_alloc(CpxArena* a, size_t size, size_t align);

/* Convenience: alloc + zero-fill. */
void* cpx_arena_calloc(CpxArena* a, size_t size, size_t align);

/* Mark/reset for stack-discipline arenas. */
typedef size_t ArenaMarker;
ArenaMarker cpx_arena_mark(const CpxArena* a);
void        cpx_arena_reset_to(CpxArena* a, ArenaMarker mark);
void        cpx_arena_reset(CpxArena* a);   /* full reset to offset=0  */

/* Query. */
size_t cpx_arena_used(const CpxArena* a);
size_t cpx_arena_available(const CpxArena* a);
float  cpx_arena_utilization(const CpxArena* a);

/* ════════════════════════════════════════════════════════════════════
 * 3. TENSOR MEMORY POOL
 *
 * Wraps arenas into a named pool with type-safe tensor allocation.
 * Tensor descriptors (shape, stride, dtype) are allocated from a
 * separate small metadata arena so they don't pollute data alignment.
 * ════════════════════════════════════════════════════════════════════ */

#define CPX_MAX_DIMS  8

typedef enum {
    DTYPE_F32 = 0,
    DTYPE_F16,
    DTYPE_BF16,
    DTYPE_I8,
    DTYPE_I4_PACKED,   /* two INT4 per byte                            */
    DTYPE_I32,
} CpxDtype;

static inline size_t cpx_dtype_size(CpxDtype dt) {
    static const size_t sz[] = {4, 2, 2, 1, 0 /*special*/, 4};
    return sz[dt];
}

/*
 * A tensor view: descriptor only, no ownership.
 * Zero-copy slicing and transposition work by adjusting this struct.
 */
typedef struct {
    void*    data;                  /* pointer into an arena            */
    int      ndim;
    int      shape[CPX_MAX_DIMS];
    int      stride[CPX_MAX_DIMS];  /* in elements, not bytes           */
    size_t   offset;                /* element offset from data ptr     */
    CpxDtype dtype;
    bool     is_contiguous;         /* fast path flag                   */
} CpxTensorView;

/*
 * A tensor with ownership (backed by an arena allocation).
 */
typedef struct {
    CpxTensorView view;
    CpxArena*     arena;        /* owning arena (for lifecycle)         */
    ArenaMarker   alloc_mark;   /* arena offset at allocation time      */
    const char*   debug_name;   /* for profiling/debugging              */
} CpxTensor;

/* Allocate a tensor from an arena. */
CpxTensor* cpx_tensor_alloc(CpxArena* arena, CpxDtype dtype,
                              int ndim, const int* shape,
                              const char* debug_name);

/* Create a view (zero-copy slice). */
CpxTensorView cpx_tensor_slice(const CpxTensorView* t,
                                 int dim, int start, int end);

/* Transpose two dimensions (swap strides, no data copy). */
CpxTensorView cpx_tensor_transpose(const CpxTensorView* t, int d0, int d1);

/* Reshape (only works on contiguous tensors). */
CpxTensorView cpx_tensor_reshape(const CpxTensorView* t,
                                   int new_ndim, const int* new_shape);

/* Contiguous copy if strides are non-standard. */
CpxTensor* cpx_tensor_contiguous(const CpxTensorView* t, CpxArena* dst_arena);

/* Total element count. */
static inline size_t cpx_tensor_numel(const CpxTensorView* t) {
    size_t n = 1;
    for (int i = 0; i < t->ndim; i++) n *= (size_t)t->shape[i];
    return n;
}

/* Byte size of the tensor data. */
static inline size_t cpx_tensor_nbytes(const CpxTensorView* t) {
    if (t->dtype == DTYPE_I4_PACKED)
        return (cpx_tensor_numel(t) + 1) / 2;
    return cpx_tensor_numel(t) * cpx_dtype_size(t->dtype);
}

/* ════════════════════════════════════════════════════════════════════
 * 4. FULL MEMORY POOL — coordinates all arenas for a model
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    CpxArena* params;
    CpxArena* grad;
    CpxArena* optimizer;        /* Adam m/v, same size as params        */
    CpxArena* kvcache;

    /* Per-thread activation and temp arenas (num_threads entries). */
    CpxArena** activations;     /* [num_threads]                        */
    CpxArena** temp;            /* [num_threads]                        */
    int num_threads;

    /* Metadata: total reserved bytes per kind. */
    size_t reserved[ARENA_KIND_COUNT];
} CpxMemArena;

CpxMemArena* cpx_mem_arena_create(
    size_t param_bytes,
    size_t grad_bytes,
    size_t optimizer_bytes,
    size_t activation_bytes_per_thread,
    size_t temp_bytes_per_thread,
    size_t kvcache_bytes,
    int    num_threads);

void cpx_mem_arena_destroy(CpxMemArena* pool);

/* Reset activation + temp arenas for all threads (call between steps). */
void cpx_mem_arena_reset_step(CpxMemArena* pool);

/* Reset only the temp arena for a specific thread (call between ops). */
void cpx_mem_arena_reset_temp(CpxMemArena* pool, int thread_id);

/* Per-thread activation arena access. */
static inline CpxArena* cpx_mem_activation_arena(CpxMemArena* pool, int tid) {
    return pool->activations[tid];
}
static inline CpxArena* cpx_mem_temp_arena(CpxMemArena* pool, int tid) {
    return pool->temp[tid];
}

/* Print memory utilization report. */
void cpx_mem_arena_print_stats(const CpxMemArena* pool);

/* ════════════════════════════════════════════════════════════════════
 * 5. ACTIVATION CHECKPOINTING
 *
 * Instead of storing all activations for backward, only store
 * "checkpoint" activations at interval `ckpt_interval` (e.g. every
 * sqrt(N) layers).  Recompute the rest during backward.
 *
 * This reduces activation memory from O(N*L*D) to O(sqrt(N)*L*D)
 * at the cost of one extra forward pass per backward segment.
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    CpxTensor** checkpoints;    /* saved tensors at checkpoint layers   */
    int         num_checkpoints;
    int         ckpt_interval;
    CpxArena*   ckpt_arena;     /* dedicated arena for checkpoints      */
} CpxActivationCheckpoint;

CpxActivationCheckpoint* cpx_ckpt_create(int num_layers, int interval,
                                           size_t capacity_bytes);
void cpx_ckpt_destroy(CpxActivationCheckpoint* ckpt);

/* Save a checkpoint activation (copies tensor data). */
void cpx_ckpt_save(CpxActivationCheckpoint* ckpt, int layer_idx,
                    const CpxTensorView* t);

/* Retrieve saved checkpoint for recomputation. */
const CpxTensorView* cpx_ckpt_get(CpxActivationCheckpoint* ckpt, int layer_idx);

/* Reset all checkpoints (call between training steps). */
void cpx_ckpt_reset(CpxActivationCheckpoint* ckpt);

/* ════════════════════════════════════════════════════════════════════
 * 6. GRADIENT ACCUMULATION UTILITIES
 * ════════════════════════════════════════════════════════════════════ */

/* Accumulate: grad += delta (AVX2 vectorised). */
void cpx_grad_accumulate(float* grad, const float* delta, int n);

/* Scale: grad *= scale (for gradient clipping / lr scaling). */
void cpx_grad_scale(float* grad, float scale, int n);

/* L2 norm of gradient (for gradient clipping). */
float cpx_grad_norm(const float* grad, int n);

/* Clip gradient by global norm in-place. */
void cpx_grad_clip(float* grad, int n, float max_norm, float global_norm);

/* Zero all gradient arenas. */
void cpx_grad_zero_all(CpxMemArena* pool);

#ifdef __cplusplus
}
#endif

#endif /* CPX_MEM_ARENA_H */
