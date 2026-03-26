/**
 * Casperix Runtime - Virtual Dispatch Optimization
 *
 * Supplements the base object system (object.h/c) with:
 *   1. Compressed VTables — deduplicates inherited methods so child
 *      vtables only store overridden entries plus a diff list.
 *   2. Inline method caching (monomorphic / polymorphic) — caches the
 *      last-seen vtable→method resolution at each call-site to skip
 *      the indirection on repeat calls.
 *   3. Devirtualization hints — marks call sites observed to be
 *      monomorphic so the compiler can inline them on recompilation.
 *
 * These structures are allocated at program init and are immutable
 * at steady-state, making them thread-safe by construction.
 */

#ifndef CASPERIX_VTABLE_OPT_H
#define CASPERIX_VTABLE_OPT_H

#include "object.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Configuration ─── */

#define ICACHE_POLY_SIZE   4      /* Polymorphic cache entries          */
#define MAX_VTABLE_REGISTRY 512   /* Max registered vtables             */

/* ─── Compressed VTable ─── */

/**
 * A compressed vtable shares the parent's method array and only stores
 * (index, ptr) pairs for overridden methods.
 */
typedef struct {
    uint32_t method_index;
    void*    method_ptr;
} VTableOverride;

typedef struct CompressedVTable {
    VTable*              base;        /* Original full VTable (owned)   */
    struct CompressedVTable* parent;  /* Compressed parent (NULL=root)  */
    VTableOverride*      overrides;   /* Only the overridden methods    */
    uint32_t             num_overrides;
    uint32_t             total_methods; /* Same as base->num_methods    */
} CompressedVTable;

/* ─── Inline Method Cache (per call-site) ─── */

typedef enum {
    IC_EMPTY,          /* No cached entry                              */
    IC_MONOMORPHIC,    /* Exactly one type seen                        */
    IC_POLYMORPHIC,    /* 2..POLY_SIZE types seen                      */
    IC_MEGAMORPHIC     /* Too many types — fallback to vtable lookup   */
} ICacheState;

typedef struct {
    VTable* vtable;
    void*   method;
} ICacheEntry;

typedef struct {
    ICacheState   state;
    uint32_t      method_index;                  /* Slot being cached  */
    ICacheEntry   entries[ICACHE_POLY_SIZE];      /* Cached resolutions */
    uint32_t      num_entries;
    uint64_t      hits;
    uint64_t      misses;
} InlineCache;

/* ─── VTable Registry (for devirtualization feedback) ─── */

typedef struct {
    CompressedVTable*   entries[MAX_VTABLE_REGISTRY];
    uint32_t            count;
} VTableRegistry;

/* ═══════════════════════════════════════════════════════════════════════
 *  Compressed VTable API
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * Build a compressed vtable from a full VTable.
 * If `parent_cv` is non-NULL, only stores methods that differ from
 * the parent's method array.
 */
CompressedVTable* cvtable_create(VTable* full,
                                  CompressedVTable* parent_cv);

/**
 * Look up a method in a compressed vtable chain.
 * Walks child → parent until it finds an override for `method_index`,
 * else returns the base method.
 */
void* cvtable_lookup(CompressedVTable* cv, uint32_t method_index);

/**
 * Destroy a compressed vtable (does NOT free the underlying VTable).
 */
void cvtable_destroy(CompressedVTable* cv);

/* ═══════════════════════════════════════════════════════════════════════
 *  Inline Cache API
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * Initialize an inline cache for a specific method slot.
 */
void icache_init(InlineCache* ic, uint32_t method_index);

/**
 * Fast-path dispatch through inline cache.
 * Returns the cached method pointer if the vtable matches;
 * otherwise updates the cache and returns the resolved method.
 */
void* icache_dispatch(InlineCache* ic, void* obj);

/**
 * Reset an inline cache (e.g. on class reload / hot-swap).
 */
void icache_reset(InlineCache* ic);

/**
 * Check if the cache has been monomorphic (useful for devirt hints).
 */
bool icache_is_monomorphic(const InlineCache* ic);

/* ═══════════════════════════════════════════════════════════════════════
 *  Registry & Stats
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * Initialize the global vtable registry.
 */
void vtable_registry_init(VTableRegistry* reg);

/**
 * Register a compressed vtable.
 */
bool vtable_registry_add(VTableRegistry* reg, CompressedVTable* cv);

/**
 * Print inline-cache hit/miss statistics.
 */
void icache_print_stats(const InlineCache* ic);

/**
 * Print overall vtable registry summary.
 */
void vtable_registry_print(const VTableRegistry* reg);

#ifdef __cplusplus
}
#endif

#endif /* CASPERIX_VTABLE_OPT_H */
