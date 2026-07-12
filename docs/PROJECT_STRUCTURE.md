# Casprix Project Structure

This document describes the layout of the Casprix compiler, runtime, toolchain,
and standard library.  Each top-level directory has a single, well-defined
responsibility; nothing lives in two places.

---

## Quick reference

```
casprix/
├── src/              Compiler (frontend → IR → codegen)
├── runtime/          C runtime library (memory, I/O, AI, vm/, …)
├── lib/              Standard library written in Casprix (.cpx)
├── include/          Public C headers for embedding / FFI
├── pkg/              Package manager (cpkg) source
├── stdlib/           Bootstrapped stdlib package index
├── tests/            Test suite (compiler + runtime)
├── tools/            Standalone developer tools
├── scripts/          CI / developer convenience scripts
├── docs/             Documentation (FEATURES, PROJECT_STRUCTURE, STDLIB_STRINGS_AND_REGEX, …)
├── examples/         Runnable example programs
├── ide/              IDE integrations (VS Code extension, grammar)
├── third_party/      Vendored external libraries (Skia, Python shim)
└── build/            CMake / Make output (git-ignored)
```

---

## `src/` — Compiler

```
src/
├── main.c                    Compiler entry point
├── driver/                   CLI argument parsing + pipeline orchestration
│   ├── cli.c / cli.h
│   └── pipeline.c / pipeline.h
│
├── compiler/
│   ├── frontend/             Lexical + syntax + early AST
│   │   ├── lexer.c / lexer.h
│   │   ├── parser.c / parser.h
│   │   └── ast.c / ast.h
│   │
│   ├── sema/                 Type-checking, symbol resolution, ownership
│   │   ├── semantic.c / semantic.h
│   │   ├── symtable.c / symtable.h
│   │   ├── escape_analysis.c / escape_analysis.h
│   │   ├── ownership_check.c / ownership_check.h
│   │   └── drop_planner.c / drop_planner.h
│   │
│   ├── middle/               Mid-level IR transformations
│   │   ├── async.c / async.h          Async/await desugaring
│   │   ├── closure.c / closure.h      Closure capture & lifting
│   │   ├── monomorphize.c             Generic instantiation
│   │   └── trait.c / trait.h          Trait method dispatch
│   │
│   ├── ir/                   MIR (Mid-level IR)
│   │   ├── mir.c / mir.h              Core IR types & builders
│   │   ├── mir_builder.c              AST → MIR lowering
│   │   ├── mir_borrow.c               Borrow checker pass
│   │   ├── mir_consteval.c            Constant folding / eval
│   │   ├── mir_inline.c               Inliner
│   │   ├── mir_lower.c                MIR → codegen lowering
│   │   ├── mir_mem2reg.c              Memory-to-register promotion
│   │   ├── mir_opt.c                  General MIR optimisations
│   │   ├── mir_printer.c              Debug printer
│   │   ├── mir_backend.c              MIR → backend bridge
│   │   ├── mir_c_backend.c            C-emission backend (portable)
│   │   ├── mir_async.c                Async lowering on MIR
│   │   └── mir_regex.c                Regex → MIR pipeline
│   │
│   ├── opt/                  High-level optimisation passes
│   │   ├── inline.c / inline.h
│   │   ├── loop_opt.c / loop_opt.h
│   │   ├── peephole.c / peephole.h
│   │   └── simd.c / simd.h            Auto-vectorisation hints
│   │
│   ├── lowering/             Platform-specific lowering
│   │   └── async_lowering.c / async_lowering.h
│   │
│   └── codegen/              x86-64 assembly emission
│       ├── asmgen.c / asmgen.h        Low-level asm builder
│       ├── codegen.c / codegen.h      Top-level orchestration
│       ├── optimizer.c / optimizer.h  Post-codegen peephole
│       └── regalloc.c / regalloc.h    Register allocator
│
└── support/                  Shared compiler utilities
    ├── arena.c / arena.h              Arena allocator
    ├── debug.c / debug.h
    ├── diagnostic.c / diagnostic.h   Error & warning reporting
    ├── error.c / error.h
    └── log.c / log.h
```

---

## `runtime/` — C Runtime Library

All runtime code is plain C (C11) with optional NASM assembly for hot paths.
`runtime/CMakeLists.txt` can be included from the root build or used
standalone (`cmake -DCASPRIX_RUNTIME_STANDALONE=ON runtime/`).

