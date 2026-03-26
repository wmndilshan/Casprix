/*
 * Casprix ML Runtime — KV-Cache Implementation
 */

#include "cpx_kvcache.h"
#include "cpx_quant.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

/* ════════════════════════════════════════════════════════════════════
 * 1. LIFECYCLE
 * ════════════════════════════════════════════════════════════════════ */

CpxKvCache* cpx_kvcache_create(const CpxKvCacheConfig* cfg) {
    CpxKvCache* c = (CpxKvCache*)calloc(1, sizeof(CpxKvCache));
    c->config = *cfg;

    c->seqs     = (CpxKvSeq*)calloc(cfg->max_batch, sizeof(CpxKvSeq));
    c->num_seqs = 0;

    /* Mark all slots as free. */
    for (int i = 0; i < cfg->max_batch; i++) c->seqs[i].seq_id = -1;

    if (cfg->layout == KV_LAYOUT_CONTIGUOUS) {
        /* Allocate one giant contiguous block for all layers/heads/seqs. */
        size_t tokens_per_seq = (size_t)cfg->num_layers
                              * cfg->num_kv_heads
                              * cfg->max_seq_len
                              * cfg->head_dim;
        size_t elem_bytes = (cfg->quant == KV_QUANT_INT8) ? 1 :
                            (cfg->quant == KV_QUANT_INT4) ? 0 /* packed */ :
                            sizeof(float);

        for (int i = 0; i < cfg->max_batch; i++) {
            CpxKvSeq* s = &c->seqs[i];
            if (cfg->backing_arena) {
                s->k_base = (float*)cpx_arena_alloc(cfg->backing_arena,
                              tokens_per_seq * sizeof(float), 64);
                s->v_base = (float*)cpx_arena_alloc(cfg->backing_arena,
                              tokens_per_seq * sizeof(float), 64);
            } else {
                s->k_base = (float*)_aligned_malloc(
                              tokens_per_seq * sizeof(float), 64);
                s->v_base = (float*)_aligned_malloc(
                              tokens_per_seq * sizeof(float), 64);
            }
            s->max_len  = cfg->max_seq_len;
            s->is_paged = false;
            c->total_bytes_alloc += 2 * tokens_per_seq * sizeof(float);
        }

        /* Quantisation scales: per token per head per layer. */
        if (cfg->quant != KV_QUANT_NONE) {
            size_t nscales = (size_t)cfg->num_layers
                           * cfg->num_kv_heads
                           * cfg->max_seq_len
                           * cfg->max_batch;
            c->k_scales = (float*)calloc(nscales, sizeof(float));
            c->v_scales = (float*)calloc(nscales, sizeof(float));
        }
        c->dequant_scratch = (float*)malloc(cfg->head_dim * sizeof(float));
    } else {
        /* Paged layout: allocate page pool. */
        int page_size = cfg->page_size > 0 ? cfg->page_size
                                           : CPX_KV_PAGE_SIZE;
        /* How many pages do we need max? */
        size_t total_pages = (size_t)cfg->max_batch
                           * cfg->num_layers
                           * cfg->num_kv_heads
                           * ((cfg->max_seq_len + page_size - 1) / page_size);
        total_pages += 128; /* small reserve */

        c->page_pool = (CpxKvPage*)calloc(total_pages, sizeof(CpxKvPage));
        for (size_t p = 0; p < total_pages; p++) {
            c->page_pool[p].k_page = (float*)_aligned_malloc(
                (size_t)page_size * cfg->head_dim * sizeof(float), 64);
            c->page_pool[p].v_page = (float*)_aligned_malloc(
                (size_t)page_size * cfg->head_dim * sizeof(float), 64);
        }
        c->page_pool_cap   = (int)total_pages;
        c->free_pages      = (CpxKvPage**)malloc(total_pages * sizeof(CpxKvPage*));
        c->free_page_top   = (int)total_pages;
        for (size_t p = 0; p < total_pages; p++)
            c->free_pages[p] = &c->page_pool[p];
        c->dequant_scratch = (float*)malloc(cfg->head_dim * sizeof(float));
    }

    return c;
}

