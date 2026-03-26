/*
 * Casprix Compiler — MIR Borrow Checker
 *
 * Static analysis pass that validates ownership and borrowing rules
 * on the MIR. Operates after lowering and (optionally) after optimization.
 *
 * Rules enforced:
 *   1. A value can have either ONE mutable borrow OR multiple immutable borrows.
 *   2. A value cannot be used after it has been moved.
 *   3. Mutable borrows are exclusive (no aliasing).
 *   4. Borrows do not outlive the borrowed value.
 *   5. Drop is called exactly once per owned value (or zero times if moved).
 *
 * Analysis approach:
 *   - Liveness analysis: compute live ranges for every SSA value.
 *   - Borrow graph: track borrow→source relationships.
 *   - Walk each block checking borrow conflicts against live ranges.
 *   - Report errors with source locations.
 */

#ifndef MIR_BORROW_H
#define MIR_BORROW_H

#include "mir.h"
#include <stdbool.h>

/* Maximum number of tracked borrows per function */
#define MIR_MAX_BORROWS  512
#define MIR_MAX_ERRORS   64

/* Borrow kind */
typedef enum {
    MIR_BK_SHARED,      /* & — immutable borrow */
    MIR_BK_MUTABLE,     /* &mut — exclusive borrow */
} MirBorrowKind;

/* A tracked borrow */
typedef struct {
    MirValueId      borrow_id;      /* SSA value of the borrow */
    MirValueId      source_id;      /* SSA value being borrowed */
    MirBorrowKind   kind;
    MirBlock*       def_block;      /* Block where borrow was created */
    int             src_line;       /* Source location */
    int             src_col;
    bool            active;         /* Still live? */
} MirBorrow;

/* Borrow check error */
typedef struct {
    enum {
        MIR_BERR_USE_AFTER_MOVE,
        MIR_BERR_DOUBLE_MOVE,
        MIR_BERR_BORROW_CONFLICT,       /* mut + shared overlap */
        MIR_BERR_MUT_ALIAS,             /* two &mut to same value */
        MIR_BERR_BORROW_OUTLIVES,       /* borrow outlives source */
        MIR_BERR_MISSING_DROP,          /* owned value not dropped */
        MIR_BERR_DOUBLE_DROP,           /* value dropped twice */
    } kind;
    const char*     message;
    int             line;
    int             col;
    MirValueId      value;
    MirValueId      conflicting;     /* for borrow conflicts */
} MirBorrowError;

/* Liveness information for a value */
typedef struct {
    MirValueId      value;
    MirBlock*       def_block;
    MirBlock*       last_use_block;
    int             def_inst_idx;
    int             last_use_idx;
    bool            is_moved;
    bool            is_dropped;
} MirLiveness;

/* Borrow checker context */
typedef struct {
    MirModule*          module;

    /* Active borrows */
    MirBorrow           borrows[MIR_MAX_BORROWS];
    int                 borrow_count;

    /* Liveness data (per function, reallocated) */
    MirLiveness*        liveness;
    int                 liveness_count;
    int                 liveness_capacity;

    /* Error accumulator */
    MirBorrowError      errors[MIR_MAX_ERRORS];
    int                 error_count;

    /* Moved values set (bitset keyed by MirValueId) */
    bool*               moved;
    int                 moved_capacity;
} MirBorrowChecker;

/* Initialize / destroy */
void mir_borrow_init(MirBorrowChecker* bc, MirModule* module);
void mir_borrow_destroy(MirBorrowChecker* bc);

/* Check a single function. Returns number of errors found. */
int mir_borrow_check_function(MirBorrowChecker* bc, MirFunction* func);

/* Check all functions in a module. Returns total error count. */
int mir_borrow_check_module(MirBorrowChecker* bc, MirModule* module);

/* Print accumulated errors to stderr */
void mir_borrow_print_errors(MirBorrowChecker* bc, FILE* out);

/* Reset state (between functions) */
void mir_borrow_reset(MirBorrowChecker* bc);

#endif /* MIR_BORROW_H */
