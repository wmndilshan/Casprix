# Changelog

All notable changes to Casprix are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Versioning follows [Semantic Versioning](https://semver.org/).

---

## [Unreleased]

### Added
- Added CMake developer, release, and CI presets.
- Added compiler version reporting, install/package metadata, and a version
  regression test.
- Added security, support, community conduct, and formatting policies.

### Fixed
- Standalone AI tests now link the AVX2 kernel assembly when AVX2 is enabled.
- CI now performs clean build, test, install, and installed-binary smoke checks
  on Linux and Windows.
- Release-mode test binaries now explicitly retain C assertions, preventing
  `NDEBUG` from silently removing runtime and compiler checks in CI.
- Skia GUI: buttons no longer resize on hover/click. `widget_button` and the
  base box widget set `flex_shrink = 0.0f` so state changes mark paint dirty
  without triggering a layout resize.

### Removed
- Removed task-specific TinyStories tooling, dead archived networking code,
  obsolete AI compatibility wrappers, and placeholder standard-library modules.
- Deleted stale/aspirational documentation and root-level scratch clutter:
  four large design blueprints moved under `docs/design/` with a
  "not implemented" banner; session-debugging notes
  (`runtime/net2/cx_worker_patch.md`, `runtime/skia/BUTTON_RESIZE_FIX.md`)
  removed; `examples/` reduced to a curated set that all compile with the
  current compiler.

### Changed
- `VERSION` is now the canonical machine-readable toolchain version.
- Downloaded Android Skia sources live under the ignored `build-tools/` tree.
- Reworked repository and release documentation around the current pre-stable
  maturity level.
- `docs/CASPRIX_SYNTAX_EBNF.md` and
  `docs/CASPRIX_SYNTAX_CONFORMANCE_MATRIX.md` corrected: `async`/`await` are
  implemented, not planned.
- `README.md` rewritten to describe only current, verified behavior.

---

## [1.0.0] — 2026-03-26

### Added
- Complete Casprix language compiler (lexer → parser → sema → MIR → x86-64)
- Mid-level Intermediate Representation (MIR): typed SSA with CFG, phi nodes, dominator tree
- Optimization passes: mem2reg, DCE, constant propagation, copy propagation, inlining, loop unrolling
- x86-64 NASM code generation with AVX2 auto-vectorization
- Linear-scan register allocator (85–90% register utilization)
- Hybrid memory runtime: ARC, ownership model, memory regions (arena), cycle GC
- Async/await runtime: coroutines, futures, task scheduler, channels
- Skia GUI framework with GDI fallback (works with MinGW, no MSVC required)
- Neural network framework: layers, optimizers, autograd, JIT kernels
- LLM training infrastructure: transformer, tokenizer, checkpoint, AVX2 backward pass
- Package manager (casprix-pkg): registry, resolver, semantic versioning, cache
- Android build support: NDK, APK builder, EGL renderer
- Full rebranding: Nuwan/ND → Casprix/CPX
- Professional CMake build system with modern INTERFACE targets
- GNU Makefile alternative with parallel build support

### Changed
- Language: PascalCase → lowercase keywords; `<-` assignment → `=` / `:=`
- Extension: `.nd` → `.cpx` (`.nd` still accepted for backward compatibility)
- Binary: `nuwanc` → `casprix`
- Compiler structure: `src/core/` → `src/support/`, `semantic/` → `sema/`, `backend/` → `codegen/`
- `pipeline.c`: replaced duplicate cleanup chains with `CompileCtx`/`goto done` pattern
- CMakeLists.txt: INTERFACE library targets for compile options and include paths

### Fixed
- Stack-frame prologue missing at top-level scope in `asmgen.c`
- VTable method index counting (was counting parents only, now counts current class)
- Float parameter passing via XMM registers in function/method calls
- 16-byte call-site stack alignment (Windows x64 ABI)
- Module merged-statement array crash in `free_stmt` (skip per-stmt free)
- `frame_loop.c` inverted window poll check
