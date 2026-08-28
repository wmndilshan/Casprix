# Casprix

Casprix is a statically-typed, compiled programming language that targets x86-64.
The compiler is written in C. It has a hand-written lexer and parser, a semantic
analysis pass with ownership and escape analysis, an SSA-form mid-level IR (MIR)
with a small optimization pipeline, and two code paths: an AST-based native
backend (the default) and an MIR-based backend that can emit native code, C, VM
bytecode, or run a program in-process through a bytecode interpreter (CVM) with
an experimental JIT.

This is an early-stage project developed by a single author with the assistance
of AI coding agents. It is not a finished language and is not production-ready.
The sections below describe what the compiler does today, not what is planned.

## Building

Two build systems are provided.

### Make (compiler only)

```
make
```

Produces `bin/casprix`. This path builds only the compiler driver plus the small
runtime files it needs for the `--execute` mode. It is the fast path for working
on the compiler itself. `make test-all` runs the compiler over the `.cpx`
fixtures in `tests/compiler/` in `--parse-only` mode.

### CMake (full toolchain)

```
cmake -S . -B build
cmake --build build
```

Produces, under `build/`:

- `casprix` — the compiler driver
- `libcasprix_runtime.a` — the C runtime (memory, async, networking, math,
  stdlib, and ML subsystems)
- `casprix-pkg` — a package-manager utility
- `casprix-lsp` — a language server (diagnostics, document symbols,
  go-to-definition)
- the C test executables, registered with CTest

Optional components are controlled by CMake options, including `ENABLE_AVX2`
(on by default) and `ENABLE_SKIA_GUI` (off by default).

## Example

```
class Counter {
    mut value: int

    func bump() {
        this.value = this.value + 1
    }
}

func sum(items: Array<Int>) -> Int {
    total: Int = 0
    for item in items {
        total += item
    }
    return total
}

func main() -> Int {
    c: Counter = new Counter()
    c.bump()
    c.bump()
    c.bump()

    nums: Array<Int> = [10, 20, 30]
    print(sum(nums))   // 60
    print(c.value)     // 3

    return 0
}
```

```
bin/casprix example.cpx -o example
./example
```

Output:

```
60
3
```

More examples are in `examples/`; every file there compiles with the current
compiler (verified with `--check-only`).

## What works

- Functions, classes with single inheritance and virtual dispatch, traits with
  `impl` blocks, structs, enums, and unions.
- Generics with monomorphization.
- Pattern matching with literal, range, and wildcard arms.
- Fixed-width integer and float types (`i8`..`i128`, `u8`..`u128`, `f32`, `f64`).
- Closures: non-capturing lambdas and closures with read-only captures work as
  values and as calls. Closures that capture a mutable variable work when they
  do not escape the enclosing function (bound to a local, called in place,
  passed to a function parameter typed as a plain function).
- `async` / `await`: `async func` bodies are lowered to state machines. `await`
  is valid only inside an `async func`.
- Ownership and escape analysis; a borrow checker over MIR, enabled with
  `--safe-mode`.
- Optimization passes over MIR: constant folding and evaluation, mem2reg,
  inlining, dead-code elimination, and loop and SIMD passes.
- Backends: default AST native backend (x86-64, via an assembler and linker);
  MIR backends selected with `--native`/`--aot` (experimental), `--emit-c`,
  `--vm`, and `--jit`.
- `--execute`: runs a program in-process through the CVM without writing an
  executable. This works for functions, recursion, and conditionals; some
  constructs (for example certain loop shapes) currently fail MIR validation and
  are not yet supported on this path.
- Package manager (`casprix-pkg`): project init, dependency declaration in a
  `casper.json` manifest, and semantic-version constraints.
- Language server (`casprix-lsp`) and a VS Code extension under `ide/vscode/`.

## Known limitations

- The JIT tiers up a bounded set of functions (covered by tests such as
  `CVM_JIT_tierup`, `JIT_self_recursive_call`, and `JIT_register_allocator`).
  General function calls through the JIT are not supported yet.
- Returning a closure that captures variables is rejected by the compiler.
- Passing a closure that captures variables as a function argument is rejected;
  plain functions and non-capturing lambdas can be passed.
- The MIR native backend (`--native`) is experimental and not on par with the
  default backend.
- `spawn` is parsed but rejected with a "not implemented yet" diagnostic;
  `where` clauses and `scope` blocks are not implemented.
- ARM64 is not a supported target.
- The networking layer (`runtime/net/`, `runtime/net2/`) has known correctness
  gaps in its task/scheduler bookkeeping.
- The GUI framework (`runtime/skia/`) has no accessibility bridge.
- The design documents under `docs/design/` describe proposed systems that are
  largely unbuilt; they are marked as such.

## Tests

```
make test-all          # compiler over tests/compiler/*.cpx (parse-only)

cmake -S . -B build && cmake --build build
ctest --test-dir build # full suite: runtime, compiler, MIR, JIT
```

`make test-all` currently reports 41 passed, 0 failed. The CTest suite has a
small number of known failures unrelated to the compiler core (a runtime stdlib
test with drifted signatures, a SIMD virtualization test, and two compiler
fixtures that exercise unimplemented library or syntax features).

## License

MIT. See [LICENSE](LICENSE).