```
runtime/
├── runtime.h / runtime.c          Top-level initialisation & teardown
├── object.h / object.c            ArcHeader, vtable, RTTI, destructors
├── vtable_opt.h / vtable_opt.c    Inline vtable cache optimisation
│
├── memory/                        Memory subsystem
│   ├── arc.h / arc.c              Atomic reference counting
│   ├── ownership.h / ownership.c  Move/borrow semantics enforcement
│   ├── cycle_gc.h / cycle_gc.c    Cycle-breaking garbage collector
│   └── memory.h / memory.c        Unified allocator facade
│
├── async/                         Async/await runtime (coroutines)
├── concurrent/                    Lock-free data structures
├── sync/                          Mutexes, condition variables, channels
├── thread/                        OS thread pool & task scheduler
│
├── io/                            Buffered I/O primitives
├── file/                          File-system helpers
├── net/                           TCP/UDP sockets + DNS
│
├── math/                          SIMD-accelerated math kernels
├── binding/                       FFI & C interop helpers
│
├── gui/                           Platform GUI integration
├── skia/                          2D graphics (GDI fallback + Skia C++)
│   ├── skia_c.h                   Pure-C surface API (~80 functions)
│   ├── skia_c_gdi.c               GDI/SDF software renderer
│   ├── skia_window_win32.c        Win32 DIB window
│   ├── scene_graph.h/c            SGNode dirty-tracking scene tree
│   ├── layout.h/c                 Flexbox row / column / stack
│   ├── events.h/c                 Capture/bubble event dispatch
│   ├── widgets.h/c                Button, TextInput, Checkbox, Slider, …
│   ├── style.h/c                  Shadows, gradients, themed palette
│   ├── text.h/c                   Font cache + multi-line layout
│   ├── animation.h/c              Property animation, 16 easing curves
│   └── frame_loop.h/c             SGApp 60 fps render/event loop
│
├── ai/                            ML / AI accelerated runtime
│   ├── llm/                       Transformer & LLM infrastructure
│   │   ├── tensor.h/c             Dense tensor (CPU + AVX2)
│   │   ├── autograd.h/c           Tape-based reverse-mode AD
│   │   ├── ops.h/c                Element-wise & reduce ops
│   │   ├── ops_avx2.asm           Hand-written AVX2 kernels
│   │   ├── transformer.h/c        Transformer block (attn + MLP)
│   │   ├── backward.h/c           All backward kernels
│   │   ├── jit.h/c                x86-64 AVX2 JIT compiler
│   │   ├── tensor_ir.h/c          SSA-style tensor IR
│   │   ├── graph_opt.h/c          DCE, const-fold, op-fusion
│   │   ├── tokenizer.h/c          BPE tokeniser
│   │   ├── training.h/c           Training loop & gradient clipping
│   │   └── checkpoint.h/c         Model serialisation
│   │
│   ├── nn/                        High-level neural-network API
│   │   ├── nn.h                   Umbrella header
│   │   ├── layers/                Dense, Conv2d, BatchNorm, …
│   │   ├── activations/           ReLU, GELU, Softmax, …
│   │   ├── loss/                  CrossEntropy, MSE, …
│   │   ├── optimizers/            SGD, Adam, AdamW
│   │   ├── data/                  Dataset, DataLoader, Sampler
│   │   ├── models/                Pre-built model skeletons
│   │   └── metrics/               Accuracy, F1, …
│   │
│   └── ml/                        HPC / acceleration primitives
│       ├── cpx_tensor_engine.h/c  High-level tensor engine
│       ├── cpx_scheduler.h/c      Work-stealing task scheduler
│       ├── cpx_mem_arena.h/c      Arena allocator (zero-GC training)
│       ├── cpx_kvcache.h/c        KV-cache for inference
│       ├── cpx_quant.h/c          INT4/INT8 quantisation
│       ├── cpx_hpc_kernel.h/c     SIMD dispatch + micro-kernels
│       └── ml_runtime.h/c         Public ML runtime facade
│
└── android/                       Android NDK platform support
```

---

## `lib/` — Casprix Standard Library

All files are Casprix source (`.cpx`).  These are compiled and shipped with
the toolchain; end-user projects import them with `import "..."`.

```
lib/
├── stdlib/            Core language library
│   ├── Collections.cpx          List, Map, Set, Queue, Stack
│   ├── Collections_Simple.cpx   Lightweight alternatives
│   ├── Generics.cpx             Generic container helpers
│   ├── Interfaces.cpx           Common interface definitions
│   ├── Lambdas.cpx              Higher-order function utilities
│   ├── Exceptions.cpx           Exception hierarchy & handlers
│   ├── Thread.cpx               Thread & synchronisation primitives
│   ├── AsyncThread.cpx          Async / await abstractions
│   ├── Network.cpx              HTTP / socket client wrappers
│   ├── AsyncNetwork.cpx         Non-blocking network helpers
│   ├── GUI.cpx                  Declarative widget toolkit (Skia)
│   └── ModernGUI.cpx            Material-style component library
│
├── core/              Low-level utilities
├── math/              Numerical & linear-algebra helpers
├── io/                Console, file, and stream I/O
├── file/              File-system helpers
├── net/               Networking (HTTP, WebSocket, …)
├── thread/            Threading utilities
├── gui/               GUI component extensions
├── skia/              Skia / UI bindings (skia.cpx, ui.cpx)
├── llm/               LLM utilities (Tokenizer.cpx, …)
├── ml/                ML helpers (KMeans.cpx, LinearRegression.cpx)
├── nn/                Neural-network DSL (nn.cpx)
└── win32/             Windows-specific helpers
```

