# Casprix Compiler — GNU Makefile
# Copyright (c) 2026 Casprix Project
# SPDX-License-Identifier: MIT
#
# SCOPE
#   This Makefile builds the CORE COMPILER ONLY (bin/casprix) and, on demand,
#   the runtime static library. It is the fast-iteration path.
#
#   The full toolchain — the LSP server (casprix-lsp), the package manager
#   (casprix-pkg), the APK builder (apk-builder), the Skia GUI framework
#   (skia_gui) and the C-level test executables — is built with CMake only:
#       cmake -S . -B build && cmake --build build && ctest --test-dir build
#
#   The compiler *source set* (src/compiler/**, src/support/**, src/driver/**,
#   src/util/**) is kept identical between this Makefile and CMakeLists.txt so
#   `bin/casprix` is byte-for-byte equivalent under either build system.
#
# Usage:
#   make              — build compiler (bin/casprix)
#   make runtime      — build runtime library
#   make all-libs     — build compiler + runtime
#   make test         — run quick smoke test
#   make test-all     — run full test suite (--parse-only over tests/compiler/)
#   make clean        — remove all build artefacts
#   make install      — install to /usr/local (Unix) or ask (Windows)
#   make help         — show this help
#
# Options:
#   DEBUG=1           — debug build (-g -O0)
#   HAS_AVX2=0        — disable AVX2 (enabled by default)

# ============================================================================
# Toolchain
# ============================================================================

CC      = gcc
AR      = ar
ASM     = nasm
RANLIB  = ranlib

# ============================================================================
# Flags
# ============================================================================

CFLAGS_BASE = -Wall -Wextra -Wno-unused-parameter -std=c11 -D_POSIX_C_SOURCE=200809L

ifdef DEBUG
    CFLAGS = $(CFLAGS_BASE) -g -O0 -DDEBUG
else
    CFLAGS = $(CFLAGS_BASE) -O2 -DNDEBUG
endif

# AVX2 SIMD — enabled by default, disable with HAS_AVX2=0
HAS_AVX2 ?= 1
ifeq ($(HAS_AVX2),1)
    CFLAGS       += -mavx2 -mfma -DHAS_AVX2
    SIMD_ASM      = $(RUNTIME_DIR)/math/simd_kernels.asm \
                    $(RUNTIME_DIR)/ai/llm/ops_avx2.asm \
                    $(RUNTIME_DIR)/async/coro_context.asm
    SIMD_OBJ      = $(OBJ_DIR)/simd_math_kernels.o \
                    $(OBJ_DIR)/simd_llm_avx2.o \
                    $(OBJ_DIR)/coro_context.o
endif

# ============================================================================
# Platform
# ============================================================================

ifeq ($(OS),Windows_NT)
    EXE_EXT  = .exe
    LDFLAGS  = -lws2_32 -lcomctl32 -lgdi32 -luser32
    RM       = del /q /f
    RMDIR    = rmdir /s /q
    MKDIR    = mkdir
    SEP      = \\
else
    EXE_EXT  =
    LDFLAGS  = -lpthread -lm
    RM       = rm -f
    RMDIR    = rm -rf
    MKDIR    = mkdir -p
    SEP      = /
endif

ASMFLAGS_WIN  = -f win64
ASMFLAGS_ELF  = -f elf64
ifeq ($(OS),Windows_NT)
    ASMFLAGS = $(ASMFLAGS_WIN)
else
    ASMFLAGS = $(ASMFLAGS_ELF)
endif

# ============================================================================
# Directories
# ============================================================================

SRC_DIR     = src
OBJ_DIR     = obj
BIN_DIR     = bin
RUNTIME_DIR = runtime
INCLUDE_DIR = include

INCLUDES = -I$(SRC_DIR) -I$(INCLUDE_DIR)

# ============================================================================
# Output artefacts
# ============================================================================

TARGET      = $(BIN_DIR)/casprix$(EXE_EXT)
RUNTIME_LIB = $(BIN_DIR)/libcasprix_runtime.a

# ============================================================================
# Compiler sources
# ============================================================================

# Support infrastructure (arena, logging, diagnostics, error)
SUPPORT_SOURCES = \
    $(SRC_DIR)/support/log.c \
    $(SRC_DIR)/support/error.c \
    $(SRC_DIR)/support/arena.c \
    $(SRC_DIR)/support/debug.c \
    $(SRC_DIR)/support/diagnostic.c

# Frontend (lexer, parser, AST)
FRONTEND_SOURCES = \
    $(SRC_DIR)/compiler/frontend/lexer.c \
    $(SRC_DIR)/compiler/frontend/parser.c \
    $(SRC_DIR)/compiler/frontend/ast.c

