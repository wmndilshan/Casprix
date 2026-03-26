/**
 * Casperix Runtime - Virtual Dispatch Optimization
 *
 * Compressed vtables + inline method caching.
 */

#include "vtable_opt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════════════════
 *  Compressed VTable
 * ═══════════════════════════════════════════════════════════════════════ */

CompressedVTable* cvtable_create(VTable* full,
                                  CompressedVTable* parent_cv)
{
    if (!full) return NULL;

    CompressedVTable* cv = (CompressedVTable*)calloc(1, sizeof(CompressedVTable));
    if (!cv) return NULL;

    cv->base          = full;
    cv->parent        = parent_cv;
    cv->total_methods = full->num_methods;

    if (!parent_cv || !full->parent) {
        /* Root class: no parent to diff against → no overrides needed,
           methods are all in base->methods directly. */
        cv->overrides      = NULL;
        cv->num_overrides  = 0;
        return cv;
    }

    /* Diff against parent's full method table */
    VTable* parent_full = parent_cv->base;
    uint32_t shared = parent_full->num_methods;
    if (shared > full->num_methods) shared = full->num_methods;

    /* First pass: count overrides */
    uint32_t count = 0;
    for (uint32_t i = 0; i < shared; i++) {
        if (full->methods[i] != parent_full->methods[i])
            count++;
    }
    /* New methods (child has more than parent) are also "overrides" */
    count += (full->num_methods > parent_full->num_methods)
             ? (full->num_methods - parent_full->num_methods) : 0;

    if (count == 0) {
        cv->overrides     = NULL;
        cv->num_overrides = 0;
        return cv;
    }

    cv->overrides = (VTableOverride*)malloc(count * sizeof(VTableOverride));
    if (!cv->overrides) {
        free(cv);
        return NULL;
    }
    cv->num_overrides = count;

    /* Second pass: record overrides */
    uint32_t idx = 0;
    for (uint32_t i = 0; i < shared; i++) {
        if (full->methods[i] != parent_full->methods[i]) {
            cv->overrides[idx].method_index = i;
            cv->overrides[idx].method_ptr   = full->methods[i];
            idx++;
        }
    }
    for (uint32_t i = shared; i < full->num_methods; i++) {
        cv->overrides[idx].method_index = i;
        cv->overrides[idx].method_ptr   = full->methods[i];
        idx++;
    }

    return cv;
}

void* cvtable_lookup(CompressedVTable* cv, uint32_t method_index) {
    if (!cv) return NULL;

    /* Search overrides in this level */
    for (uint32_t i = 0; i < cv->num_overrides; i++) {
        if (cv->overrides[i].method_index == method_index)
            return cv->overrides[i].method_ptr;
    }

    /* Not overridden here — walk parent chain */
    if (cv->parent)
        return cvtable_lookup(cv->parent, method_index);

    /* Root: use base method table directly */
    if (cv->base && method_index < cv->base->num_methods)
        return cv->base->methods[method_index];

    return NULL;
}

void cvtable_destroy(CompressedVTable* cv) {
    if (!cv) return;
    free(cv->overrides);
    free(cv);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Inline Method Cache
 * ═══════════════════════════════════════════════════════════════════════ */

void icache_init(InlineCache* ic, uint32_t method_index) {
    memset(ic, 0, sizeof(InlineCache));
    ic->state        = IC_EMPTY;
    ic->method_index = method_index;
}

/**
 * Core dispatch: try the cache first, then resolve and update.
 */
void* icache_dispatch(InlineCache* ic, void* obj) {
    if (!obj || !ic) return NULL;

    VTable* vt = obj_get_vtable(obj);
    if (!vt) return NULL;

    /* ── Fast path: check cached entries ── */
    for (uint32_t i = 0; i < ic->num_entries; i++) {
        if (ic->entries[i].vtable == vt) {
            ic->hits++;
            return ic->entries[i].method;
        }
    }

    /* ── Slow path: resolve from vtable ── */
    ic->misses++;

    void* method = NULL;
    if (ic->method_index < vt->num_methods)
        method = vt->methods[ic->method_index];

    if (!method) return NULL;

    /* Update cache */
    switch (ic->state) {
        case IC_EMPTY:
            ic->entries[0].vtable = vt;
            ic->entries[0].method = method;
            ic->num_entries = 1;
            ic->state = IC_MONOMORPHIC;
            break;

        case IC_MONOMORPHIC:
        case IC_POLYMORPHIC:
            if (ic->num_entries < ICACHE_POLY_SIZE) {
                ic->entries[ic->num_entries].vtable = vt;
                ic->entries[ic->num_entries].method = method;
                ic->num_entries++;
                ic->state = IC_POLYMORPHIC;
            } else {
                /* Overflow → megamorphic: no more caching */
                ic->state = IC_MEGAMORPHIC;
            }
            break;

        case IC_MEGAMORPHIC:
            /* Just return — don't update cache */
            break;
    }

    return method;
}

void icache_reset(InlineCache* ic) {
    uint32_t idx = ic->method_index;
    memset(ic, 0, sizeof(InlineCache));
    ic->method_index = idx;
    ic->state = IC_EMPTY;
}

bool icache_is_monomorphic(const InlineCache* ic) {
    return ic->state == IC_MONOMORPHIC;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  VTable Registry
 * ═══════════════════════════════════════════════════════════════════════ */

void vtable_registry_init(VTableRegistry* reg) {
    memset(reg, 0, sizeof(VTableRegistry));
}

bool vtable_registry_add(VTableRegistry* reg, CompressedVTable* cv) {
    if (!reg || !cv) return false;
    if (reg->count >= MAX_VTABLE_REGISTRY) return false;
    reg->entries[reg->count++] = cv;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Diagnostics
 * ═══════════════════════════════════════════════════════════════════════ */

void icache_print_stats(const InlineCache* ic) {
    const char* state_str = "UNKNOWN";
    switch (ic->state) {
        case IC_EMPTY:        state_str = "EMPTY";        break;
        case IC_MONOMORPHIC:  state_str = "MONOMORPHIC";  break;
        case IC_POLYMORPHIC:  state_str = "POLYMORPHIC";  break;
        case IC_MEGAMORPHIC:  state_str = "MEGAMORPHIC";  break;
    }
    printf("  InlineCache[slot=%u]: state=%s  entries=%u  "
           "hits=%llu  misses=%llu  hit-rate=%.1f%%\n",
           ic->method_index, state_str, ic->num_entries,
           (unsigned long long)ic->hits,
           (unsigned long long)ic->misses,
           ic->hits + ic->misses > 0
               ? (double)ic->hits / (double)(ic->hits + ic->misses) * 100.0
               : 0.0);
}

void vtable_registry_print(const VTableRegistry* reg) {
    printf("=== VTable Registry ===\n");
    printf("  Registered: %u / %d\n", reg->count, MAX_VTABLE_REGISTRY);

    for (uint32_t i = 0; i < reg->count; i++) {
        CompressedVTable* cv = reg->entries[i];
        printf("  [%u] %s: %u methods, %u overrides",
               i,
               cv->base ? cv->base->class_name : "?",
               cv->total_methods,
               cv->num_overrides);
        if (cv->parent && cv->parent->base)
            printf(" (parent: %s)", cv->parent->base->class_name);
        printf("\n");
    }
}
