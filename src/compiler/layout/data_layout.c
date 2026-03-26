/**
 * Casperix Compiler - Data Layout Optimization
 *
 * Implements:
 *   1. Alignment-descending field reordering (minimal padding)
 *   2. Hot/cold field grouping within alignment tiers
 *   3. SOA candidate detection for small, partially-accessed structs
 *   4. Offset computation respecting natural alignment
 */

#include "data_layout.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─── Helpers ─── */

static size_t align_up(size_t val, size_t align) {
    return (val + align - 1) & ~(align - 1);
}

/* Compute naive layout size (original ordering). */
static size_t compute_naive_size(DLField* fields, size_t n, size_t* out_align) {
    size_t offset = 0;
    size_t max_align = 1;
    for (size_t i = 0; i < n; i++) {
        offset = align_up(offset, fields[i].alignment);
        offset += fields[i].size;
        if (fields[i].alignment > max_align)
            max_align = fields[i].alignment;
    }
    offset = align_up(offset, max_align);   /* trailing padding */
    *out_align = max_align;
    return offset;
}

/* ─── Comparator: descending alignment, then hot-first, then original order ─── */

static int field_cmp_align_hot(const void* a, const void* b) {
    const DLField* fa = (const DLField*)a;
    const DLField* fb = (const DLField*)b;

    /* Descending alignment */
    if (fa->alignment != fb->alignment)
        return (fa->alignment > fb->alignment) ? -1 : 1;

    /* Hot fields first (within same alignment tier) */
    if (fa->is_hot != fb->is_hot)
        return fa->is_hot ? -1 : 1;

    /* Preserve original order as tie-breaker */
    return (fa->original_idx < fb->original_idx) ? -1 : 1;
}

/* Comparator: alignment-only (no hot/cold) */
static int field_cmp_align_only(const void* a, const void* b) {
    const DLField* fa = (const DLField*)a;
    const DLField* fb = (const DLField*)b;

    if (fa->alignment != fb->alignment)
        return (fa->alignment > fb->alignment) ? -1 : 1;
    return (fa->original_idx < fb->original_idx) ? -1 : 1;
}

/* Compute offsets in current field order. */
static size_t compute_offsets(DLField* fields, size_t n, size_t* out_align) {
    size_t offset = 0;
    size_t max_align = 1;
    for (size_t i = 0; i < n; i++) {
        offset = align_up(offset, fields[i].alignment);
        fields[i].optimized_off = offset;
        offset += fields[i].size;
        if (fields[i].alignment > max_align)
            max_align = fields[i].alignment;
    }
    offset = align_up(offset, max_align);
    *out_align = max_align;
    return offset;
}

/* ─── SOA heuristic ─── */

/**
 * A struct is a good SOA candidate when:
 *   - It has >= 3 fields
 *   - At most 2 fields are "hot" (frequently accessed in loops)
 *   - Splitting hot from cold would save > 50 % bandwidth
 * This is a conservative heuristic used to emit codegen hints.
 */
