/*
 * Casprix ML Runtime — KV-Cache Engine
 *
 * ════════════════════════════════════════════════════════════════════
 * MEMORY LAYOUT
 * ════════════════════════════════════════════════════════════════════
 *
 * CONTIGUOUS layout (best for short, fixed-length sequences):
 *   K[layer][head][seq][d_head]  — shape [L, H, S, D]
 *   V[layer][head][seq][d_head]  — shape [L, H, S, D]
 *   Stride order: layer > head > seq > d_head
 *   Advantage: single allocation, cache-friendly inner loops.
 *   Cost: max_seq_len × num_heads × d_head per layer, always live.
 *
 * PAGED layout (best for variable-length, concurrent batches):
 *   Physical pages of PAGE_SIZE tokens (default 16).
 *   Page table: seq_pos → page_ptr (one per sequence in batch).
 *   Advantage: zero internal fragmentation, can grow/shrink.
 *   Cost: indirection on every KV read; page-table cache miss.
 *   Used in: vLLM, TensorRT-LLM, llama.cpp batch inference.
 *
 * Hybrid strategy:
 *   Short requests (< SMALL_SEQ_THRESHOLD): contiguous.
 *   Long / streaming requests: paged.
 *   Promoted automatically in cpx_kvcache_maybe_promote().
 *
 * ════════════════════════════════════════════════════════════════════
 * SPECULATIVE DECODING
 * ════════════════════════════════════════════════════════════════════
 *
 * Draft phase:
 *   Draft model generates K candidate tokens t[i+1..i+K].
 *   Each token written to speculative_slots[] which are NOT yet
 *   committed to the main KV cache.
 *
 * Verify phase:
 *   Target model scores all K+1 tokens in one batched forward.
 *   Accepted prefix of length j: commit j slots to main cache.
 *   Rejected remainder: cpx_kvcache_rollback(j) discards tail.
 *
 * cpx_kvcache_rollback(seq, new_len):
 *   Resets seq_len to new_len.
 *   Paged variant: returns pages past new_len to free list.
 *   Contiguous variant: just updates seq_len (no dealloc needed).
 *
 * ════════════════════════════════════════════════════════════════════
 * MULTI-QUERY & GROUPED-QUERY ATTENTION (MQA / GQA)
 * ════════════════════════════════════════════════════════════════════
 *
 * MQA (num_kv_heads=1):  K and V stored once for all Q heads.
 * GQA (num_kv_heads=H/G): H query heads share G KV heads.
 * Cache shape: [L, num_kv_heads, S, D] not [L, num_heads, S, D].
 * Each Q head at index h reads KV head at index h / (H/G).
 * cpx_kvcache_create takes num_kv_heads as explicit parameter.
 *
 * ════════════════════════════════════════════════════════════════════
 * QUANTIZED KV CACHE
 * ════════════════════════════════════════════════════════════════════
 *
 * Reduce memory pressure for long contexts:
 *   KV_QUANT_INT8: 4× compression, ~0.1 PPL degradation at 2048 ctx.
 *   KV_QUANT_INT4: 8× compression, noticeable at > 4096 ctx.
 * Scales stored per head, per token (not per channel — avoids
 * extra memory for short seqs).
 *
 * Write path: cpx_kvcache_write → quantize_row_int8 → store.
 * Read path:  cpx_kvcache_read_range → dequant to scratch buf → return.
 */

#ifndef CPX_KVCACHE_H
#define CPX_KVCACHE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "cpx_mem_arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════════
 * 1. CONFIGURATION
 * ════════════════════════════════════════════════════════════════════ */

#define CPX_KV_MAX_LAYERS       64
#define CPX_KV_MAX_HEADS       128
#define CPX_KV_MAX_HEAD_DIM    256
#define CPX_KV_PAGE_SIZE        16   /* tokens per page (paged mode) */
#define CPX_KV_SMALL_SEQ_THRESH 512  /* use contiguous below this    */
#define CPX_KV_MAX_SPEC_TOKENS   16  /* max speculative draft length */

typedef enum {
    KV_QUANT_NONE = 0,
    KV_QUANT_INT8,
    KV_QUANT_INT4,
} KvQuantType;

typedef enum {
    KV_LAYOUT_CONTIGUOUS = 0,
    KV_LAYOUT_PAGED,
} KvLayout;

typedef struct {
    int          num_layers;
    int          num_kv_heads;   /* GQA: < num_q_heads */
    int          head_dim;
    int          max_seq_len;    /* max tokens per sequence          */
    int          max_batch;      /* max concurrent sequences         */
    KvLayout     layout;
    KvQuantType  quant;          /* optional KV compression          */
    int          page_size;      /* paged: tokens per page           */
    CpxArena*    backing_arena;  /* CpxMemArena.kvcache              */
} CpxKvCacheConfig;

/* ════════════════════════════════════════════════════════════════════
 * 2. SEQUENCE SLOT (one per concurrent request in a batch)
 * ════════════════════════════════════════════════════════════════════ */

/*
 * Page-table entry for paged layout.
 * Each entry holds a pointer to a slab of CPX_KV_PAGE_SIZE tokens.
 */
typedef struct {
    float* k_page;   /* [page_size, head_dim] */
    float* v_page;   /* [page_size, head_dim] */
} CpxKvPage;

