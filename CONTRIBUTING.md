# Contributing to Casprix

Casprix welcomes focused bug fixes, tests, documentation, and design proposals.
Before implementing a large language feature, open a discussion describing its
grammar, type-system impact, lowering strategy, compatibility, and tests.

Participation is governed by [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md). Report
security issues according to [SECURITY.md](SECURITY.md).

## Development setup

Required tools are CMake 3.20+, Ninja, a C11 compiler, and NASM.

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Use the release preset before submitting changes that affect optimization,
code generation, runtime behavior, or packaging:

```bash
cmake --preset release
cmake --build --preset release
ctest --test-dir build/release --output-on-failure
```

The `Makefile` is a compiler-only fast path. CMake is authoritative for the
complete compiler, runtime, tools, install layout, and tests.

## Change requirements

- Add positive and negative tests for language semantics.
- Add runtime tests for public C behavior and ownership boundaries.
- Keep diagnostics actionable and deterministic.
- Update the syntax EBNF and conformance matrix for language changes.
- Update `CHANGELOG.md` under `Unreleased` for user-visible changes.
- Preserve Windows and Linux behavior; avoid platform-specific assumptions in
  shared code.
- Do not commit build outputs, downloaded SDKs, datasets, model checkpoints, or
  generated logs.
- Keep public modules in `lib/`, public C headers in `include/casprix/`, and
  implementation details in `src/` or `runtime/`.

## Style

- C/C++ and Casprix use four-space indentation and a 100-column target.
- C symbols use `snake_case`; types use the established project convention.
- Casprix types use `PascalCase` and functions use `camelCase` or the
  surrounding module's established convention.
- Run `git diff --check` before submitting.
- `.clang-format`, `.editorconfig`, and `.gitattributes` define mechanical
  formatting and line-ending policy. Avoid unrelated bulk reformatting.

## Commits and pull requests

Use a descriptive branch and Conventional Commit-style subjects where useful,
for example `fix(parser): reject duplicate constructor labels`. Keep commits
reviewable and explain user-visible behavior, tests, compatibility, and known
limitations in the pull request.

All contributions are licensed under the project's MIT License.