static bool check_soa_candidate(DLStructLayout* s) {
    if (s->num_fields < 3) return false;

    size_t hot = 0, cold = 0;
    size_t hot_size = 0, cold_size = 0;
    for (size_t i = 0; i < s->num_fields; i++) {
        if (s->fields[i].is_hot) {
            hot++;
            hot_size += s->fields[i].size;
        } else {
            cold++;
            cold_size += s->fields[i].size;
        }
    }

    /* Need at least 1 hot field and cold fields to split */
    if (hot == 0 || cold == 0) return false;

    /* SOA wins when hot portion is < 50% of total struct size */
    return hot_size * 2 < s->optimized_size;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════ */

void dl_context_init(DLContext* ctx) {
    memset(ctx, 0, sizeof(DLContext));
    ctx->enable_hot_cold_split = true;
    ctx->enable_soa_hints      = true;
}

int dl_register_struct(DLContext* ctx, const char* name) {
    if (ctx->num_structs >= DL_MAX_STRUCTS) return -1;

    int idx = (int)ctx->num_structs++;
    DLStructLayout* s = &ctx->structs[idx];
    memset(s, 0, sizeof(DLStructLayout));
    s->name = name;
    return idx;
}

bool dl_add_field(DLContext* ctx, int struct_idx,
                  const char* name, size_t size, size_t alignment,
                  bool is_pointer)
{
    if (struct_idx < 0 || (size_t)struct_idx >= ctx->num_structs) return false;
    DLStructLayout* s = &ctx->structs[struct_idx];
    if (s->num_fields >= DL_MAX_FIELDS) return false;

    DLField* f = &s->fields[s->num_fields];
    f->name         = name;
    f->size         = size;
    f->alignment    = alignment > 0 ? alignment : 1;
    f->original_idx = s->num_fields;
    f->optimized_off = 0;
    f->access_count = 0;
    f->is_pointer   = is_pointer;
    f->is_hot       = false;
    s->num_fields++;
    return true;
}

void dl_set_access_count(DLContext* ctx, int struct_idx,
                         size_t field_idx, uint32_t count)
{
    if (struct_idx < 0 || (size_t)struct_idx >= ctx->num_structs) return;
    DLStructLayout* s = &ctx->structs[struct_idx];
    if (field_idx >= s->num_fields) return;

    s->fields[field_idx].access_count = count;
    s->fields[field_idx].is_hot = (count > 0);
}

void dl_optimize_all(DLContext* ctx) {
    ctx->total_padding_saved = 0;

    for (size_t i = 0; i < ctx->num_structs; i++) {
        DLStructLayout* s = &ctx->structs[i];
        if (s->num_fields == 0) continue;

        /* 1. Compute original (naive) size */
        s->original_size = compute_naive_size(
            s->fields, s->num_fields, &s->original_align);

        /* 2. Sort fields */
        if (ctx->enable_hot_cold_split) {
            qsort(s->fields, s->num_fields, sizeof(DLField),
                  field_cmp_align_hot);
        } else {
            qsort(s->fields, s->num_fields, sizeof(DLField),
                  field_cmp_align_only);
        }

        /* 3. Compute optimized offsets */
        s->optimized_size = compute_offsets(
            s->fields, s->num_fields, &s->optimized_align);

        /* 4. Padding delta */
        if (s->original_size > s->optimized_size) {
            s->padding_saved = s->original_size - s->optimized_size;
            ctx->total_padding_saved += s->padding_saved;
        } else {
            s->padding_saved = 0;
        }

        /* 5. SOA candidate check */
        if (ctx->enable_soa_hints) {
            s->soa_candidate = check_soa_candidate(s);
        }
    }
}

const DLStructLayout* dl_get_layout(const DLContext* ctx, int struct_idx) {
    if (struct_idx < 0 || (size_t)struct_idx >= ctx->num_structs)
        return NULL;
    return &ctx->structs[struct_idx];
}

void dl_print_report(const DLContext* ctx) {
    printf("=== Data Layout Optimization Report ===\n");
    printf("  Structs analysed:   %u\n", (unsigned)ctx->num_structs);
    printf("  Total padding saved:%u bytes\n\n", (unsigned)ctx->total_padding_saved);

    for (size_t i = 0; i < ctx->num_structs; i++) {
        const DLStructLayout* s = &ctx->structs[i];
        printf("  struct %s:\n", s->name ? s->name : "(anon)");
        printf("    Original  size=%u  align=%u\n",
               (unsigned)s->original_size, (unsigned)s->original_align);
        printf("    Optimized size=%u  align=%u  (saved %u bytes)\n",
               (unsigned)s->optimized_size, (unsigned)s->optimized_align,
               (unsigned)s->padding_saved);

        if (s->soa_candidate)
            printf("    ** SOA candidate -- consider splitting hot/cold **\n");

        printf("    Fields (optimized order):\n");
        for (size_t j = 0; j < s->num_fields; j++) {
            const DLField* f = &s->fields[j];
            printf("      [%2u] %-20s  off=%-4u  size=%-3u  align=%-2u %s%s\n",
                   (unsigned)j, f->name ? f->name : "?",
                   (unsigned)f->optimized_off, (unsigned)f->size,
                   (unsigned)f->alignment,
                   f->is_hot ? "HOT " : "",
                   f->is_pointer ? "PTR" : "");
        }
        printf("\n");
    }
}
