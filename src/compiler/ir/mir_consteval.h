/*
 * Casprix Compiler — MIR Constant Evaluation Engine
 *
 * Evaluates constexpr functions at compile time by interpreting
 * MIR instructions.
 *
 * Restrictions on constexpr functions:
 *   - No heap allocation (no obj_alloc, no ARC)
 *   - No I/O or extern calls
 *   - Termination guaranteed by step limit
 *   - Only deterministic operations
 *
 * Usage:
 *   MirConstEval ce;
 *   mir_consteval_init(&ce, module);
 *   MirConstValue result;
 *   if (mir_consteval_call(&ce, func, args, n_args, &result)) { ... }
 *   mir_consteval_destroy(&ce);
 */

#ifndef MIR_CONSTEVAL_H
#define MIR_CONSTEVAL_H

#include "mir.h"

/* Maximum steps before termination (prevents infinite loops) */
#define MIR_CONSTEVAL_MAX_STEPS  100000

/* Constant value kinds */
typedef enum {
    MIR_CVAL_INT,
    MIR_CVAL_FLOAT,
    MIR_CVAL_BOOL,
    MIR_CVAL_STRING,
    MIR_CVAL_NULL,
    MIR_CVAL_AGGREGATE,   /* struct / array */
    MIR_CVAL_VOID,
    MIR_CVAL_ERROR,       /* evaluation failed */
} MirConstValueKind;

/* Result of constant evaluation */
typedef struct MirConstValue {
    MirConstValueKind   kind;
    MirType*            type;
    union {
        int64_t         i64;
        double          f64;
        bool            b;
        const char*     str;
        struct {
            struct MirConstValue* fields;
            int n_fields;
        }               agg;
    } as;
} MirConstValue;

/* Const-eval interpreter state */
typedef struct {
    MirModule*          module;

    /* Value slots (indexed by MirValueId) */
    MirConstValue*      slots;
    int                 slot_count;
    int                 slot_capacity;

    /* Step counter for termination */
    int                 steps;
    int                 max_steps;

    /* Error reporting */
    const char*         error_msg;
    int                 error_line;

    /* Call depth tracking (prevents deep recursion) */
    int                 call_depth;
    int                 max_call_depth;
} MirConstEval;

/* Initialize / destroy */
void mir_consteval_init(MirConstEval* ce, MirModule* module);
void mir_consteval_destroy(MirConstEval* ce);

/* Check if a function is eligible for constexpr evaluation */
bool mir_consteval_eligible(MirFunction* func);

/* Evaluate a constexpr function call. Returns true on success. */
bool mir_consteval_call(MirConstEval* ce, MirFunction* func,
                        MirConstValue* args, int n_args,
                        MirConstValue* out_result);

/* Evaluate a single instruction (internal, exposed for testing) */
bool mir_consteval_step(MirConstEval* ce, MirInst* inst);

/* Utility: create constant values */
MirConstValue mir_cval_int(int64_t v);
MirConstValue mir_cval_float(double v);
MirConstValue mir_cval_bool(bool v);
MirConstValue mir_cval_void(void);

#endif /* MIR_CONSTEVAL_H */