---

## `include/` — Public C Headers

Headers here form the stable ABI that embedders and FFI consumers use.
They must never include internal compiler or runtime implementation details.

```
include/
└── casprix/
    ├── common.h         Shared macros, types, ABI attributes
    ├── lang_abi.h       Calling-convention & name-mangling contract
    ├── stdlib.h         Standard library C entry-points
    ├── gc.h             GC / ARC public API
    ├── collections.h    Collections C API
    ├── concurrent.h     Concurrency primitives C API
    ├── async.h          Async runtime C API
    ├── thread.h         Thread pool C API
    ├── string_ops.h     String helper C API
    ├── file_io.h        File I/O C API
    ├── network.h        Networking C API
    ├── http.h           HTTP client/server C API
    └── gui.h            GUI / Skia C API
```

---

## `pkg/` — Package Manager (cpkg)

```
pkg/
├── CMakeLists.txt        Standalone or sub-project build
│
├── core/                 Package manager library
│   ├── manifest.h/c      casper.json parsing & generation
│   ├── registry.h/c      Remote registry client
│   ├── resolver.h/c      Semver dependency resolver (SAT)
│   ├── cache.h/c         Local package cache
│   ├── installer.h/c     Download, verify, extract packages
│   ├── publisher.h/c     Pack & publish to registry
│   └── semver.h/c        Semantic version comparisons
│
└── cli/                  Command-line interface
    ├── main.c            Entry point → calls cpkg_run()
    ├── cli.h             Public interface declaration
    └── cli_complete.c    Command dispatch & help text
```

---

## `stdlib/` — Bootstrapped Package Index

Contains the `casper.json` manifest for the Casprix standard library package
and helper `.cpx` modules used during bootstrap before the full stdlib is
available.

---

## `tests/` — Test Suite

```
tests/
├── CMakeLists.txt           Registers all tests with CTest
├── runner.c / runner.sh     Compiler integration harness (invokes `casprix` on .cpx tests)
├── test_lang_abi.c          ABI compatibility smoke tests
├── test_vm_jit.c            CVM interpreter + JIT bridge (links MIR + runtime/vm)
│
├── compiler/
│   ├── test_*.cpx           Compile-and-run feature tests
│   ├── test_mir_regex.c     MIR regex pipeline (standalone executable)
│   ├── test_simd_virt.c     SIMD legalization / backend smoke test
│   ├── test_mir_corpus_verify.c   Golden MIR corpus checks
│   └── dump_mir_regex.c     Debug helper for MIR regex
│
├── corpus/                  Large / focused .cpx fixtures (e.g. regex, float pipeline)
│
├── runtime/                 C-based unit & integration tests
│   ├── test_stdlib.c
│   ├── test_coroutine.c     (often Windows-only in CMake)
│   ├── test_scheduler.c
│   ├── test_networking.c
│   ├── test_*.c             ML/attention/GEMM/KV-cache experiments
│   ├── benchmark.c
│   ├── bench_simd.c
│   └── demo_http_server.c
│
└── android/                 APK tooling smoke tests (manifest, zip, …)
```

---

## `tools/` — Developer Tools

```
tools/
├── build-tools/          Python 3 shim + build helper scripts
├── apk_builder/          Android APK packaging tool (standalone C binary)
├── android/              NDK / SDK management scripts
└── training/             LLM fine-tuning helper scripts
```

---

## `scripts/` — CI & Developer Convenience

```
scripts/
├── build.sh / build.bat       Full project build (Debug + Release)
└── run_tests.sh / run_tests.bat   Run CTest suite
```

---

## `ide/` — IDE Integrations

```
ide/
└── vscode/            VS Code extension
    ├── package.json
    ├── syntaxes/      casprix.tmLanguage.json  (TextMate grammar)
    └── …
```

---

## `examples/` — Runnable Examples

```
examples/
├── basic/             Hello world, variables, functions
├── advanced/          Closures, generics, pattern matching
├── gui/               Skia UI widgets & animation
├── ml/                Tensor ops, autograd, training loop
├── network/           HTTP client/server
├── tinystories/       GPT-2 mini fine-tune on TinyStories
└── android/           Android demo app
```

---

## `third_party/` — Vendored Libraries

```
third_party/
├── skia/              Aseprite prebuilt Skia headers (MSVC)
├── skia-src/          Skia C++ source used by GDI backend
└── …
```

---

## Key conventions

| Convention | Detail |
|---|---|
| File extension | `.cpx` for Casprix source, `.h`/`.c` for C runtime/tools |
| Module imports | `import "lib/core/..."` — full path relative to project root |
| Fallback extension | `.nd` accepted by compiler for backward compatibility |
| Build output | All artefacts go to `build/`; never committed |
| Naming | `snake_case` for C symbols; `PascalCase` for Casprix types |
| ABI prefix | `__casprix_` for runtime symbols exported to generated code |
| Log macros | `CPX_LOG_*` / `CpxLog*` — never `printf` in runtime hot paths |
