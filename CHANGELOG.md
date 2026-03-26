# Changelog

All notable changes to Casprix are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Versioning follows [Semantic Versioning](https://semver.org/).

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
