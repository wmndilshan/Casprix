# Contributing to Casprix

Welcome to the Casprix project! This document provides guidelines for contributing to the compiler, runtime, and standard library.

## Git Workflow

The project follows a standardized development workflow:

1.  **Branching**: Create a descriptive branch for your work.
    ```bash
    git checkout -b feature/your-feature-name
    ```
2.  **Committing**: We recommend [Conventional Commits](https://www.conventionalcommits.org/):
    - `feat: add new ghost-call optimization`
    - `fix: resolve memory leak in parser`
    - `docs: update architecture diagram`
3.  **Pull Requests**: Open a PR against the `main` branch with a clear description of the problem and your solution.

## Commit Guidelines

- Use the present tense ("add" not "added").
- Use the imperative mood ("move" not "moves").
- Limit the subject line to 72 characters.
- Reference issues and pull requests liberally.

## Running Tests

All contributions must pass the automated test suite before merging.

### 1. Build Verification
```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build
```

### 2. CTest Suite
```bash
cd build
ctest --output-on-failure
```

### 3. Manual Compiler Tests
```bash
# Run the standalone C test runner
./build/tests/compiler_suite_runner
```

## Development Standards

1.  **Code Style**: Follow the existing C style in the codebase (2-space indentation, descriptive naming).
2.  **Documentation**: Update relevant `.md` files and header comments for any new functionality.
3.  **Portability**: Ensure changes are compatible with Windows (MinGW/MSVC), Linux, and macOS.

## License

By contributing to Casprix, you agree that your contributions will be licensed under the project's MIT License.