# Semantic analysis
SEMA_SOURCES = \
    $(SRC_DIR)/compiler/sema/semantic.c \
    $(SRC_DIR)/compiler/sema/symtable.c \
    $(SRC_DIR)/compiler/sema/ownership_check.c \
    $(SRC_DIR)/compiler/sema/escape_analysis.c \
    $(SRC_DIR)/compiler/sema/drop_planner.c \
    $(SRC_DIR)/compiler/sema/linear_view.c

# Data layout
LAYOUT_SOURCES = \
    $(SRC_DIR)/compiler/layout/data_layout.c

# Middle-end transforms
MIDDLE_SOURCES = \
    $(SRC_DIR)/compiler/middle/closure.c \
    $(SRC_DIR)/compiler/middle/monomorphize.c \
    $(SRC_DIR)/compiler/middle/trait.c \
    $(SRC_DIR)/compiler/middle/async.c

# MIR — mid-level IR
MIR_SOURCES = \
    $(SRC_DIR)/compiler/ir/mir.c \
    $(SRC_DIR)/compiler/ir/mir_builder.c \
    $(SRC_DIR)/compiler/ir/mir_printer.c \
    $(SRC_DIR)/compiler/ir/mir_lower.c \
    $(SRC_DIR)/compiler/ir/mir_opt.c \
    $(SRC_DIR)/compiler/ir/mir_consteval.c \
    $(SRC_DIR)/compiler/ir/mir_borrow.c \
    $(SRC_DIR)/compiler/ir/mir_backend.c \
    $(SRC_DIR)/compiler/ir/mir_c_backend.c \
    $(SRC_DIR)/compiler/ir/mir_mem2reg.c \
    $(SRC_DIR)/compiler/ir/mir_inline.c \
    $(SRC_DIR)/compiler/ir/mir_async.c \
    $(SRC_DIR)/compiler/ir/mir_regex.c

# Code generation (x86-64)
CODEGEN_SOURCES = \
    $(SRC_DIR)/compiler/codegen/asmgen.c \
    $(SRC_DIR)/compiler/codegen/optimizer.c \
    $(SRC_DIR)/compiler/codegen/regalloc.c

# Optimization passes
OPT_SOURCES = \
    $(SRC_DIR)/compiler/opt/loop_opt.c \
    $(SRC_DIR)/compiler/opt/simd.c \
    $(SRC_DIR)/compiler/opt/inline.c \
    $(SRC_DIR)/compiler/opt/peephole.c

# Utilities
UTIL_SOURCES = \
    $(SRC_DIR)/util/tools.c \
    $(SRC_DIR)/util/module.c

# Driver (CLI, I/O, pipeline, entry point)
DRIVER_SOURCES = \
    $(SRC_DIR)/driver/cli.c \
    $(SRC_DIR)/driver/io.c \
    $(SRC_DIR)/driver/pipeline.c \
    $(SRC_DIR)/main.c

ALL_COMPILER_SOURCES = \
    $(SUPPORT_SOURCES) \
    $(FRONTEND_SOURCES) \
    $(SEMA_SOURCES) \
    $(LAYOUT_SOURCES) \
    $(MIDDLE_SOURCES) \
    $(MIR_SOURCES) \
    $(CODEGEN_SOURCES) \
    $(OPT_SOURCES) \
    $(UTIL_SOURCES) \
    $(DRIVER_SOURCES)

COMPILER_OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/compiler/%.o,$(ALL_COMPILER_SOURCES))

# CVM interpreter + minimal I/O it needs — linked into the driver so that
# `casprix --execute` can run a compiled program in-process (Option A: no
# .cpxv/.cpxj file round-trip). Built with the runtime object rule (-Iruntime).
CVM_EXEC_SOURCES = \
    $(RUNTIME_DIR)/vm/cvm_engine.c \
    $(RUNTIME_DIR)/vm/jit_bridge.c \
    $(RUNTIME_DIR)/io/direct_io.c \
    $(RUNTIME_DIR)/io/fast_format.c
CVM_EXEC_OBJECTS = $(patsubst $(RUNTIME_DIR)/%.c,$(OBJ_DIR)/runtime/%.o,$(CVM_EXEC_SOURCES))

# ============================================================================
# Runtime sources
# ============================================================================

RUNTIME_CORE = \
    $(RUNTIME_DIR)/runtime.c \
    $(RUNTIME_DIR)/object.c \
    $(RUNTIME_DIR)/vtable_opt.c

