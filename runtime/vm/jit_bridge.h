/*
 * Casprix JIT Bridge
 *
 * Manages the tier-up transition from CVM interpreted execution to
 * native machine code.
 *
 * Responsibilities
 * ────────────────
 * 1. Allocate executable memory (mmap on POSIX, VirtualAlloc on Win32).
 * 2. Call asmgen (src/compiler/codegen/asmgen.c) to produce x86-64
 *    assembly text, then assemble it to machine code in-memory using
 *    a portable mini-assembler or, when NASM is available, via a
 *    temp-file round-trip.
 * 3. Install the resulting code pointer into the CvmProfile.
 * 4. Provide a thread-safe trampoline that marshals the CVM register
 *    file into native calling convention (System V AMD64 / Microsoft
 *    x64) and back.
 *
 * Thread safety
 * ─────────────
 * Compilation is protected by a per-bridge mutex so that concurrent
 * interpreter threads that hit the JIT threshold simultaneously only
 * compile the function once.
 *
 * Memory lifecycle
 * ────────────────
 * Each compiled function owns its executable block.  cjb_destroy()
 * frees all blocks.  Individual blocks can be freed with
 * cjb_free_native().
 */

#ifndef JIT_BRIDGE_H
#define JIT_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Pull in the CVM types (CvmProfile, CvmState, CvmReg, CvmNativeFn) */
#include "cvm_engine.h"

/* Forward declarations for MIR types */
typedef struct MirFunction  MirFunction;
typedef struct MirModule    MirModule;

/* ─────────────────────────────────────────────────────────────
 * Executable memory helpers
 * ───────────────────────────────────────────────────────────── */

/* Allocate `size` bytes of memory that is simultaneously writable and
 * executable.  Returns NULL on failure. */
void* jit_alloc_exec(size_t size);

/* Free a block returned by jit_alloc_exec(). */
void  jit_free_exec(void* mem, size_t size);

/* ─────────────────────────────────────────────────────────────
 * JIT result
 * ───────────────────────────────────────────────────────────── */

typedef enum {
    JIT_OK = 0,
    JIT_ERR_NO_NASM,        /* NASM not found on PATH */
    JIT_ERR_COMPILE_FAILED, /* asmgen or NASM returned error */
    JIT_ERR_ALLOC_FAILED,   /* executable memory allocation failed */
    JIT_ERR_ALREADY_NATIVE, /* function is already compiled */
} JitResult;

/* ─────────────────────────────────────────────────────────────
 * Trampoline entry
 * ───────────────────────────────────────────────────────────── */

/* All JIT-compiled functions are entered through the trampoline, which
 * translates the CVM flat-register calling convention into the platform
 * ABI.  `regs[0..n_args-1]` are the arguments; the return value is
 * written back to `regs[0]` on exit. */
typedef void (*JitTrampoline)(CvmReg* regs, int n_args);

/* ─────────────────────────────────────────────────────────────
 * JIT bridge handle
 * ───────────────────────────────────────────────────────────── */

typedef struct CvmJitBridge {
    /* All executable code blocks allocated so far (for cleanup) */
    void**   exec_blocks;
    size_t*  exec_sizes;
    int      block_count;
    int      block_cap;

    /* Statistics */
    int      functions_compiled;
    uint64_t total_native_bytes;
} CvmJitBridge;

/* ─────────────────────────────────────────────────────────────
 * API
 * ───────────────────────────────────────────────────────────── */

/* Create and destroy a JIT bridge. */
CvmJitBridge* cjb_create(void);
void          cjb_destroy(CvmJitBridge* bridge);

/* Compile `func` from `module` to native code.
 * On success: profile->native_fn is set, profile->tier = CVM_TIER_NATIVE.
 * On failure: profile remains in INTERPRET tier; compilation is skipped. */
JitResult cjb_compile_function(CvmJitBridge* bridge,
                                MirModule*    module,
                                MirFunction*  func,
                                CvmProfile*   profile);

/* Free the native code block owned by `profile`.
 * Sets native_fn = NULL and tier = CVM_TIER_INTERPRET. */
void cjb_free_native(CvmJitBridge* bridge, CvmProfile* profile);

/* Execute a JIT-compiled function via the trampoline.
 * `regs[0..n_args-1]` are arguments; return value is placed in regs[0]. */
void cjb_call(CvmJitBridge* bridge, CvmProfile* profile,
              CvmReg* regs, int n_args);

/* Print compilation statistics to `out`. */
void cjb_print_stats(CvmJitBridge* bridge, FILE* out);

#endif /* JIT_BRIDGE_H */