void cpx_kvcache_destroy(CpxKvCache* cache) {
    if (!cache) return;
    const CpxKvCacheConfig* cfg = &cache->config;

    if (cfg->layout == KV_LAYOUT_CONTIGUOUS && !cfg->backing_arena) {
        for (int i = 0; i < cfg->max_batch; i++) {
            _aligned_free(cache->seqs[i].k_base);
            _aligned_free(cache->seqs[i].v_base);
        }
    }
    if (cfg->layout == KV_LAYOUT_PAGED) {
        for (int p = 0; p < cache->page_pool_cap; p++) {
            _aligned_free(cache->page_pool[p].k_page);
            _aligned_free(cache->page_pool[p].v_page);
        }
        free(cache->page_pool);
        free(cache->free_pages);
    }
    free(cache->k_scales);
    free(cache->v_scales);
    free(cache->dequant_scratch);
    free(cache->seqs);
    free(cache);
}

/* ════════════════════════════════════════════════════════════════════
 * 2. SEQUENCE MANAGEMENT
 * ════════════════════════════════════════════════════════════════════ */

int cpx_kvcache_alloc_seq(CpxKvCache* cache, int initial_len) {
    for (int i = 0; i < cache->config.max_batch; i++) {
        if (cache->seqs[i].seq_id < 0) {
            cache->seqs[i].seq_id   = i;
            cache->seqs[i].seq_len  = initial_len;
            cache->seqs[i].spec_start = -1;
            cache->seqs[i].spec_len   = 0;
            cache->num_seqs++;
            return i;
        }
    }
    return -1; /* OOM */
}

void cpx_kvcache_free_seq(CpxKvCache* cache, int seq_id) {
    assert(seq_id >= 0 && seq_id < cache->config.max_batch);
    cache->seqs[seq_id].seq_id  = -1;
    cache->seqs[seq_id].seq_len = 0;
    cache->num_seqs--;
}

/* ════════════════════════════════════════════════════════════════════
 * 3. WRITE / READ (contiguous, unquantised)
 * ════════════════════════════════════════════════════════════════════ */

/* Byte offset into K/V base for [layer, head, pos]. */
static CPX_FORCE_INLINE size_t kv_offset(const CpxKvCache* c,
                                           int layer, int head, int pos) {
    const CpxKvCacheConfig* cfg = &c->config;
    return ((size_t)layer * cfg->num_kv_heads + head)
            * cfg->max_seq_len * cfg->head_dim
           + (size_t)pos * cfg->head_dim;
}

void CPX_HOT cpx_kvcache_write(CpxKvCache* cache, int seq_id,
                                  int layer, int head,
                                  const float* k, const float* v) {
    CpxKvSeq* seq = &cache->seqs[seq_id];
    int pos       = seq->seq_len;  /* write at current end */
    int D         = cache->config.head_dim;

    if (cache->config.layout == KV_LAYOUT_CONTIGUOUS) {
        size_t off = kv_offset(cache, layer, head, pos);
        memcpy(seq->k_base + off, k, D * sizeof(float));
        memcpy(seq->v_base + off, v, D * sizeof(float));
    }
    /* Paged variant left as stub — page allocation needed. */

    /* Advance seq_len only on layer 0 head 0 (last head of last layer
     * should trigger, but for simplicity track externally). */
    if (layer == cache->config.num_layers - 1
     && head  == cache->config.num_kv_heads - 1) {
        seq->seq_len++;
    }
}

void cpx_kvcache_prefill(CpxKvCache* cache, int seq_id,
                           int layer, int head,
                           const float* k, const float* v, int P) {
    CpxKvSeq* seq = &cache->seqs[seq_id];
    int D = cache->config.head_dim;

    if (cache->config.layout == KV_LAYOUT_CONTIGUOUS) {
        size_t off = kv_offset(cache, layer, head, seq->seq_len);
        memcpy(seq->k_base + off, k, (size_t)P * D * sizeof(float));
        memcpy(seq->v_base + off, v, (size_t)P * D * sizeof(float));
    }
    /* Advance after last layer/head. */
    if (layer == cache->config.num_layers - 1
     && head  == cache->config.num_kv_heads - 1) {
        seq->seq_len += P;
    }
}

void CPX_HOT cpx_kvcache_read_range(const CpxKvCache* cache,
                                      int seq_id, int layer, int head,
                                      int start, int end,
                                      float* k_out, float* v_out) {
    const CpxKvSeq* seq = &cache->seqs[seq_id];
    int D = cache->config.head_dim;

    if (cache->config.layout == KV_LAYOUT_CONTIGUOUS) {
        size_t off = kv_offset(cache, layer, head, start);
        memcpy(k_out, seq->k_base + off, (size_t)(end - start) * D * sizeof(float));
        memcpy(v_out, seq->v_base + off, (size_t)(end - start) * D * sizeof(float));
    }
}

