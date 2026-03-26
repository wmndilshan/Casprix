# Casprix Architecture Overview 👻

Casprix is a high-performance compiled programming language designed for systems programming, real-time graphics, and machine learning inference. This document describes the compiler's modular architecture and its compilation pipeline.

## 1. Modular Driver Design

The Casprix compiler driver (`src/driver/`) is organized into specialized modules to ensure maintainability and a clean separation of concerns:

- **CLI (`cli.c/h`)**: A robust command-line interface that handles option parsing, environment discovery (NDK/SDK paths), and session configuration.
- **IO (`io.c/h`)**: Centralized resource management for file reading, code emission, and diagnostic logging.
- **Pipeline (`pipeline.c/h`)**: The orchestration layer that executes compiler passes in a strictly defined sequence.

## 2. Compilation Pipeline

Casprix uses a multi-pass compilation strategy to transform source code into optimized native machine code.

### Pass 1: Frontend (Lexical & Syntax)
- **Lexer**: Converts raw source text into a stream of typed tokens.
- **Parser**: A recursive-descent parser that builds the Abstract Syntax Tree (AST), enforcing the language's formal grammar.

### Pass 2: Semantic Analysis & Symbol Resolution
- **Symbol Table**: Manages nested scopes, class hierarchies, and member visibility.
- **Type Checking**: Enforces strong typing, generic monomorphization, and interface conformance.
- **Ownership Analysis**: Tracks resource lifetimes and ensures memory safety without a runtime garbage collector.

### Pass 3: Intermediate Representation (IR)
- The AST is lowered into a target-independent Mid-level IR (MIR), facilitating high-level optimizations and multi-backend support.

### Pass 4: Backend & Code Generation
- **Optimization**: Performs peephole reductions, constant folding, and dead-code elimination.
- **Register Allocation**: Uses a linear-scan allocator to map IR variables to physical CPU registers (x86-64).
- **Emission**: Generates NASM-compatible assembly, following platform-specific ABIs (Windows x64 Calling Convention).

## 3. Runtime Integration

The Casprix runtime library (`runtime/`) is a performance-critical layer written in C and hand-optimized Assembly (AVX2):

- **Core Runtime**: Low-latency Arena allocation and string interning.
- **Graphics (Skia)**: Native 2D/3D rendering backed by the Skia library.
- **ML/Tensor (AVX2)**: High-speed mathematical kernels for neural network inference.
- **Networking/Async**: An event-driven I/O model based on asynchronous sockets.

## 4. Design Philosophy

- **Transparency**: High-level features (closures, generics) have predictable, zero-cost implementations.
- **Safety**: Strong compile-time guarantees for memory and resource usage.
- **Efficiency**: A slim runtime footprint with deep hardware utilization.
