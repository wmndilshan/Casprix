/*
 * Casprix Compiler - SIMD Virtualization Layer
 *
 * This header defines a target-neutral SIMD abstraction in the MIR.  The
 * compiler front-end (and the autovectorizer) emit generic vector
 * instructions (MIR_VEC_*) that describe an algorithm in terms of
 * *logical* SIMD lanes (e.g. "8 lanes of f32, add them pairwise").
 *
 * Before asm emission the SIMD virtualization pass picks a concrete
 * SimdTarget -- AVX-512, AVX2, SSE2, NEON, or pure scalar -- and
 * legalizes the MIR so every remaining VEC_* instruction's logical
 * width is *exactly* representable by one native vector register on the
 * selected target.  If no SIMD is available the pass scalarizes VEC_*
 * into ordinary MIR arithmetic loops so the AI/LLM stack still runs on
 * low-end/edge devices.
 *
 * Separation of concerns:
 *
 *   Front-end / autovectorizer   ->   MIR_VEC_* (logical width)
 *                                     |
 *                                     v
 *                           simd_legalize_module()
 *                                     |
 *                                     v
 *   Backend-ready MIR (native-width VEC_*  OR  scalar fallback MIR)
 *                                     |
 *                                     v
 *          x86-64 / AArch64 / VM / JIT   emits machine code
 *
 * The backend mapping tables live with each backend; this header only
 * declares the *capability* model, the legalization entry point, and
 * the small textual helpers that individual backends use to render
 * vector operands.
 */

#ifndef CASPRIX_SIMD_H
#define CASPRIX_SIMD_H

#include "compiler/ir/mir.h"
#include "compiler/ir/mir_backend.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* ────────────────────────────────────────────────────────────────────
 * SIMD capability model
 * ──────────────────────────────────────────────────────────────────── */

/* Capabilities are ordered by register width.  SIMD_CAP_NONE means
 * scalar-only (no SIMD registers available -- e.g. a 32-bit MCU, a
 * freshly built WASM32 target, or a tiny Colombo edge gateway). */
typedef enum {
    SIMD_CAP_NONE   = 0,    /* no SIMD; fall back to scalar loops */
    SIMD_CAP_SSE2   = 1,    /* 128-bit, x86-64 baseline           */
    SIMD_CAP_NEON   = 2,    /* 128-bit, AArch64/ARMv8 baseline    */
    SIMD_CAP_AVX2   = 3,    /* 256-bit, x86-64                    */
    SIMD_CAP_AVX512 = 4,    /* 512-bit, x86-64                    */
} SimdCapability;

/* A concrete target selection: capability + architecture.  The
 * architecture decides the instruction mnemonics (AVX intrinsics
 * vs NEON vs scalar); the capability decides the native vector
 * register width in bits. */
typedef struct {
    MirTargetArch  arch;
    SimdCapability capability;
    int            native_bits;   /* 0 if scalar, else 128/256/512 */
    const char*    name;          /* human readable target name    */
} SimdTarget;

/* Capability / target construction & queries. */
const char*    simd_capability_name(SimdCapability c);
SimdTarget     simd_target_make(MirTargetArch arch, SimdCapability cap);
SimdTarget     simd_target_default(MirTargetArch arch);      /* sensible default per arch */
SimdTarget     simd_target_detect_host(void);                /* probes the build machine  */

/* Lane size of a MirType in bytes.  Returns 0 for non-primitive lane types. */
int            simd_lane_bytes(const MirType* lane_type);

/* Native lane count for (target, lane_type) -- e.g. AVX2 + f32 => 8. */
int            simd_native_lanes(SimdTarget target, const MirType* lane_type);

/* True iff the target can fuse a*b+c into a single instruction (FMA). */
bool           simd_has_fma(SimdTarget target);

/* ────────────────────────────────────────────────────────────────────
 * Legalization pass
 *
 * Walks every function in `module` and rewrites MIR_VEC_* so that each
 * remaining VEC_* instruction's width equals the native lane count for
 * the chosen target.  A logical VEC_ADD of 32 lanes on AVX2 becomes
 * 4× VEC_ADDs of 8 lanes; on a scalar target it becomes 32 scalar
 * MIR_ADDs chained by ordinary SSA values.
 *
 * Returns the number of rewrites performed.  Safe to run multiple
 * times -- after convergence (return value == 0) the module is ready
 * for the backend.
 * ──────────────────────────────────────────────────────────────────── */
typedef struct {
    int  vec_ops_seen;            /* VEC_* instructions visited         */
    int  vec_ops_split;           /* widened/split into groups          */
    int  vec_ops_scalarized;      /* lowered to scalar MIR              */
    int  fma_fused;               /* mul+add pairs folded to VEC_FMA    */
    int  fma_expanded;            /* VEC_FMA lowered to mul+add (non-FMA target) */
} SimdLegalizeStats;

int            simd_legalize_function(MirFunction* func, SimdTarget target,
                                       SimdLegalizeStats* out_stats);
int            simd_legalize_module(MirModule* module, SimdTarget target,
                                     SimdLegalizeStats* out_stats);

/* ────────────────────────────────────────────────────────────────────
 * Emission helpers shared by every SIMD-aware backend
 *
 * These functions render a single MIR_VEC_* instruction as textual
 * assembly for the given target and write it to `out`.  They are used
 * by the x86-64 and AArch64 backends; the scalar backend never reaches
 * here because legalization removes all VEC_* ops first.
 *
 * All helpers return true on success and false if the op/lane-type
 * combination is unsupported on that target (the caller should then
 * either scalarize the op or abort compilation).
 * ──────────────────────────────────────────────────────────────────── */
bool           simd_emit_x86_64(FILE* out, const MirInst* inst,
                                 SimdTarget target);
bool           simd_emit_aarch64(FILE* out, const MirInst* inst,
                                  SimdTarget target);
/* Scalar emission is used as a last-resort path by the generic backend
 * when legalization has been skipped.  Normally unreachable. */
bool           simd_emit_scalar_text(FILE* out, const MirInst* inst);

/* ────────────────────────────────────────────────────────────────────
 * Autovectorizer (existing AST-level entry points)
 *
 * These predate the virtualization layer and are kept for the existing
 * pipeline that promotes scalar `for` loops to SIMD; they now funnel
 * into the generic VEC_* machinery internally.
 * ──────────────────────────────────────────────────────────────────── */
#ifndef CASPRIX_AST_STMT_FWD
#define CASPRIX_AST_STMT_FWD
typedef struct Stmt Stmt; /* defined in compiler/frontend/ast.h */
#endif

typedef struct {
    bool avx2_available;
    bool avx512_available;
    bool neon_available;
    int  vector_width;
    int  loops_vectorized;
    int  scalar_fallbacks;
} VectorContext;

void init_vectorizer(VectorContext* ctx);
bool can_vectorize_loop(Stmt* loop);
bool vectorize_loop(Stmt* loop, VectorContext* ctx);

#endif /* CASPRIX_SIMD_H */
