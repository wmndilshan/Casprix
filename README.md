# Casprix

Casprix is an experimental, statically typed compiled language and toolchain
written primarily in C. The repository contains the compiler, runtime,
standard-library modules, package manager, language server, VS Code extension,
and Android tooling.

Casprix is under active development and is not yet a stable production
language. Language semantics, the standard library, package format, and
generated ABI may change before a stable release.

## Toolchain

| Component | Purpose | Maturity |
|---|---|---|
| `casprix` | Parser, type checker, MIR pipeline, native/C/VM/JIT backends | Active development |
| `libcasprix_runtime` | Memory, async, networking, GUI, math, and CPU AI runtime | Active development |
| `casprix-pkg` | Package manifests, resolution, cache, and registry client | Experimental |
| `casprix-lsp` | Diagnostics, symbols, and go-to-definition | Experimental |
| `ide/vscode` | Syntax support and language-server integration | Experimental |
| Android tooling | ARM64 NativeActivity APK build, install, and launch | Experimental |

The native compiler backend targets x86-64 Windows and Linux. The Android
runtime targets ARM64 through the NDK. macOS and other architectures are
best-effort and are not release-gated.

## Requirements

- CMake 3.20 or newer
- Ninja
- A C11 compiler (GCC/MinGW is the regularly tested toolchain)
- NASM for AVX2 assembly kernels
- Optional: Android SDK/NDK, Java, and ADB for Android builds

## Build and test

The checked-in presets provide consistent local and CI configurations:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

For an optimized build:

```bash
cmake --preset release
cmake --build --preset release
ctest --test-dir build/release --output-on-failure
```

Without presets:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The root `Makefile` remains a fast compiler-only development path. CMake is
the authoritative full-toolchain build.

## Language example

```cpx
class Point {
    public mut x: i32
    public mut y: i32

    public func Point(x: i32, y: i32) -> void {
        this.x = x
        this.y = y
    }
}

func classify(value: i32) -> i32 {
    if value < 0 {
        return -1
    } else if value == 0 {
        return 0
    }
    return 1
}

func main() -> i32 {
    let point = new Point(3, 4)
    return classify(point.x - 3)
}
```

Compile or type-check it:

```bash
casprix --version
casprix --check-only example.cpx
casprix example.cpx -o example
```

## Implemented language surface

- Functions, expression-bodied functions, classes, constructor overloads,
  inheritance, virtual dispatch, structs, enums, unions, traits, and `impl`
  blocks.
- Generics with monomorphization.
- Pattern matching with literals, ranges, wildcards, and enum exhaustiveness.
- Fixed-width numeric types, fixed arrays, ranges, slices, and `for-in`.
- `Result<T, E>` and postfix `?`.
- Non-capturing lambdas and bounded support for captured closures.
- `async func` and `await` lowering to state machines.
- Aliased imports and qualified imported function calls.
- Ownership and escape analysis, plus MIR borrow checking under `--safe-mode`.
- Native x86-64, C emission, CVM bytecode/interpreter, and experimental JIT
  paths.

The normative syntax references are
[CASPRIX_SYNTAX_EBNF.md](docs/CASPRIX_SYNTAX_EBNF.md) and
[CASPRIX_SYNTAX_CONFORMANCE_MATRIX.md](docs/CASPRIX_SYNTAX_CONFORMANCE_MATRIX.md).

## Android

```powershell
tools/android/casprix-android.ps1 doctor
tools/android/casprix-android.ps1 build -KeepBuild
tools/android/casprix-android.ps1 install
tools/android/casprix-android.ps1 launch
tools/android/casprix-android.ps1 logs
```

The default application is `examples/android/hello_android.cpx`. Prepare the
ARM64 Skia archive with `tools/android/build_skia_arm64.ps1`. Downloaded
source and intermediate toolchains are stored under ignored `build-tools/`;
the resulting local static library is also ignored.

## Known limitations

- The MIR native backend and JIT are experimental and do not support every
  language construct.
- Escaping captured closures and captured closures passed as plain function
  values are rejected.
- `spawn`, `where` clauses, and `scope` blocks are not implemented.
- The HTTP server currently focuses on bounded HTTP/1 request handling; TLS,
  persistent connections, and a stable production server API are not provided.
- Package registry and model-loading paths must not be treated as hardened
  untrusted-input boundaries.
- Some GUI syntax fixtures remain disabled while the UI API is being migrated.

## Repository guidance

- [Architecture](ARCHITECTURE.md)
- [Feature status](docs/FEATURES.md)
- [Project structure](docs/PROJECT_STRUCTURE.md)
- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)
- [Support policy](SUPPORT.md)
- [Changelog](CHANGELOG.md)

Casprix is available under the [MIT License](LICENSE).