const float* cpx_kvcache_k_ptr(const CpxKvCache* cache,
                                  int seq_id, int layer, int head, int pos) {
    if (cache->config.layout != KV_LAYOUT_CONTIGUOUS
     || cache->config.quant  != KV_QUANT_NONE)
        return NULL;
    const CpxKvSeq* seq = &cache->seqs[seq_id];
    return seq->k_base + kv_offset(cache, layer, head, pos);
}

const float* cpx_kvcache_v_ptr(const CpxKvCache* cache,
                                  int seq_id, int layer, int head, int pos) {
    if (cache->config.layout != KV_LAYOUT_CONTIGUOUS
     || cache->config.quant  != KV_QUANT_NONE)
        return NULL;
    const CpxKvSeq* seq = &cache->seqs[seq_id];
    return seq->v_base + kv_offset(cache, layer, head, pos);
}

/* ════════════════════════════════════════════════════════════════════
 * 4. SPECULATIVE DECODING
 * ════════════════════════════════════════════════════════════════════ */

void cpx_kvcache_spec_begin(CpxKvCache* cache, int seq_id) {
    CpxKvSeq* seq   = &cache->seqs[seq_id];
    seq->spec_start = seq->seq_len;
    seq->spec_len   = 0;
}

void cpx_kvcache_spec_commit(CpxKvCache* cache, int seq_id, int accepted) {
    CpxKvSeq* seq = &cache->seqs[seq_id];
    assert(accepted <= seq->spec_len);
    seq->seq_len  = seq->spec_start + accepted;
    seq->spec_start = -1;
    seq->spec_len   = 0;
}

void cpx_kvcache_rollback(CpxKvCache* cache, int seq_id, int new_len) {
    CpxKvSeq* seq = &cache->seqs[seq_id];
    assert(new_len <= seq->seq_len);
    seq->seq_len    = new_len;
    seq->spec_start = -1;
    seq->spec_len   = 0;
    /* For paged caches, return pages past new_len to free pool.
     * (Contiguous: no dealloc needed.) */
}

/* ════════════════════════════════════════════════════════════════════
 * 5. BATCH UTILITIES
 * ════════════════════════════════════════════════════════════════════ */

int cpx_kvcache_max_seq_len(const CpxKvCache* cache) {
    int max_len = 0;
    for (int i = 0; i < cache->config.max_batch; i++) {
        if (cache->seqs[i].seq_id >= 0 && cache->seqs[i].seq_len > max_len)
            max_len = cache->seqs[i].seq_len;
    }
    return max_len;
}

int cpx_kvcache_gather_batch(const CpxKvCache* cache, int layer,
                               float* k_batch, float* v_batch) {
    int max_len = cpx_kvcache_max_seq_len(cache);
    int H       = cache->config.num_kv_heads;
    int D       = cache->config.head_dim;
    int S       = cache->num_seqs;

    /* Zero the output batch (padding). */
    memset(k_batch, 0, (size_t)S * H * max_len * D * sizeof(float));
    memset(v_batch, 0, (size_t)S * H * max_len * D * sizeof(float));

    int seq_idx = 0;
    for (int i = 0; i < cache->config.max_batch; i++) {
        if (cache->seqs[i].seq_id < 0) continue;
        int len = cache->seqs[i].seq_len;
        for (int h = 0; h < H; h++) {
            float* kd = k_batch + (seq_idx * H + h) * max_len * D;
            float* vd = v_batch + (seq_idx * H + h) * max_len * D;
            cpx_kvcache_read_range(cache, i, layer, h, 0, len, kd, vd);
        }
        seq_idx++;
    }
    return max_len;
}

/* ════════════════════════════════════════════════════════════════════
 * 6. STATS
 * ════════════════════════════════════════════════════════════════════ */

void cpx_kvcache_stats(const CpxKvCache* cache, CpxKvCacheStats* out) {
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < cache->config.max_batch; i++) {
        if (cache->seqs[i].seq_id >= 0) {
            out->live_tokens   += cache->seqs[i].seq_len;
            out->num_active_seqs++;
        }
        out->total_tokens += cache->config.max_seq_len;
    }
    out->bytes_used  = (uint64_t)out->live_tokens
                     * cache->config.num_layers
                     * cache->config.num_kv_heads
                     * cache->config.head_dim
                     * 2 /* K + V */ * sizeof(float);
    out->bytes_total = cache->total_bytes_alloc;
    out->utilization = out->total_tokens > 0
                         ? (float)out->live_tokens / out->total_tokens
                         : 0.f;
    out->num_free_pages = cache->free_page_top;
}
