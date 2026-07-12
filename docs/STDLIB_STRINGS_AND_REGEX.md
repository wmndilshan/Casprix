# Standard library, strings, `StringView`, and regex

This document ties together three related layers: **Casprix `.cpx` modules** under `lib/`, the **C runtime** used by generated code and embedders, and **compiler internals** for regex and linear string views.

---

## 1. Casprix standard modules (`lib/`)

Shipped Casprix sources are rooted at `lib/`. Typical entry points:

| Area | Path | Notes |
|------|------|--------|
| Core collections / generics | `lib/stdlib/Collections.cpx`, `Generics.cpx`, `Collections_Simple.cpx` | Lists, maps, patterns (evolving) |
| Interfaces & lambdas | `lib/stdlib/Interfaces.cpx`, `Lambdas.cpx` | Shared protocols and helpers |
| Concurrency | `lib/stdlib/Thread.cpx`, `AsyncThread.cpx` | Maps to runtime thread / async APIs |
| Networking | `lib/stdlib/Network.cpx`, `AsyncNetwork.cpx` | HTTP / socket-oriented wrappers |
| GUI (Skia) | `lib/stdlib/GUI.cpx`, `ModernGUI.cpx` | Optional; needs `ENABLE_SKIA_GUI` build |
| LLM | `lib/llm/` | Tokenizer and related `.cpx` |

Imports use quoted paths, e.g. `import "lib/stdlib/Collections.cpx"` (exact style depends on your project layout).

The **`stdlib/`** directory holds the bootstrap package index and small modules used before the full tree is available; see `docs/PROJECT_STRUCTURE.md`.

---

## 2. Runtime string API (C)

User-facing string operations for generated code and tests are implemented in C and declared in [`include/casprix/string_ops.h`](../include/casprix/string_ops.h). The implementation lives in [`runtime/stdlib/string_ops.c`](../runtime/stdlib/string_ops.c).

Symbols use the historical `nuwan_string_*` prefix. Capabilities include:

- Concatenation, substring, length
- Case conversion (hot paths may use SIMD-style batching where available)
- Search: `index_of`, `last_index_of`, `contains`, `starts_with`, `ends_with`
- Trim variants, `split`, `replace` / `replace_all`
- FNV-1a hash (`nuwan_string_hash`) and helpers used by the runtime

Unit coverage: [`tests/runtime/test_stdlib.c`](../tests/runtime/test_stdlib.c).

---

## 3. Linear `StringView` (compiler semantic layer)

The compiler models a **non-owning** view into `string` storage as a **linear** type: a `StringView` has move-only semantics and must not outlive its parent `string` (enforced in semantic analysis, escape analysis, and the drop planner).

Authoritative design notes: [`src/compiler/sema/linear_view.h`](../src/compiler/sema/linear_view.h).

Practical implications:

- Use-after-move of a `StringView` is rejected at compile time where tracked.
- Returning or storing a `StringView` beyond the parent’s scope is rejected when detected.
- Regression / exploratory samples: [`tests/corpus/stringview_linear.cpx`](../tests/corpus/stringview_linear.cpx).

---

## 4. Regex → MIR compiler (inside the toolchain)

**End-user regex syntax in `.cpx` is not the focus here**; the repository includes a **compiler subsystem** that compiles a regex *pattern* into a **MIR function** that scans a byte buffer in one left-to-right pass (DFA-shaped control flow).

- Implementation: [`src/compiler/ir/mir_regex.c`](../src/compiler/ir/mir_regex.c), API in [`src/compiler/ir/mir_regex.h`](../src/compiler/ir/mir_regex.h).
- Pipeline sketch: parse pattern → NFA (Thompson) → DFA (subset construction) → MIR basic blocks per state + shared reject block.
- Tests: [`tests/compiler/test_mir_regex.c`](../tests/compiler/test_mir_regex.c), corpus integration in [`tests/compiler/test_mir_corpus_verify.c`](../tests/compiler/test_mir_corpus_verify.c) (includes `tests/corpus/cfg_regex_dfa.cpx`).

This is primarily for **optimization / codegen experiments** and corpus verification; wiring every regex surface feature in the *language* front end is separate work.

---

## 5. Related documentation

- [FEATURES.md](FEATURES.md) — compiler capabilities and CLI usage
- [PROJECT_STRUCTURE.md](PROJECT_STRUCTURE.md) — directory map
- [CASPRIX_SYNTAX_EBNF.md](CASPRIX_SYNTAX_EBNF.md) — grammar reference
