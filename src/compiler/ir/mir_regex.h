/*
 * Casprix Compiler — Regex → MIR Compiler
 *
 * Compiles a regular expression pattern into a Casprix MIR function that
 * matches the given pattern against a byte string in strictly O(n) time.
 *
 *   Pipeline:
 *       pattern string
 *             │ parse  (recursive-descent)
 *             ▼
 *       Regex AST (literal / class / concat / alt / star / plus / opt)
 *             │ Thompson's construction
 *             ▼
 *       NFA (epsilon-NFA with 256-bit byte classes)
 *             │ subset construction
 *             ▼
 *       DFA (fully deterministic, ≤ 2^|NFA| states in theory; in practice
 *            tiny for well-behaved patterns)
 *             │ MIR codegen
 *             ▼
 *       MirFunction   i32 @regex_<NAME>(ptr<u8> input, i64 len)
 *
 * The generated function returns 1 on a successful *full* match of the
 * input and 0 otherwise. One basic block is emitted per DFA state plus
 * an `end_<S>` return block per state and a shared `reject` block:
 *
 *       entry:
 *           pos_slot = ALLOCA i64
 *           STORE pos_slot, 0
 *           BR state_0
 *
 *       state_S:
 *           pos      = LOAD  pos_slot
 *           at_end   = CMP_GE pos, len
 *           CONDBR at_end -> end_S, body_S
 *       body_S:
 *           byte_ptr = GEP  input, pos
 *           ch       = LOAD byte_ptr                ; u8
 *           pos1     = ADD  pos, 1
 *           STORE pos_slot, pos1
 *           SWITCH ch -> case b0 -> state_T0,
 *                        case b1 -> state_T1, ...
 *                        default -> reject
 *       end_S:                         ; fall-off-input return
 *           RET (1 if S ∈ accepting else 0)
 *
 *       reject:
 *           RET 0
 *
 * Because every transition is explicit and the body blocks have no loops
 * encoded as back-edges into themselves (the `state_S` block is entered
 * via the SWITCH of *another* state — which mem2reg lifts `pos_slot`
 * through — the resulting CFG is reducible, trivially dominator-friendly,
 * and feeds cleanly into `mir_optimize_function(...)`:
 *
 *     - MIR_CONST_INT folding drops `pos1 = pos + 1` constant edges.
 *     - MIR_SWITCH with a single reachable target collapses to MIR_BR.
 *     - mem2reg promotes `pos_slot` into an SSA `phi` over state blocks.
 *     - DCE strips the `end_S` blocks of any state from which all bytes
 *       (i.e. empty accepting states in a dead branch) are unreachable.
 *     - simplify_cfg merges chains of unique successors.
 *
 * The backend (`asmgen.c`) then lowers MIR_SWITCH to a native jump-table
 * (x86-64) or a binary-search tree (ARM64) without any regex-specific
 * code: by compiling through MIR we inherit *every* architecture the
 * compiler already supports.
 *
 * Supported syntax (byte-oriented):
 *
 *     literal bytes        a Z 7 space etc.
 *     dot                  .          (any byte except \0; change via flag)
 *     groups               ( ... )
 *     alternation          a|b
 *     quantifiers          a*  a+  a?
 *     character classes    [abc]  [^abc]  [a-z]  [0-9A-Fa-f]
 *     escapes              \n \r \t \0 \\ \. \[ \] \( \) \| \* \+ \? \-
 *     shorthand            \d \D  \w \W  \s \S
 *
 * NOT supported (intentional — would require NFA-style backtracking or
 * tagged-DFA extensions): bounded `{n,m}`, anchors `^`/`$`, back-refs,
 * look-around. The compiler reports a clean diagnostic for these.
 */
#ifndef MIR_REGEX_H
#define MIR_REGEX_H

#include "mir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MirRegexResult {
    bool          ok;               /* whole pipeline succeeded */
    const char*   error;            /* arena-owned message, NULL if ok    */
    int           error_pos;        /* byte offset into pattern, or -1    */

    MirFunction*  func;             /* emitted match function, or NULL    */
    int           dfa_states;       /* # DFA states (informational)       */
    int           nfa_states;       /* # NFA states (informational)       */
    int           accepting_states; /* # accepting DFA states             */
} MirRegexResult;

/*
 * Compile `pattern` into a matcher function named `func_name` added to
 * `module`. On success, `result.func` is the newly-installed MirFunction
 * (also reachable via `mir_module_find_function(module, func_name)`).
 *
 * On failure, `result.ok == false` and `result.error` points at an
 * arena-allocated diagnostic (safe for the module's lifetime).
 */
MirRegexResult mir_regex_compile(MirModule*  module,
                                 const char* func_name,
                                 const char* pattern);

/*
 * Reference DFA matcher, exposed for testing: runs the same DFA produced
 * by the compiler against a byte buffer. Returns true iff the DFA
 * fully matches `len` bytes of `input`. Useful for cross-checking the
 * generated MIR against the canonical DFA behaviour without having to
 * run the full backend pipeline.
 */
bool mir_regex_match_reference(const char* pattern,
                               const uint8_t* input,
                               size_t len,
                               const char** error_out);

#ifdef __cplusplus
}
#endif

#endif /* MIR_REGEX_H */