RUNTIME_MEMORY = \
    $(RUNTIME_DIR)/memory/arc.c \
    $(RUNTIME_DIR)/memory/ownership.c \
    $(RUNTIME_DIR)/memory/cycle_gc.c \
    $(RUNTIME_DIR)/memory/memory.c \
    $(RUNTIME_DIR)/memory/gc.c \
    $(RUNTIME_DIR)/memory/region.c \
    $(RUNTIME_DIR)/memory/refcount.c \
    $(RUNTIME_DIR)/memory/tlocal_heap.c

ifeq ($(OS),Windows_NT)
    COROUTINE_SRC = $(wildcard $(RUNTIME_DIR)/async/coroutine_win32.c)
else
    COROUTINE_SRC = $(wildcard $(RUNTIME_DIR)/async/coroutine_posix.c)
endif

# Optional runtime subsystems (included only if files exist)
RUNTIME_EXT = \
    $(wildcard $(RUNTIME_DIR)/async/future.c) \
    $(wildcard $(RUNTIME_DIR)/async/task.c) \
    $(wildcard $(RUNTIME_DIR)/async/scheduler.c) \
    $(COROUTINE_SRC) \
    $(wildcard $(RUNTIME_DIR)/concurrent/channel.c) \
    $(wildcard $(RUNTIME_DIR)/sync/lockfree_deque.c) \
    $(wildcard $(RUNTIME_DIR)/sync/atomic.c) \
    $(wildcard $(RUNTIME_DIR)/net/socket.c) \
    $(wildcard $(RUNTIME_DIR)/net/http.c) \
    $(wildcard $(RUNTIME_DIR)/net/http_server.c) \
    $(wildcard $(RUNTIME_DIR)/io/file_async.c) \
    $(wildcard $(RUNTIME_DIR)/io/iocp.c) \
    $(wildcard $(RUNTIME_DIR)/file/file_runtime.c) \
    $(wildcard $(RUNTIME_DIR)/binding/lang_abi.c) \
    $(wildcard $(RUNTIME_DIR)/math/math_lib_runtime.c) \
    $(wildcard $(RUNTIME_DIR)/math/linalg_runtime.c) \
    $(wildcard $(RUNTIME_DIR)/math/stats_runtime.c) \
    $(wildcard $(RUNTIME_DIR)/ai/llm/*.c)

ALL_RUNTIME_SOURCES = $(RUNTIME_CORE) $(RUNTIME_MEMORY) $(RUNTIME_EXT)
RUNTIME_OBJECTS     = $(patsubst $(RUNTIME_DIR)/%.c,$(OBJ_DIR)/runtime/%.o,$(ALL_RUNTIME_SOURCES))

ifeq ($(HAS_AVX2),1)
    ALL_RUNTIME_OBJECTS = $(RUNTIME_OBJECTS) $(SIMD_OBJ)
else
    ALL_RUNTIME_OBJECTS = $(RUNTIME_OBJECTS)
endif

# ============================================================================
# Primary targets
# ============================================================================

.PHONY: all runtime all-libs test test-all test-quick clean install help

all: $(TARGET)

runtime: $(RUNTIME_LIB)

all-libs: $(TARGET) $(RUNTIME_LIB)

# ============================================================================
# Link
# ============================================================================

$(TARGET): $(COMPILER_OBJECTS) $(CVM_EXEC_OBJECTS) | $(BIN_DIR)
	@echo "[LINK] $@"
	$(CC) $(COMPILER_OBJECTS) $(CVM_EXEC_OBJECTS) -o $@ $(LDFLAGS)
	@echo "Build complete: $@"

$(RUNTIME_LIB): $(ALL_RUNTIME_OBJECTS) | $(BIN_DIR)
	@echo "[AR]   $@"
	$(AR) rcs $@ $(ALL_RUNTIME_OBJECTS)
	$(RANLIB) $@
	@echo "Runtime library: $@"

# ============================================================================
# Compiler object rules
# ============================================================================

$(OBJ_DIR)/compiler/%.o: $(SRC_DIR)/%.c
	@$(MKDIR) $(dir $@) 2>/dev/null || true
	@echo "[CC]   $<"
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ============================================================================
# Runtime object rules
# ============================================================================

# CVM exec objects need _GNU_SOURCE for MAP_ANONYMOUS (jit_bridge mmap path).
# This pattern rule is more specific than the generic runtime rule below and
# only matches the four CVM_EXEC_SOURCES via the explicit prerequisite list.
$(CVM_EXEC_OBJECTS): $(OBJ_DIR)/runtime/%.o: $(RUNTIME_DIR)/%.c
	@$(MKDIR) $(dir $@) 2>/dev/null || true
	@echo "[CC]   $< (cvm-exec)"
	$(CC) $(CFLAGS) -D_GNU_SOURCE $(INCLUDES) -I$(RUNTIME_DIR) -c $< -o $@

$(OBJ_DIR)/runtime/%.o: $(RUNTIME_DIR)/%.c
	@$(MKDIR) $(dir $@) 2>/dev/null || true
	@echo "[CC]   $<"
	$(CC) $(CFLAGS) $(INCLUDES) -I$(RUNTIME_DIR) -c $< -o $@

# ============================================================================
# SIMD assembly kernels
# ============================================================================

ifeq ($(HAS_AVX2),1)
$(OBJ_DIR)/simd_math_kernels.o: $(RUNTIME_DIR)/math/simd_kernels.asm | $(OBJ_DIR)
	@echo "[ASM]  $<"
	$(ASM) $(ASMFLAGS) $< -o $@

$(OBJ_DIR)/simd_llm_avx2.o: $(RUNTIME_DIR)/ai/llm/ops_avx2.asm | $(OBJ_DIR)
	@echo "[ASM]  $<"
	$(ASM) $(ASMFLAGS) $< -o $@

$(OBJ_DIR)/coro_context.o: $(RUNTIME_DIR)/async/coro_context.asm | $(OBJ_DIR)
	@echo "[ASM]  $<"
	$(ASM) $(ASMFLAGS) $< -o $@
endif

# ============================================================================
# Directory creation
# ============================================================================

$(BIN_DIR):
	$(MKDIR) $(BIN_DIR)

$(OBJ_DIR):
	$(MKDIR) $(OBJ_DIR)

# ============================================================================
# Tests
# ============================================================================

# Quick smoke test — compile a minimal program
test: $(TARGET)
	@echo "=== Quick smoke test ==="
	@$(TARGET) tests/compiler/test_minimal.cpx && echo "[PASS] test_minimal" || echo "[FAIL] test_minimal"

# Compile all .cpx files in tests/compiler/ and report pass/fail
test-all: $(TARGET)
	@echo "=== Compiler test suite ==="
	@PASS=0; FAIL=0; \
	for f in tests/compiler/*.cpx; do \
	    name=$$(basename "$$f" .cpx); \
	    if $(TARGET) "$$f" --parse-only > /dev/null 2>&1; then \
	        echo "[PASS] $$name"; PASS=$$((PASS+1)); \
	    else \
	        echo "[FAIL] $$name"; FAIL=$$((FAIL+1)); \
	    fi; \
	done; \
	echo ""; \
	echo "Results: $$PASS passed, $$FAIL failed"

# Compile a single example as a quick sanity check
test-quick: $(TARGET)
	@echo "=== Quick compilation check ==="
	@$(TARGET) examples/basic/first.cpx && echo "[PASS] examples/basic/first.cpx" || echo "[FAIL] examples/basic/first.cpx"

# ============================================================================
# Clean
# ============================================================================

clean:
ifeq ($(OS),Windows_NT)
	-$(RMDIR) $(OBJ_DIR) 2>nul
	-$(RMDIR) $(BIN_DIR) 2>nul
	-$(RM) *.exe *.o *.asm 2>nul
else
	$(RMDIR) $(OBJ_DIR) $(BIN_DIR)
	$(RM) *.exe *.o *.asm
endif
	@echo "Clean complete"

# ============================================================================
# Install
# ============================================================================

install: all-libs
ifeq ($(OS),Windows_NT)
	@echo "Windows install: copy $(TARGET) to a directory on your PATH."
	@echo "Runtime library: $(RUNTIME_LIB)"
else
	install -d /usr/local/bin /usr/local/lib
	install -m 755 $(TARGET) /usr/local/bin/casprix
	install -m 644 $(RUNTIME_LIB) /usr/local/lib/
	install -d /usr/local/include/casprix
	install -m 644 include/casprix/*.h /usr/local/include/casprix/
	@echo "Installed: /usr/local/bin/casprix"
endif

# ============================================================================
# Help
# ============================================================================

help:
	@echo "Casprix Compiler Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all         Build the compiler (default)"
	@echo "  runtime     Build the runtime library"
	@echo "  all-libs    Build compiler + runtime"
	@echo "  test        Quick smoke test"
	@echo "  test-all    Full compiler test suite"
	@echo "  test-quick  Compile one example as sanity check"
	@echo "  clean       Remove all build artefacts"
	@echo "  install     Install to system (Unix: /usr/local)"
	@echo "  help        Show this message"
	@echo ""
	@echo "Options:"
	@echo "  DEBUG=1     Build with debug symbols (-g -O0)"
	@echo "  HAS_AVX2=0  Disable AVX2/FMA SIMD optimizations"
	@echo ""
	@echo "Examples:"
	@echo "  make                   Build in release mode"
	@echo "  make DEBUG=1           Build in debug mode"
	@echo "  make HAS_AVX2=0        Build without SIMD"
	@echo "  make test-all          Run all compiler tests"
