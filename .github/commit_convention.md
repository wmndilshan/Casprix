# Commit Message Convention

Casprix uses **Conventional Commits** (`v1.0.0`).

```
<type>(<scope>): <short summary>

[optional body — wrap at 72 chars]

[optional footer: Closes #N, Breaking-Change: ...]
```

## Types

| Type       | When to use                                              |
|------------|----------------------------------------------------------|
| `feat`     | New language feature, stdlib addition, or tool           |
| `fix`      | Bug fix (compiler crash, wrong codegen, runtime fault)   |
| `perf`     | Performance improvement with measurable impact           |
| `refactor` | Code restructuring — no functional change                |
| `test`     | Add or fix tests                                         |
| `docs`     | Documentation, comments, examples                        |
| `build`    | CMakeLists, Makefile, CI, dependencies                   |
| `ci`       | GitHub Actions workflow changes only                     |
| `chore`    | Maintenance that doesn't fit elsewhere (version bumps…)  |
| `revert`   | Reverts a previous commit                                |

## Scopes (optional but encouraged)

`lexer` · `parser` · `sema` · `mir` · `codegen` · `regalloc` · `linker`
`runtime` · `memory` · `async` · `ai` · `gui` · `skia`
`stdlib` · `pkg` · `ci` · `docs`

## Examples

```
feat(parser): add pattern matching on enum variants

fix(codegen): align call-site stack to 16 bytes on Windows x64
Closes #42

perf(ai/llm): replace scalar matmul with AVX2 kernel

build: upgrade to CMake 3.28 and Ninja 1.12

docs(stdlib): add usage examples for Collections.cpx
```

## Rules

- Summary line: **≤ 72 characters**, **imperative mood**, **no period at end**
- Breaking changes: add `!` after type/scope (`feat!:`) **and** a
  `Breaking-Change: <description>` footer
- Reference issues in the footer: `Closes #N`, `Fixes #N`, `Related to #N`
