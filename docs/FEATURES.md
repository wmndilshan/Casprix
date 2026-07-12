# Casprix compiler — feature overview

This file summarizes **language surface**, **compiler pipelines**, and **CLI usage** for the current tree. Line counts are indicative only.

## Language surface

### Core

- Classes with inheritance and virtual dispatch
- Closures (surface and capture rules still evolving in places)
- Generics with monomorphization (`List<T>`-style)
- Arrays, `string`, control flow (`if` / `while` / `for` / `match`)
- **Linear `StringView`**: non-owning borrow of `string` data with move-only semantics; checked in semantic / escape / drop passes (see [STDLIB_STRINGS_AND_REGEX.md](STDLIB_STRINGS_AND_REGEX.md))

### Optimizations (representative)

- MIR-based middle end: const eval, borrow checking on MIR, inlining, mem2reg, SIMD legalization, etc.
- x86-64 codegen: linear-scan register allocation, peephole passes, loop optimizations, SIMD path in `src/compiler/opt/simd.c`
- AVX2: enabled at **CMake** / compile time for the runtime and some kernels (`ENABLE_AVX2`), not via a `casprix -mavx2` flag

## Regex (toolchain)

- **MIR regex compiler** (`mir_regex.c`): pattern → NFA → DFA → MIR function for linear-time matching over bytes. Used in tests and corpus verification; see [STDLIB_STRINGS_AND_REGEX.md](STDLIB_STRINGS_AND_REGEX.md#4-regex--mir-compiler-inside-the-toolchain).

## Performance (illustrative)

Reported speedups in marketing docs are **order-of-magnitude** goals from combining RA + SIMD + loop opts + inlining; measure on your workload.

| Layer | Role |
|-------|------|
| Register allocation | Spill reduction, calling conventions |
| Peephole | Local instruction cleanup |
| Loop optimization | Unrolling, invariant motion |
| SIMD hints | Vectorize hot loops where legal |

## CLI usage

The driver binary is **`casprix`** (not `casprixc`). Build it with CMake (`scripts/build.sh` or `scripts/build.bat`), then:

```bash
casprix program.cpx -o out
casprix program.cpx -o out --opt-level=2
casprix program.cpx --check-only
casprix --help
```

Relevant backend / pipeline flags (see `--help` for the full set):

- `--mir` — run MIR middle end before codegen
- `--native` / `--aot` — MIR native backend (experimental)
- `--vm` / `--jit` — MIR VM / JIT-oriented emission paths (experimental)
- `--emit-c` — C emission via MIR
- `--safe-mode` — borrow checking (implies `--mir`)

Flags such as `-mavx2` or `-fno-vectorize` on the **compiler executable** are not the primary knobs; vectorization of **generated** code is driven by the optimizer and target options in your build.

## Tests

- `tests/compiler/test_*.cpx` — compile-and-run
- `tests/compiler/test_mir_regex.c` — regex → MIR pipeline
- `tests/runtime/test_stdlib.c` — C runtime string helpers
- `tests/corpus/stringview_linear.cpx` — StringView linearity

## Status

The compiler and runtime are under active development. Treat “production ready” claims in older snapshots as aspirational unless validated for your use case.