typedef struct {
    int    seq_id;            /* unique request ID                   */
    int    seq_len;           /* tokens written so far               */
    int    max_len;           /* allocated capacity (contiguous)     */
    bool   is_paged;

    union {
        /* Contiguous: K and V blocks for all layers. */
        struct {
            float* k_base;   /* [num_layers, num_kv_heads, max_len, head_dim] */
            float* v_base;
        };
        /* Paged: pointer tables per layer per head. */
        struct {
            CpxKvPage** pages; /* [num_layers × num_kv_heads][num_pages] */
            int          num_pages;
        };
    };

    /* Speculative decoding state. */
    int   spec_start;         /* token index where spec begins       */
    int   spec_len;           /* number of speculative tokens written*/
} CpxKvSeq;

/* ════════════════════════════════════════════════════════════════════
 * 3. KV CACHE OBJECT
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    CpxKvCacheConfig config;
    CpxKvSeq*        seqs;         /* [max_batch]                    */
    int              num_seqs;     /* currently active sequences      */

    /* Free page pool (paged mode). */
    CpxKvPage*       page_pool;    /* flat array of all pages         */
    int              page_pool_cap;
    CpxKvPage**      free_pages;   /* stack of free page pointers     */
    int              free_page_top;

    /* INT8 quantization scales when quant != NONE.
     * Scales stored per-token per-head: [L, H, seq] float32. */
    float*           k_scales;
    float*           v_scales;

    /* Scratch buffer for dequantisation (single-threaded read path). */
    float*           dequant_scratch; /* [head_dim]                  */

    uint64_t         total_bytes_alloc;
} CpxKvCache;

/* ════════════════════════════════════════════════════════════════════
 * 4. LIFECYCLE
 * ════════════════════════════════════════════════════════════════════ */

CpxKvCache* cpx_kvcache_create(const CpxKvCacheConfig* cfg);
void        cpx_kvcache_destroy(CpxKvCache* cache);

/* Allocate a new sequence slot; returns seq_id >= 0 or -1 on OOM. */
int  cpx_kvcache_alloc_seq(CpxKvCache* cache, int initial_len);
void cpx_kvcache_free_seq(CpxKvCache* cache, int seq_id);

/* Pre-fill (prompt): bulk-write P tokens at once. */
void cpx_kvcache_prefill(CpxKvCache* cache, int seq_id,
                           int layer, int head,
                           const float* k,  /* [P, head_dim] */
                           const float* v,  /* [P, head_dim] */
                           int P);

/* ════════════════════════════════════════════════════════════════════
 * 5. WRITE / READ
 * ════════════════════════════════════════════════════════════════════ */

/*
 * Write a single new KV pair at position seq_len (auto-increments).
 * k, v: [head_dim] f32 vectors.
 */
void CPX_HOT cpx_kvcache_write(CpxKvCache* cache, int seq_id,
                                 int layer, int head,
                                 const float* k, const float* v);

/*
 * Read a contiguous range [start, end) of KV pairs for one head.
 * k_out, v_out: caller-provided buffers [end-start, head_dim].
 * For quantised cache, dequantises into the provided buffers.
 */
void CPX_HOT cpx_kvcache_read_range(const CpxKvCache* cache,
                                      int seq_id, int layer, int head,
                                      int start, int end,
                                      float* k_out, float* v_out);

/*
 * Return a direct pointer to K/V row at position `pos`.
 * Only valid for CONTIGUOUS layout and unquantised cache.
 * Returns NULL otherwise (caller must use read_range).
 */
const float* cpx_kvcache_k_ptr(const CpxKvCache* cache,
                                  int seq_id, int layer, int head,
                                  int pos);
const float* cpx_kvcache_v_ptr(const CpxKvCache* cache,
                                  int seq_id, int layer, int head,
                                  int pos);

/* ════════════════════════════════════════════════════════════════════
 * 6. SPECULATIVE DECODING
 * ════════════════════════════════════════════════════════════════════ */

/*
 * Begin a speculative region.  Subsequent cpx_kvcache_write calls
 * land in the speculative staging area (not yet committed).
 */
void cpx_kvcache_spec_begin(CpxKvCache* cache, int seq_id);

/*
 * Commit the first `accepted` speculative tokens.
 * Remaining draft tokens are discarded.
 * After commit, seq_len advances by `accepted`.
 */
void cpx_kvcache_spec_commit(CpxKvCache* cache, int seq_id,
                               int accepted);

/*
 * Rollback: rewind seq_len to `new_len`, discarding all
 * tokens written after new_len (including speculative ones).
 * For paged caches, pages past new_len are returned to the pool.
 */
void cpx_kvcache_rollback(CpxKvCache* cache, int seq_id, int new_len);

/* ════════════════════════════════════════════════════════════════════
 * 7. BATCH UTILITIES
 * ════════════════════════════════════════════════════════════════════ */

/*
 * Compact attention: during decode, all active sequences have
 * different lengths. This computes the maximum sequence length
 * across all active seqs (needed for causal mask dimension).
 */
int cpx_kvcache_max_seq_len(const CpxKvCache* cache);

/* Gather KV tensors for all active sequences into a single
 * padded batch for grouped-query attention.
 *   k_batch: [num_seqs, num_kv_heads, max_len, head_dim]
 *   v_batch: same shape
 * Returns the actual max_len after gathering (padding applied). */
int cpx_kvcache_gather_batch(const CpxKvCache* cache,
                               int layer,
                               float* k_batch, float* v_batch);

/* Stats / diagnostics. */
typedef struct {
    uint64_t total_tokens;
    uint64_t live_tokens;
    uint64_t bytes_used;
    uint64_t bytes_total;
    int      num_active_seqs;
    int      num_free_pages;    /* paged mode */
    float    utilization;       /* live_tokens / total_tokens        */
} CpxKvCacheStats;

void cpx_kvcache_stats(const CpxKvCache* cache, CpxKvCacheStats* out);

#ifdef __cplusplus
}
#endif

#endif /* CPX_KVCACHE_H */
