<div align="center">

```
 ██████╗ █████╗ ███████╗██████╗ ██████╗ ██╗██╗  ██╗
██╔════╝██╔══██╗██╔════╝██╔══██╗██╔══██╗██║╚██╗██╔╝
██║     ███████║███████╗██████╔╝██████╔╝██║ ╚███╔╝ 
██║     ██╔══██║╚════██║██╔═══╝ ██╔══██╗██║ ██╔██╗ 
╚██████╗██║  ██║███████║██║     ██║  ██║██║██╔╝ ██╗
 ╚═════╝╚═╝  ╚═╝╚══════╝╚═╝     ╚═╝  ╚═╝╚═╝╚═╝  ╚═╝
```

**Fast. Modern. Powerful. 👻**

[![Version](https://img.shields.io/badge/version-1.0.0-blue.svg?style=for-the-badge)](https://github.com/user/casprix)
[![License](https://img.shields.io/badge/license-MIT-green.svg?style=for-the-badge)](LICENSE)
[![Author](https://img.shields.io/badge/author-Nuwan-purple.svg?style=for-the-badge)](https://github.com/user)
[![Language](https://img.shields.io/badge/language-C%20%2B%20x86--64-orange.svg?style=for-the-badge)]()
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg?style=for-the-badge)]()

*A modern, high-performance compiled programming language with production-grade optimization capabilities.*

[Quick Start](#-quick-start) · [Language Guide](#-language-guide) · [Architecture](#-compiler-architecture) · [Benchmarks](#-performance-benchmarks) · [Package Manager](#-package-manager) · [Docs](#-documentation)

</div>

---

## 👻 What is Casprix?

Casprix is a **compiled, statically-typed systems programming language** designed for developers who need both expressiveness and raw performance. It compiles directly to x86-64 native code via a multi-stage optimizing compiler pipeline, achieving runtime speeds **5–10x faster** than unoptimized equivalents — with a compilation pipeline **7.5x faster** thanks to its arena allocator.

Casprix combines:
- The **safety model** of modern systems languages (ownership, borrow annotations, lifetime regions)
- The **expressiveness** of high-level languages (closures, generics, async/await, traits)
- The **performance** of hand-optimized C (SIMD vectorization, register allocation, loop unrolling)

---

## ✨ Language Features

![Language Features](diagrams/features.svg)

| Category | Features |
|----------|----------|
| **Performance** | Register allocation, SIMD/AVX2 vectorization, loop unrolling (4x), arena allocator |
| **Safety** | Ownership model, borrow analysis, escape analysis, lifetime tracking |
| **Language** | Closures, generics (monomorphized), async/await, traits, pattern matching |
| **Memory** | Hybrid model: GC + ownership + memory regions |
| **Tooling** | Built-in package manager, semantic versioning, dependency resolution |
| **Targets** | x86-64 (primary), ARM64 (planned), VM bytecode, JIT (roadmap) |

---

## 🏗️ Compiler Architecture

Casprix uses a **multi-stage optimizing compiler** centered around a typed IR. The pipeline is designed for correctness first, then aggressive optimization.

![Compiler Pipeline](diagrams/pipeline.svg)

The pipeline flows through five distinct stages:

**Frontend** — lexing, parsing, name resolution, type inference, monomorphization, and closure lowering. Produces a clean, typed AST ready for IR conversion.

**IR (typed SSA)** — the heart of the compiler. An explicit control flow graph with basic blocks, phi nodes, ownership annotations, lifetime regions, and stack/heap abstraction. Two analysis passes run here: the **Borrow Checker** (static aliasing and race prevention) and the **Const-Eval Engine** (compile-time execution with termination guarantees).

**Optimization Pipeline** — 8 ordered passes over the IR. Each pass unlocks the next: constant propagation enables copy propagation, which enables dead code elimination, and so on through inlining, escape analysis, strength reduction, control flow simplification, and loop optimization.

**Backend Abstraction Layer** — target-independent interface handling ABI, calling conventions, and register abstraction. Decouples the optimizer from the code generator and enables multiple backends.

**Backends** — AOT native (x86-64 NASM today, ARM64 planned), VM bytecode (beta), and tiered JIT (roadmap).

---

## 🧠 Memory Model

![Memory Model](diagrams/memory.svg)

Casprix uses a **hybrid memory model** — the compiler's escape analysis pass decides at IR time where each allocation lives:

- **Stack** — locals that don't escape their scope. Zero overhead, freed automatically.
- **Ownership heap** — values with a single owner that outlive their scope. Freed deterministically on drop.
- **Memory regions (arena)** — bulk lifetime allocations freed all at once. Responsible for the 7.5x compile speedup.
- **GC** — available for managed objects and cyclic graphs. Opt-in, not default.

All allocation decisions are static — no runtime type checks, no boxing overhead.

---

## ⚙️ VM & JIT Architecture

![VM and JIT](diagrams/vm-jit.svg)

**VM Bytecode** uses a register-based instruction format (not stack-based), reducing instruction count and dispatch overhead. The VM enforces memory safety and sandboxing at the bytecode level, with a typed FFI bridge for native interop.

**Tiered JIT** operates in four levels:
1. **Tier 0 (Baseline)** — fast emission, minimal optimization, gets code running immediately
2. **Profiling** — call counters and branch frequency tracking identify hot paths
3. **Tier 2 (Optimizing)** — speculative optimization with inline caches and type feedback
4. **Deopt / OSR** — guard failures trigger deoptimization; on-stack replacement allows mid-execution tier transitions

---

## 📊 Performance Benchmarks

![Benchmarks](diagrams/benchmarks.svg)

### Compilation Speed

| Project Size | Without Arena | With Arena | Speedup |
|:-------------|:-------------:|:----------:|:-------:|
| 1,000 LOC    | 600 ms        | 80 ms      | **7.5x** |
| 10,000 LOC   | 5.2 s         | 0.7 s      | **7.4x** |

### Runtime Performance

| Benchmark      | Unoptimized | Optimized | Speedup |
|:---------------|:-----------:|:---------:|:-------:|
| Array Sum      | 850 ms      | 95 ms     | **8.9x** |
| Fibonacci(40)  | 3,200 ms    | 480 ms    | **6.7x** |
| Matrix Mult    | 4,500 ms    | 520 ms    | **8.7x** |

*Optimizations: register allocation + SIMD vectorization + loop unrolling + constant folding + inlining.*

---

## 🚀 Quick Start

### Installation

```bash
# Clone repository
git clone https://github.com/user/casprix.git
cd casprix

# Build (Linux / macOS)
./build.sh

# Build (Windows)
.\build.ps1
```

### Hello, World!

```casprix
# hello.cpx
print("Hello, Casprix! 👻")
```

```bash
./run.sh hello.cpx
# → Hello, Casprix! 👻
```

### Compile with Optimizations

```bash
casprixc program.cpx -o output          # standard
casprixc program.cpx -o output -O2      # optimized
casprixc program.cpx -o output -O2 -mavx2  # + SIMD
```

---

## 📖 Language Guide

### Variables & Types

```casprix
let x: int    = 42
let name: string = "Casprix"
let pi: float = 3.14159
let flag: bool = true
```

### Functions

```casprix
func add(a: int, b: int) -> int {
    return a + b
}

let result = add(5, 3)  // result = 8
```

### Closures

```casprix
let increment = |x: int| => x + 1
```

Pipe-lambda syntax is available, but captured closures and first-class closure calls are still being finished in the current front end. See [examples/basic/closures.cpx](/d:/Projects/ND/examples/basic/closures.cpx) as a planned-surface reference rather than a guaranteed compiling sample.

### Generic Types

```casprix
class List<T> {
    mut items: array<T>;
    mut count: int;

    func add(item: T) {
        this.items[this.count] = item
        this.count = this.count + 1
    }
}

let numbers = new List<int>()
numbers.add(42)
```

### Async / Await

Async / await is planned for CASPRIX, but the current parser does not accept it yet. Keep async examples out of the main language guide until the surface syntax is wired end to end.

### Traits

```casprix
trait Printable {
    func toString() -> string
}

class Point {
    let x: int;
    let y: int;
}

impl Printable for Point {
    func toString() -> string {
        return "(" + this.x + ", " + this.y + ")"
    }
}
```

### Pattern Matching

```casprix
func describe(x: int) -> string {
    match x {
        0     => "zero",
        1..9  => "single digit",
        10..99 => "two digits",
        _     => "large number"
    }
}
```

### Classes & Inheritance

```casprix
class Animal {
    let name: string;
    func speak() -> string { return "..." }
}

class Dog extends Animal {
    func speak() -> string {
        return "Woof! I'm " + this.name
    }
}
```

---

## 🔧 Optimization Pipeline

```
Pass 1 — Const Propagation      Folds compile-time-known values inline
Pass 2 — Copy Propagation       Eliminates redundant variable copies
Pass 3 — Dead Code Elimination  Removes unreachable branches and unused defs
Pass 4 — Function Inlining      Expands small hot functions at call sites
Pass 5 — Escape Analysis        Stack-promotes heap allocations where safe
Pass 6 — Strength Reduction     Replaces expensive ops with cheaper equivalents
Pass 7 — CF Simplification      Merges/eliminates redundant basic blocks
Pass 8 — Loop Optimization      Invariant hoisting, 4x unrolling, AVX2 vectorization
```

Backend applies **linear scan register allocation** (85–90% utilization) and **AVX2 auto-vectorization** on eligible loops.

---

## 📦 Package Manager

```bash
casprix pkg init              # initialize project
casprix pkg install http      # install package
casprix pkg install json@^1.5 # with version constraint
```

### Version Constraints

| Syntax | Meaning |
|--------|---------|
| `^2.0.0` | Compatible with 2.x.x |
| `~2.1.0` | Patch updates only (2.1.x) |
| `>=1.5.0` | At least version 1.5.0 |
| `2.0.0` | Exact pin |

### `package.json`

```json
{
    "name": "myapp",
    "version": "1.0.0",
    "dependencies": {
        "http":   "^2.0.0",
        "json":   "^1.5.0",
        "crypto": "~3.1.0"
    }
}
```
[Quick Start](#-quick-start) · [Language Guide](#-language-guide) · [Architecture](ARCHITECTURE.md) · [Contributing](CONTRIBUTING.md) · [Benchmarks](#-performance-benchmarks) · [Package Manager](#-package-manager) · [Docs](#-documentation)

## 🗂️ Project Structure

```
casprix/
├── src/compiler/
│   ├── frontend/        # Lexer, parser, AST
│   ├── semantic/        # Type checking, escape analysis, ownership
│   ├── middle/          # IR, optimization passes, monomorphization
│   ├── backend/         # x86-64 NASM code generation
│   └── opt/             # Peephole, SIMD, inlining
├── runtime/             # Runtime library (C)
│   ├── memory/          # GC, regions, refcount, ownership
│   ├── async/           # Async/await coroutine scheduler
│   ├── net/             # Networking stack
│   └── llm/             # LLM training runtime (AVX2)
├── include/             # Public C headers
├── lib/                 # Casprix standard modules (.cpx)
├── stdlib/              # Core standard library
├── pkg/                 # Package manager source
├── tools/               # Build and development tools
│   └── build-tools/     # Internal build scripts (consolidated)
├── tests/               # Test suite
└── examples/
    ├── basic/           # Hello world, variables, functions
    ├── advanced/        # Async, closures, generics, threads
    ├── gui/             # GUI applications
    ├── ml/              # Machine learning
    ├── network/         # Networking
    └── tinystories/     # LLM training pipeline
```

---

## 📈 Project Statistics

| Metric | Value |
|:-------|:------|
| Compiler source | ~15,000 lines (C + x86-64 asm) |
| Runtime source | ~8,000 lines (memory, async, net, GUI, ML) |
| Compilation speedup | **7.5x** (arena allocator) |
| Runtime speedup | **5–10x** (combined optimizations) |
| Primary target | Windows x64 |
| Secondary targets | Linux, macOS |

---

## 🗺️ Roadmap

- [x] x86-64 AOT native code generation
- [x] Arena allocator + 8-pass optimization pipeline
- [x] Generics with monomorphization
- [x] Closures with variable capture
- [x] Async/await runtime
- [x] Built-in package manager
- [ ] VM bytecode backend (beta)
- [ ] ARM64 native target
---

## Roadmap

- [ ] Tiered JIT compiler
- [ ] Full borrow checker (IR-based)
- [ ] LSP language server
- [ ] Self-hosted compiler

---

## Documentation

- [Feature Summary](docs/FEATURES.md)
- [Project Structure](docs/PROJECT_STRUCTURE.md)
- [Package Manager Guide](docs/pkg/README.md)
- [Standard Library Reference](docs/stdlib/README.md)

---

## License

MIT License — see [LICENSE](LICENSE) for details.

---

<div align="center">

**Casprix — Fast, Modern, Powerful** 👻

</div>
