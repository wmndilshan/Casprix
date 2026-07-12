/*
 * Casprix VM — Register-Based MIR Interpreter (CVM)
 *
 * Architecture overview
 * ─────────────────────
 * CVM consumes Casprix MIR directly — the MIR IS the bytecode.
 * No separate encoding step is needed; the in-memory MirModule is
 * walked instruction-by-instruction using a computed-goto dispatch
 * loop (GCC/Clang `__label__` extension; MSVC falls back to switch).
 *
 * Register file
 * ─────────────
 * Each MirValueId maps to a 64-bit slot in a flat array (CvmReg*).
 * Floating-point values are stored in the same slot as a bit-cast
 * uint64_t so that the dispatch loop needs no type tag overhead.
 * Ownership rules from ownership_check.c are NOT re-evaluated at
 * runtime (they were proven at compile time); the VM trusts MIR.
 *
 * Tiering
 * ───────
 * Every MirFunction has an associated CvmProfile entry.  On each
 * call the `call_count` is incremented; when it exceeds
 * CVM_JIT_THRESHOLD the JIT bridge (jit_bridge.h) is invoked to
 * compile the function to native code, store the code pointer in
 * the profile, and execute native code on subsequent calls.
 *
 * GC integration
 * ──────────────
 * The CvmFrame struct holds a pointer to the VM register file for
 * the active frame.  cvm_gc_scan_frames() iterates the live frame
 * stack and calls nuwan_gc_add_root() for every pointer-typed slot,
 * allowing the mark-sweep GC to trace VM-managed heap objects.
 */

#ifndef CVM_ENGINE_H
#define CVM_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* Forward declarations from MIR */
typedef struct MirModule    MirModule;
typedef struct MirFunction  MirFunction;
typedef struct MirInst      MirInst;
typedef uint32_t            MirValueId;

/* Forward declaration from JIT bridge */
typedef struct CvmJitBridge CvmJitBridge;

/* ─────────────────────────────────────────────────────────────
 * Configuration
 * ───────────────────────────────────────────────────────────── */

/* Number of call-count ticks before a function is promoted to JIT. */
#ifndef CVM_JIT_THRESHOLD
#  define CVM_JIT_THRESHOLD  1000
#endif

/* Maximum register-file slots per frame (hard upper bound for safety). */
#ifndef CVM_MAX_REGS
#  define CVM_MAX_REGS  4096
#endif

/* Maximum call-stack depth before a stack-overflow trap. */
#ifndef CVM_MAX_CALL_DEPTH
#  define CVM_MAX_CALL_DEPTH  512
#endif

/* ─────────────────────────────────────────────────────────────
 * Register type
 * ───────────────────────────────────────────────────────────── */

/* A single 64-bit slot that holds an integer, pointer, or the bits
 * of a double (via cvm_f64_to_bits / cvm_bits_to_f64 helpers). */
typedef uint64_t CvmReg;

static inline CvmReg  cvm_f64_to_bits(double d)  { CvmReg r; __builtin_memcpy(&r, &d, 8); return r; }
static inline double  cvm_bits_to_f64(CvmReg r)  { double d; __builtin_memcpy(&d, &r, 8); return d; }

/* ─────────────────────────────────────────────────────────────
 * Per-function JIT profile
 * ───────────────────────────────────────────────────────────── */

typedef enum {
    CVM_TIER_INTERPRET = 0,   /* being interpreted */
    CVM_TIER_JIT_PENDING,     /* compilation triggered, not yet complete */
    CVM_TIER_NATIVE,          /* native code available via native_fn */
} CvmTier;

/* Native function signature: args are passed as a flat CvmReg array;
 * the return value is placed in regs[0]. */
typedef void (*CvmNativeFn)(CvmReg* regs, int n_args);

typedef struct {
    const char*     func_name;
    int32_t         call_count;
    int32_t         loop_iterations;
    CvmTier         tier;
    CvmNativeFn     native_fn;   /* non-NULL when TIER_NATIVE */
    void*           native_mem;  /* executable memory block (for free) */
    size_t          native_size;
} CvmProfile;

/* ─────────────────────────────────────────────────────────────
 * Call frame
 * ───────────────────────────────────────────────────────────── */

typedef struct CvmFrame {
    MirFunction*    func;
    CvmReg*         regs;       /* register file, size = func->next_value_id */
    int             reg_count;

    /* MIR_ALLOCA allocations owned by this frame (freed on return). */
    void**          allocas;
    int             alloca_count;
    int             alloca_cap;

    /* Return value slot in the *caller's* register file */
    CvmReg*         caller_regs;
    MirValueId      result_id;  /* where the caller expects the return value */

    /* GC linkage: next older frame on the chain */
    struct CvmFrame* prev;
} CvmFrame;

/* ─────────────────────────────────────────────────────────────
 * VM state
 * ───────────────────────────────────────────────────────────── */

typedef struct {
    MirModule*      module;

    /* Per-function hotness profiles */
    CvmProfile*     profiles;
    int             profile_count;
    int             profile_cap;

    /* Live call-frame stack (for GC scanning) */
    CvmFrame*       frame_stack;   /* top (newest) frame */
    int             frame_depth;
    int             frame_depth_peak;

    /* Optional JIT bridge (NULL → no JIT) */
    CvmJitBridge*   jit;

    /* Diagnostics */
    FILE*           diag;          /* NULL → stderr */
    int             trap_code;     /* non-zero on fatal trap */

    /* Statistics */
    uint64_t        inst_executed;
    uint64_t        jit_calls;
} CvmState;

/* ─────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────── */

/* Create a new VM state bound to `module`.
 * `jit` may be NULL to disable tiering. */
CvmState* cvm_state_create(MirModule* module, CvmJitBridge* jit);
void      cvm_state_destroy(CvmState* vm);

/* Execute the function named `entry` with `n_args` integer arguments.
 * Returns the integer result (or 0 for void). */
int64_t   cvm_run(CvmState* vm, const char* entry, int64_t* args, int n_args);

/* Execute a specific MirFunction directly. */
CvmReg    cvm_call_function(CvmState* vm, MirFunction* func,
                            CvmReg* args, int n_args);

/* Look up (or create) the profile for `func`. */
CvmProfile* cvm_get_profile(CvmState* vm, MirFunction* func);

/* GC integration: register all live VM frame pointer slots as roots. */
void cvm_gc_scan_frames(CvmState* vm);

/* Diagnostics */
void cvm_print_stats(CvmState* vm, FILE* out);

#endif /* CVM_ENGINE_H */
