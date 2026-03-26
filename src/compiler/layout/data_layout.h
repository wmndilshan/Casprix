/**
 * Casperix Compiler - Data Layout Optimization
 *
 * Optimizes struct field ordering and array-of-struct transformations
 * to improve cache locality and reduce padding waste.
 *
 * Passes:
 *   1. Field reordering: sort fields by descending alignment to
 *      minimize padding (C-compatible, deterministic).
 *   2. Hot/cold field splitting: separate frequently-accessed fields
 *      from rarely-accessed ones so hot data fits in cache lines.
 *   3. SOA transform hints: for arrays of small structs where only one
 *      or two fields are accessed in a loop, suggest/emit SOA layout.
 */

#ifndef CASPERIX_DATA_LAYOUT_H
#define CASPERIX_DATA_LAYOUT_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Limits ─── */

#define DL_MAX_FIELDS       128
#define DL_MAX_STRUCTS      256
#define DL_CACHE_LINE       64     /* x86-64 cache line size          */

/* ─── Field descriptor ─── */

typedef struct {
    const char*     name;          /* Field identifier                 */
    size_t          size;          /* Size in bytes                    */
    size_t          alignment;     /* Required alignment               */
    size_t          original_idx;  /* Index in the source definition   */
    size_t          optimized_off; /* Computed offset after reorder    */
    uint32_t        access_count;  /* Profile/heuristic access count   */
    bool            is_pointer;    /* Reference type (needs GC trace)  */
    bool            is_hot;        /* Frequently accessed              */
} DLField;

/* ─── Struct layout descriptor ─── */

typedef struct {
    const char*     name;                  /* Struct/class name        */
    DLField         fields[DL_MAX_FIELDS]; /* Field list               */
    size_t          num_fields;
    size_t          original_size;         /* Before optimization      */
    size_t          optimized_size;        /* After optimization       */
    size_t          original_align;
    size_t          optimized_align;
    size_t          padding_saved;         /* Bytes of padding removed */
    bool            soa_candidate;         /* Recommend SOA transform  */
} DLStructLayout;

/* ─── Layout optimizer context ─── */

typedef struct {
    DLStructLayout  structs[DL_MAX_STRUCTS];
    size_t          num_structs;
    size_t          total_padding_saved;
    bool            enable_hot_cold_split;
    bool            enable_soa_hints;
} DLContext;

/* ─── API ─── */

/**
 * Initialize a data-layout optimization context.
 */
void dl_context_init(DLContext* ctx);

/**
 * Register a struct for layout optimization.
 * Returns the index or -1 on overflow.
 */
int dl_register_struct(DLContext* ctx, const char* name);

/**
 * Add a field to a registered struct.
 * Returns true on success.
 */
bool dl_add_field(DLContext* ctx, int struct_idx,
                  const char* name, size_t size, size_t alignment,
                  bool is_pointer);

/**
 * Set heuristic access count for a field (from profiling or static
 * analysis).  Fields with count > 0 are considered "hot".
 */
void dl_set_access_count(DLContext* ctx, int struct_idx,
                         size_t field_idx, uint32_t count);

/**
 * Run the optimization passes on all registered structs:
 *   - Field reordering (alignment-descending)
 *   - Hot/cold grouping (if enabled)
 *   - Compute optimized offsets and sizes
 *   - SOA candidate detection
 */
void dl_optimize_all(DLContext* ctx);

/**
 * Get the optimized layout for a struct by index.
 */
const DLStructLayout* dl_get_layout(const DLContext* ctx, int struct_idx);

/**
 * Print a human-readable layout report.
 */
void dl_print_report(const DLContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* CASPERIX_DATA_LAYOUT_H */
