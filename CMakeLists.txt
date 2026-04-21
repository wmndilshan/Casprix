# CMakeLists.txt — Casprix Compiler Build System
# Copyright (c) 2026 Casprix Project
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)

project(Casprix
    VERSION 1.0.0
    LANGUAGES C
    DESCRIPTION "Casprix: Modern high-performance compiled programming language"
    HOMEPAGE_URL "https://github.com/wmndilshan/casprix"
)

# ============================================================================
# Standard CMake settings
# ============================================================================

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Release if no build type specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# ============================================================================
# Build options
# ============================================================================

option(BUILD_COMPILER        "Build Casprix compiler executable"     ON)
option(BUILD_RUNTIME         "Build libcasprix_runtime static lib"   ON)
option(BUILD_PACKAGE_MANAGER "Build casprix-pkg utility"             ON)
option(BUILD_TESTS           "Build and register the test suite"     ON)
option(ENABLE_AVX2           "Enable AVX2/FMA SIMD optimizations"    ON)
option(ENABLE_DEBUG_LOGGING  "Compile with DEBUG_LOGGING defined"    OFF)
option(ENABLE_SKIA_GUI       "Enable Skia-based GUI framework"       OFF)
option(SKIA_GUI_ENABLE_ASAN  "Enable AddressSanitizer for skia_gui consumers" OFF)
option(SKIA_GUI_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer for skia_gui consumers" OFF)
option(CASPRIX_ENABLE_NINJA_AUTOREGEN
       "Allow Ninja to rerun CMake automatically during builds"
       OFF)

# CMake regeneration has been flaky on the Windows/Ninja build tree,
# leaving behind .tmp build files and stalling otherwise simple target builds.
# Prefer explicit reconfigure steps over auto-regeneration there.
if(WIN32 AND CMAKE_GENERATOR MATCHES "Ninja" AND NOT CASPRIX_ENABLE_NINJA_AUTOREGEN)
    set(CMAKE_SUPPRESS_REGENERATION ON)
endif()

# NASM — optional, needed for SIMD assembly kernels
include(CheckLanguage)
check_language(ASM_NASM)
if(CMAKE_ASM_NASM_COMPILER AND ENABLE_AVX2)
    enable_language(ASM_NASM)
    set(NASM_AVAILABLE ON)
    # NASM output format: elf64 on Linux/macOS, win64 on Windows
    if(WIN32)
        set(CMAKE_ASM_NASM_FLAGS "-f win64" CACHE STRING "" FORCE)
    else()
        set(CMAKE_ASM_NASM_FLAGS "-f elf64" CACHE STRING "" FORCE)
    endif()
    # Do NOT forward C compile flags (<FLAGS>) to NASM — it only understands its own options
    set(CMAKE_ASM_NASM_COMPILE_OBJECT
        "<CMAKE_ASM_NASM_COMPILER> ${CMAKE_ASM_NASM_FLAGS} -o <OBJECT> <SOURCE>")
elseif(ENABLE_AVX2)
    message(STATUS "NASM not found — AVX2 assembly kernels disabled, falling back to C")
    set(ENABLE_AVX2 OFF CACHE BOOL "" FORCE)
    set(NASM_AVAILABLE OFF)
else()
    set(NASM_AVAILABLE OFF)
endif()

# ============================================================================
# Compiler flags — use interface targets for clean propagation
# ============================================================================

add_library(casprix_compile_options INTERFACE)

if(MSVC)
    target_compile_options(casprix_compile_options INTERFACE
        /W4 /wd4996
        $<$<CONFIG:Release>:/O2 /DNDEBUG>
        $<$<CONFIG:Debug>:/Od /Zi>
    )
else()
    target_compile_options(casprix_compile_options INTERFACE
        -Wall -Wextra -Wno-unused-parameter
        $<$<CONFIG:Release>:-O2 -DNDEBUG>
        $<$<CONFIG:Debug>:-O0 -g>
        $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG>
        $<$<BOOL:${ENABLE_AVX2}>:-mavx2 -mfma>
    )
    # Expose all glibc extensions on Linux/macOS (includes POSIX-2008, strdup, usleep, DT_DIR…)
    target_compile_definitions(casprix_compile_options INTERFACE
        $<$<NOT:$<BOOL:${WIN32}>>:_GNU_SOURCE=1>
    )
endif()

if(ENABLE_DEBUG_LOGGING)
    target_compile_definitions(casprix_compile_options INTERFACE DEBUG_LOGGING)
endif()
if(ENABLE_AVX2)
    target_compile_definitions(casprix_compile_options INTERFACE HAS_AVX2)
endif()

# Shared include paths

add_library(casprix_include_dirs INTERFACE)
target_include_directories(casprix_include_dirs INTERFACE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

# ============================================================================
# Compiler source groups
# ============================================================================

set(SUPPORT_SOURCES
    src/support/log.c
    src/support/error.c
    src/support/arena.c
    src/support/debug.c
    src/support/diagnostic.c
)

set(FRONTEND_SOURCES
    src/compiler/frontend/lexer.c
    src/compiler/frontend/parser.c
    src/compiler/frontend/ast.c
)

set(SEMA_SOURCES
    src/compiler/sema/semantic.c
    src/compiler/sema/symtable.c
    src/compiler/sema/ownership_check.c
    src/compiler/sema/escape_analysis.c
    src/compiler/sema/drop_planner.c
    src/compiler/sema/linear_view.c
)

set(LAYOUT_SOURCES
    src/compiler/layout/data_layout.c
)

set(MIDDLE_SOURCES
    src/compiler/middle/closure.c
    src/compiler/middle/monomorphize.c
    src/compiler/middle/trait.c
    src/compiler/middle/async.c
)

set(MIR_SOURCES
    src/compiler/ir/mir.c
    src/compiler/ir/mir_builder.c
    src/compiler/ir/mir_printer.c
    src/compiler/ir/mir_lower.c
    src/compiler/ir/mir_opt.c
    src/compiler/ir/mir_consteval.c
    src/compiler/ir/mir_borrow.c
    src/compiler/ir/mir_backend.c
    src/compiler/ir/mir_c_backend.c
    src/compiler/ir/mir_mem2reg.c
    src/compiler/ir/mir_inline.c
    src/compiler/ir/mir_async.c
    src/compiler/ir/mir_regex.c
)

set(CODEGEN_SOURCES
    src/compiler/codegen/asmgen.c
    src/compiler/codegen/optimizer.c
    src/compiler/codegen/regalloc.c
)

set(OPT_SOURCES
    src/compiler/opt/loop_opt.c
    src/compiler/opt/simd.c
    src/compiler/opt/inline.c
    src/compiler/opt/peephole.c
)

set(UTIL_SOURCES
    src/util/tools.c
    src/util/module.c
)

set(DRIVER_SOURCES
    src/driver/cli.c
    src/driver/io.c
    src/driver/pipeline.c
    src/main.c
)

set(ALL_COMPILER_SOURCES
    ${SUPPORT_SOURCES}
    ${FRONTEND_SOURCES}
    ${SEMA_SOURCES}
    ${LAYOUT_SOURCES}
    ${MIDDLE_SOURCES}
    ${MIR_SOURCES}
    ${CODEGEN_SOURCES}
    ${OPT_SOURCES}
    ${UTIL_SOURCES}
    ${DRIVER_SOURCES}
)

# ============================================================================
# Compiler library + driver executable
# ============================================================================
#
# The bulk of the compiler is built as a static library so MIR verification
# and other integration tests can link the same sources without compiling
# a second monolithic executable.

if(BUILD_COMPILER)
    set(CASPRIX_COMPILER_LIB_SOURCES ${ALL_COMPILER_SOURCES})
    list(REMOVE_ITEM CASPRIX_COMPILER_LIB_SOURCES src/main.c)

    add_library(casprix_compiler STATIC ${CASPRIX_COMPILER_LIB_SOURCES})
    target_link_libraries(casprix_compiler PRIVATE
        casprix_compile_options
        casprix_include_dirs
        $<$<NOT:$<BOOL:${MSVC}>>:m>
    )

    add_executable(casprix src/main.c)
    target_link_libraries(casprix PRIVATE
        casprix_compiler
        casprix_compile_options
        casprix_include_dirs
        $<$<NOT:$<BOOL:${MSVC}>>:m>
    )

    set_target_properties(casprix PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    install(TARGETS casprix DESTINATION bin)
endif()

# ============================================================================
# Runtime library
# ============================================================================

if(BUILD_RUNTIME)
    set(RUNTIME_SOURCES runtime/runtime.c)

    # CVM interpreter + JIT bridge (register-based MIR VM)
    foreach(_f cvm_engine jit_bridge)
        if(EXISTS "${CMAKE_SOURCE_DIR}/runtime/vm/${_f}.c")
            list(APPEND RUNTIME_SOURCES "runtime/vm/${_f}.c")
        endif()
    endforeach()

    # Memory subsystem
    foreach(_f arc ownership cycle_gc memory gc region refcount tlocal_heap)
        if(EXISTS "${CMAKE_SOURCE_DIR}/runtime/memory/${_f}.c")
            list(APPEND RUNTIME_SOURCES "runtime/memory/${_f}.c")
        endif()
    endforeach()

    # Object system
    foreach(_f object vtable_opt)
        if(EXISTS "${CMAKE_SOURCE_DIR}/runtime/${_f}.c")
            list(APPEND RUNTIME_SOURCES "runtime/${_f}.c")
        endif()
    endforeach()

    # LLM subsystem
    foreach(_f tensor ops transformer backward training checkpoint
                 tokenizer bindings autograd transformer_backward
                 tensor_ir graph_opt jit)
        if(EXISTS "${CMAKE_SOURCE_DIR}/runtime/ai/llm/${_f}.c")
            list(APPEND RUNTIME_SOURCES "runtime/ai/llm/${_f}.c")
        endif()
    endforeach()

    # Math subsystem
    foreach(_f math_lib_runtime linalg_runtime stats_runtime)
        if(EXISTS "${CMAKE_SOURCE_DIR}/runtime/math/${_f}.c")
            list(APPEND RUNTIME_SOURCES "runtime/math/${_f}.c")
        endif()
    endforeach()
    # SIMD assembly kernels (NASM, only when AVX2 enabled and NASM available)
    if(NASM_AVAILABLE AND EXISTS "${CMAKE_SOURCE_DIR}/runtime/math/simd_kernels.asm")
        list(APPEND RUNTIME_SOURCES runtime/math/simd_kernels.asm)
        set_source_files_properties(runtime/math/simd_kernels.asm
            PROPERTIES COMPILE_OPTIONS "")  # don't forward C flags to NASM
    endif()
    if(NASM_AVAILABLE AND EXISTS "${CMAKE_SOURCE_DIR}/runtime/ai/llm/ops_avx2.asm")
        list(APPEND RUNTIME_SOURCES runtime/ai/llm/ops_avx2.asm)
        set_source_files_properties(runtime/ai/llm/ops_avx2.asm
            PROPERTIES COMPILE_OPTIONS "")
    endif()

    # Async subsystem
    foreach(_f future task scheduler)
        if(EXISTS "${CMAKE_SOURCE_DIR}/runtime/async/${_f}.c")
            list(APPEND RUNTIME_SOURCES "runtime/async/${_f}.c")
        endif()
    endforeach()
    if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/runtime/async/coroutine_win32.c")
        list(APPEND RUNTIME_SOURCES runtime/async/coroutine_win32.c)
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/runtime/async/coroutine_posix.c")
        list(APPEND RUNTIME_SOURCES runtime/async/coroutine_posix.c)
    endif()

    # Concurrent / sync
    if(EXISTS "${CMAKE_SOURCE_DIR}/runtime/concurrent/channel.c")
        list(APPEND RUNTIME_SOURCES runtime/concurrent/channel.c)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/runtime/sync/lockfree_deque.c")
        list(APPEND RUNTIME_SOURCES runtime/sync/lockfree_deque.c)
    endif()

    # Network (new clean API — legacy disabled due to type conflicts)
    foreach(_f socket http http_server)
        if(EXISTS "${CMAKE_SOURCE_DIR}/runtime/net/${_f}.c")
            list(APPEND RUNTIME_SOURCES "runtime/net/${_f}.c")
        endif()
    endforeach()

    # I/O subsystem
    if(EXISTS "${CMAKE_SOURCE_DIR}/runtime/io/file_async.c")
        list(APPEND RUNTIME_SOURCES runtime/io/file_async.c)
    endif()
    if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/runtime/io/iocp.c")
        list(APPEND RUNTIME_SOURCES runtime/io/iocp.c)
    elseif(UNIX AND EXISTS "${CMAKE_SOURCE_DIR}/runtime/io/epoll.c")
        list(APPEND RUNTIME_SOURCES runtime/io/epoll.c)
    endif()

    # File runtime + language ABI shim
    if(EXISTS "${CMAKE_SOURCE_DIR}/runtime/file/file_runtime.c")
        list(APPEND RUNTIME_SOURCES runtime/file/file_runtime.c)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/runtime/binding/lang_abi.c")
        list(APPEND RUNTIME_SOURCES runtime/binding/lang_abi.c)
    endif()

    # Stdlib collections and string ops
    foreach(_f collections string_ops)
        if(EXISTS "${CMAKE_SOURCE_DIR}/runtime/stdlib/${_f}.c")
            list(APPEND RUNTIME_SOURCES "runtime/stdlib/${_f}.c")
        endif()
    endforeach()

    add_library(casprix_runtime STATIC ${RUNTIME_SOURCES})

    target_link_libraries(casprix_runtime PUBLIC
        casprix_compile_options
        $<$<NOT:$<BOOL:${MSVC}>>:m>
        $<$<BOOL:${WIN32}>:ws2_32>
        $<$<NOT:$<BOOL:${MSVC}>>:pthread>
    )

    target_include_directories(casprix_runtime PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/runtime
        ${CMAKE_SOURCE_DIR}/runtime/memory
        ${CMAKE_SOURCE_DIR}/runtime/ai/llm
        ${CMAKE_SOURCE_DIR}/runtime/ai/nn
        ${CMAKE_SOURCE_DIR}/runtime/ai/ml
        ${CMAKE_SOURCE_DIR}/runtime/async
        ${CMAKE_SOURCE_DIR}/runtime/concurrent
        ${CMAKE_SOURCE_DIR}/runtime/sync
        ${CMAKE_SOURCE_DIR}/runtime/net
        ${CMAKE_SOURCE_DIR}/runtime/io
        ${CMAKE_SOURCE_DIR}/runtime/stdlib
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )

    set_target_properties(casprix_runtime PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    install(TARGETS casprix_runtime DESTINATION lib)
endif()

# ============================================================================
# Package manager
# ============================================================================

if(BUILD_PACKAGE_MANAGER AND EXISTS "${CMAKE_SOURCE_DIR}/pkg/core/registry.c")
    set(PKG_SOURCES "")
    foreach(_f registry resolver manifest cache installer publisher semver)
        if(EXISTS "${CMAKE_SOURCE_DIR}/pkg/core/${_f}.c")
            list(APPEND PKG_SOURCES "pkg/core/${_f}.c")
        endif()
    endforeach()
    foreach(_f main cli cli_complete)
        if(EXISTS "${CMAKE_SOURCE_DIR}/pkg/cli/${_f}.c")
            list(APPEND PKG_SOURCES "pkg/cli/${_f}.c")
        endif()
    endforeach()

    add_executable(casprix-pkg ${PKG_SOURCES})
    target_include_directories(casprix-pkg PRIVATE
        ${CMAKE_SOURCE_DIR}/pkg
        ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(casprix-pkg PRIVATE
        casprix_compile_options
        $<$<NOT:$<BOOL:${MSVC}>>:m>
    )
    set_target_properties(casprix-pkg PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    install(TARGETS casprix-pkg DESTINATION bin)
endif()

# ============================================================================
# Skia GUI framework
# ============================================================================

if(EXISTS "${CMAKE_SOURCE_DIR}/runtime/skia/scene_graph.c")
    set(SKIA_DIR "${CMAKE_SOURCE_DIR}/third_party/skia")
    option(USE_SKIA_BACKEND
        "Use real Skia C++ backend (requires MSVC-compatible toolchain)" OFF)

    if(ENABLE_SKIA_GUI AND USE_SKIA_BACKEND AND EXISTS "${SKIA_DIR}/lib/skia.lib")
        enable_language(CXX)
        set(CMAKE_CXX_STANDARD 17)
        set(CMAKE_CXX_STANDARD_REQUIRED ON)
        add_library(skia_c STATIC runtime/skia/skia_c.cpp)
        target_include_directories(skia_c PRIVATE
            ${SKIA_DIR} ${CMAKE_SOURCE_DIR}/runtime/skia)
        target_link_libraries(skia_c PRIVATE "${SKIA_DIR}/lib/skia.lib")
        message(STATUS "  Skia backend:  C++ (skia_c.cpp)")
    else()
        add_library(skia_c STATIC runtime/skia/skia_c_gdi.c)
        target_include_directories(skia_c PRIVATE
            ${CMAKE_SOURCE_DIR}/runtime/skia)
        message(STATUS "  Skia backend:  GDI fallback (skia_c_gdi.c)")
    endif()

    set(SKIA_GUI_SOURCES
        runtime/skia/scene_graph.c
        runtime/skia/layout.c
        runtime/skia/events.c
        runtime/skia/widgets.c
        runtime/skia/charts.c
        runtime/skia/effects.c
        runtime/skia/shapes.c
        runtime/skia/style.c
        runtime/skia/text.c
        runtime/skia/animation.c
        runtime/skia/frame_loop.c
        runtime/skia/skia_window_win32.c
    )
    if(ANDROID AND EXISTS "${CMAKE_SOURCE_DIR}/runtime/skia/frame_loop_android.c")
        list(APPEND SKIA_GUI_SOURCES runtime/skia/frame_loop_android.c)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/runtime/android/android_runtime.c")
        list(APPEND SKIA_GUI_SOURCES runtime/android/android_runtime.c)
    endif()

    add_library(skia_gui STATIC ${SKIA_GUI_SOURCES})
    target_include_directories(skia_gui PUBLIC
        ${CMAKE_SOURCE_DIR}/runtime/skia
        ${CMAKE_SOURCE_DIR}/runtime/android)
    target_link_libraries(skia_gui PUBLIC
        skia_c
        $<$<BOOL:${WIN32}>:user32 gdi32 ole32 oleaut32>)

    if(SKIA_GUI_ENABLE_ASAN)
        if(MSVC)
            target_compile_options(skia_gui PUBLIC /fsanitize=address)
            target_link_options(skia_gui INTERFACE /fsanitize=address)
        else()
            target_compile_options(skia_gui PUBLIC -fsanitize=address -fno-omit-frame-pointer)
            target_link_options(skia_gui INTERFACE -fsanitize=address)
        endif()
    endif()

    if(SKIA_GUI_ENABLE_UBSAN)
        if(MSVC)
            message(WARNING "SKIA_GUI_ENABLE_UBSAN is not supported on MSVC; ignoring toggle")
        else()
            target_compile_options(skia_gui PUBLIC -fsanitize=undefined -fno-omit-frame-pointer)
            target_link_options(skia_gui INTERFACE -fsanitize=undefined)
        endif()
    endif()

    if(BUILD_RUNTIME)
        target_link_libraries(casprix_runtime PUBLIC skia_gui)
    endif()

    if(EXISTS "${CMAKE_SOURCE_DIR}/test_skia_hello.c")
        add_executable(test_skia_hello test_skia_hello.c)
        target_link_libraries(test_skia_hello PRIVATE
            skia_gui $<$<BOOL:${WIN32}>:user32 gdi32 ole32 oleaut32>)
    endif()
endif()

# ============================================================================
# APK builder tool
# ============================================================================

if(EXISTS "${CMAKE_SOURCE_DIR}/tools/apk_builder/main.c")
    add_executable(apk-builder
        tools/apk_builder/main.c
        tools/apk_builder/apk_builder.c
        tools/apk_builder/manifest.c
        tools/apk_builder/zip_writer.c
        tools/apk_builder/ndk_utils.c
    )
    target_include_directories(apk-builder PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/apk_builder)
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        target_link_libraries(apk-builder PRIVATE ZLIB::ZLIB)
        target_compile_definitions(apk-builder PRIVATE HAVE_ZLIB)
    endif()
    target_link_libraries(apk-builder PRIVATE
        $<$<NOT:$<BOOL:${MSVC}>>:m>)
    install(TARGETS apk-builder DESTINATION bin)
endif()

# ============================================================================
# Tests
# ============================================================================

if(BUILD_TESTS AND EXISTS "${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()

# ============================================================================
# Install rules
# ============================================================================

install(DIRECTORY include/casprix/ DESTINATION include/casprix
        FILES_MATCHING PATTERN "*.h")

if(EXISTS "${CMAKE_SOURCE_DIR}/stdlib")
    install(DIRECTORY stdlib/ DESTINATION lib/casprix/stdlib
            FILES_MATCHING PATTERN "*.cpx")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/lib")
    install(DIRECTORY lib/ DESTINATION lib/casprix/lib
            FILES_MATCHING PATTERN "*.cpx")
endif()

# ============================================================================
# Configuration summary
# ============================================================================

message(STATUS "")
message(STATUS "╔══════════════════════════════════════╗")
message(STATUS "║   Casprix Compiler  v${PROJECT_VERSION}          ║")
message(STATUS "╠══════════════════════════════════════╣")
message(STATUS "║ Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "║ Compiler:     ${CMAKE_C_COMPILER_ID}")
message(STATUS "║ Install:      ${CMAKE_INSTALL_PREFIX}")
message(STATUS "╠══════════════════════════════════════╣")
message(STATUS "║ Components:")
message(STATUS "║   Compiler:   ${BUILD_COMPILER}")
message(STATUS "║   Runtime:    ${BUILD_RUNTIME}")
message(STATUS "║   Pkg mgr:    ${BUILD_PACKAGE_MANAGER}")
message(STATUS "║   Tests:      ${BUILD_TESTS}")
message(STATUS "╠══════════════════════════════════════╣")
message(STATUS "║ Features:")
message(STATUS "║   AVX2:       ${ENABLE_AVX2}")
message(STATUS "║   Debug log:  ${ENABLE_DEBUG_LOGGING}")
message(STATUS "║   Skia GUI:   ${ENABLE_SKIA_GUI}")
message(STATUS "╚══════════════════════════════════════╝")
message(STATUS "")
