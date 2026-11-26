# Casprix Test Suite
cmake_minimum_required(VERSION 3.20)

if(TARGET casprix)
    add_test(NAME compiler_version COMMAND $<TARGET_FILE:casprix> --version)
    set_tests_properties(compiler_version PROPERTIES
        PASS_REGULAR_EXPRESSION "casprix [0-9]+\\.[0-9]+\\.[0-9]+")
endif()

# ── Runtime unit tests ──────────────────────────────────────────────────────
set(RUNTIME_TEST_SRCS
    runtime/test_stdlib.c
    runtime/test_scheduler.c
    runtime/test_networking.c
    runtime/test_arc_gc.c
    runtime/benchmark.c
)

# This fixture intentionally drives raw POSIX descriptors and pthreads. The
# portable socket API is covered by test_networking on Windows.
if(UNIX)
    list(APPEND RUNTIME_TEST_SRCS runtime/test_net_hardening.c)
endif()

if(WIN32)
    list(APPEND RUNTIME_TEST_SRCS runtime/test_coroutine.c)
endif()

set(AI_CPU_KERNEL_TEST_SRCS
    ${CMAKE_SOURCE_DIR}/runtime/ai/kernels/cpu_ops.c
)
if(NASM_AVAILABLE AND
   EXISTS "${CMAKE_SOURCE_DIR}/runtime/ai/kernels/cpu_avx2.asm")
    list(APPEND AI_CPU_KERNEL_TEST_SRCS
        ${CMAKE_SOURCE_DIR}/runtime/ai/kernels/cpu_avx2.asm
    )
endif()

foreach(test_src ${RUNTIME_TEST_SRCS})
    get_filename_component(test_name ${test_src} NAME_WE)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${test_src}")
        add_executable(${test_name} ${test_src})
        target_link_libraries(${test_name} PRIVATE casprix_runtime)
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/runtime
        )
        add_test(NAME ${test_name} COMMAND ${test_name})
    endif()
endforeach()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/test_ai_autograd.c")
    add_executable(test_ai_autograd
        runtime/test_ai_autograd.c
        ${CMAKE_SOURCE_DIR}/runtime/ai/core/tensor.c
        ${CMAKE_SOURCE_DIR}/runtime/ai/core/autograd.c
        ${AI_CPU_KERNEL_TEST_SRCS}
    )
    target_include_directories(test_ai_autograd PRIVATE
        ${CMAKE_SOURCE_DIR}/runtime/ai/core
    )
    target_link_libraries(test_ai_autograd PRIVATE
        casprix_compile_options
        $<$<NOT:$<BOOL:${MSVC}>>:m>
    )
    add_test(NAME test_ai_autograd COMMAND test_ai_autograd)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/test_ai_ml.c")
    add_executable(test_ai_ml
        runtime/test_ai_ml.c
        ${CMAKE_SOURCE_DIR}/runtime/ai/core/tensor.c
        ${CMAKE_SOURCE_DIR}/runtime/ai/distributed/cpu_collective.c
        ${CMAKE_SOURCE_DIR}/runtime/ai/training/training.c
        ${CMAKE_SOURCE_DIR}/runtime/ai/training/optimizer.c
        ${CMAKE_SOURCE_DIR}/runtime/ai/evaluation/metrics.c
        ${CMAKE_SOURCE_DIR}/runtime/ai/classical/algorithms.c
    )
    target_include_directories(test_ai_ml PRIVATE ${CMAKE_SOURCE_DIR}/runtime/ai)
    target_link_libraries(test_ai_ml PRIVATE
        casprix_compile_options
        $<$<NOT:$<BOOL:${MSVC}>>:m>
    )
    add_test(NAME test_ai_ml COMMAND test_ai_ml)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/test_ai_nn.c")
    add_executable(test_ai_nn
        runtime/test_ai_nn.c
        ${CMAKE_SOURCE_DIR}/runtime/ai/core/tensor.c
        ${AI_CPU_KERNEL_TEST_SRCS}
        ${CMAKE_SOURCE_DIR}/runtime/ai/ops/activations.c
        ${CMAKE_SOURCE_DIR}/runtime/ai/ops/losses.c
        ${CMAKE_SOURCE_DIR}/runtime/ai/training/training.c
        ${CMAKE_SOURCE_DIR}/runtime/ai/training/optimizer.c
        ${CMAKE_SOURCE_DIR}/runtime/ai/nn/nn.c
        ${CMAKE_SOURCE_DIR}/runtime/ai/nn/layers/dense.c
    )
    target_include_directories(test_ai_nn PRIVATE ${CMAKE_SOURCE_DIR}/runtime/ai)
    target_link_libraries(test_ai_nn PRIVATE
        casprix_compile_options
        $<$<NOT:$<BOOL:${MSVC}>>:m>
    )
    add_test(NAME test_ai_nn COMMAND test_ai_nn)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/test_mir_validation.c")
    add_executable(test_mir_validation
        runtime/test_mir_validation.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_builder.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_mem2reg.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_printer.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_borrow.c
    )
    target_include_directories(test_mir_validation PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(test_mir_validation PRIVATE
        casprix_compile_options
        $<$<NOT:$<BOOL:${MSVC}>>:m>
    )
    add_test(NAME test_mir_validation COMMAND test_mir_validation)
endif()

find_package(ZLIB QUIET)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/android/test_manifest_gen.c")
    add_executable(android_test_manifest_gen
        android/test_manifest_gen.c
        ${CMAKE_SOURCE_DIR}/tools/apk_builder/manifest.c
    )
    target_include_directories(android_test_manifest_gen PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/apk_builder
    )
    add_test(NAME android_test_manifest_gen COMMAND android_test_manifest_gen)
endif()

# Accessibility bridge (v1b) — real SGNode traversal. Links the a11y bridge
# against the scene-graph runtime + a tiny skia stub; no Android/JNI needed.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/android/test_a11y_bridge.c" AND
   EXISTS "${CMAKE_SOURCE_DIR}/runtime/android/a11y_bridge.c")
    add_executable(android_test_a11y_bridge
        android/test_a11y_bridge.c
        ${CMAKE_SOURCE_DIR}/runtime/android/a11y_bridge.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/scene_graph.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/widgets.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/layout.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/events.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/style.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/animation.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/text.c
        ${CMAKE_CURRENT_SOURCE_DIR}/android/a11y_skia_stub.c
    )
    target_include_directories(android_test_a11y_bridge PRIVATE
        ${CMAKE_SOURCE_DIR}/runtime/skia
        ${CMAKE_SOURCE_DIR}/runtime/android
        ${CMAKE_SOURCE_DIR}/runtime
    )
    target_link_libraries(android_test_a11y_bridge PRIVATE m)
    add_test(NAME android_test_a11y_bridge COMMAND android_test_a11y_bridge)
endif()

# Text-input disabled / error states + their accessibility-bridge reporting.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/android/test_text_input_states.c" AND
   EXISTS "${CMAKE_SOURCE_DIR}/runtime/android/a11y_bridge.c")
    add_executable(android_test_text_input_states
        android/test_text_input_states.c
        ${CMAKE_SOURCE_DIR}/runtime/android/a11y_bridge.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/scene_graph.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/widgets.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/layout.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/events.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/style.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/animation.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/text.c
        ${CMAKE_CURRENT_SOURCE_DIR}/android/a11y_skia_stub.c
    )
    target_include_directories(android_test_text_input_states PRIVATE
        ${CMAKE_SOURCE_DIR}/runtime/skia
        ${CMAKE_SOURCE_DIR}/runtime/android
        ${CMAKE_SOURCE_DIR}/runtime
    )
    target_link_libraries(android_test_text_input_states PRIVATE m)
    add_test(NAME android_test_text_input_states COMMAND android_test_text_input_states)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/android/test_android_runtime.c")
    add_executable(android_test_runtime
        android/test_android_runtime.c
        ${CMAKE_SOURCE_DIR}/runtime/android/android_runtime.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/scene_graph.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/layout.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/events.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/widgets.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/style.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/animation.c
        ${CMAKE_SOURCE_DIR}/runtime/skia/text.c
        ${CMAKE_CURRENT_SOURCE_DIR}/android/a11y_skia_stub.c
    )
    target_include_directories(android_test_runtime PRIVATE
        ${CMAKE_SOURCE_DIR}/runtime/skia
        ${CMAKE_SOURCE_DIR}/runtime/android
        ${CMAKE_SOURCE_DIR}/runtime
    )
    target_link_libraries(android_test_runtime PRIVATE m)
    add_test(NAME android_test_runtime COMMAND android_test_runtime)
endif()

foreach(_android_zip_test test_apk_pack test_zip_writer)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/android/${_android_zip_test}.c")
        add_executable(android_${_android_zip_test}
            android/${_android_zip_test}.c
            ${CMAKE_SOURCE_DIR}/tools/apk_builder/zip_writer.c
        )
        target_include_directories(android_${_android_zip_test} PRIVATE
            ${CMAKE_SOURCE_DIR}/tools/apk_builder
        )
        if(ZLIB_FOUND)
            target_link_libraries(android_${_android_zip_test} PRIVATE ZLIB::ZLIB)
            target_compile_definitions(android_${_android_zip_test} PRIVATE HAVE_ZLIB)
        endif()
        add_test(NAME android_${_android_zip_test} COMMAND android_${_android_zip_test})
    endif()
endforeach()

# ── Benchmarks (built but not CTest cases) ───────────────────────────────────
foreach(_bench bench_simd)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/${_bench}.c")
        add_executable(${_bench} runtime/${_bench}.c)
        target_link_libraries(${_bench} PRIVATE casprix_runtime)
        target_include_directories(${_bench} PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/runtime
        )
    endif()
endforeach()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/bench_http_concurrency.c")
    add_executable(bench_http_concurrency
        runtime/bench_http_concurrency.c
        ${CMAKE_SOURCE_DIR}/examples/ecommerce/backend.c
        ${CMAKE_SOURCE_DIR}/runtime/net/socket.c
        ${CMAKE_SOURCE_DIR}/runtime/net/http.c
        ${CMAKE_SOURCE_DIR}/runtime/net/http_server.c
        ${CMAKE_SOURCE_DIR}/runtime/async/future.c
        ${CMAKE_SOURCE_DIR}/runtime/async/async_runtime.c
        ${CMAKE_SOURCE_DIR}/runtime/async/scheduler.c
        ${CMAKE_SOURCE_DIR}/runtime/sync/lockfree_deque.c
        $<$<BOOL:${WIN32}>:${CMAKE_SOURCE_DIR}/runtime/io/iocp.c>
        $<$<BOOL:${UNIX}>:${CMAKE_SOURCE_DIR}/runtime/io/epoll.c>
    )
    target_link_libraries(bench_http_concurrency PRIVATE
        casprix_compile_options
        $<$<BOOL:${WIN32}>:ws2_32>
        $<$<BOOL:${UNIX}>:pthread>
    )
    target_include_directories(bench_http_concurrency PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/runtime
    )
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/test_async_runtime.c")
    add_executable(test_async_runtime
        runtime/test_async_runtime.c
        ${CMAKE_SOURCE_DIR}/runtime/async/async_runtime.c
        ${CMAKE_SOURCE_DIR}/runtime/async/future.c
        ${CMAKE_SOURCE_DIR}/runtime/async/scheduler.c
        ${CMAKE_SOURCE_DIR}/runtime/concurrent/channel.c
        ${CMAKE_SOURCE_DIR}/runtime/sync/lockfree_deque.c
        $<$<BOOL:${WIN32}>:${CMAKE_SOURCE_DIR}/runtime/io/iocp.c>
        $<$<BOOL:${UNIX}>:${CMAKE_SOURCE_DIR}/runtime/io/epoll.c>
    )
    target_link_libraries(test_async_runtime PRIVATE
        casprix_compile_options
        $<$<BOOL:${WIN32}>:ws2_32>
        $<$<BOOL:${UNIX}>:pthread>
    )
    target_include_directories(test_async_runtime PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME test_async_runtime COMMAND test_async_runtime)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/test_ecommerce_backend.c")
    add_executable(test_ecommerce_backend
        runtime/test_ecommerce_backend.c
        ${CMAKE_SOURCE_DIR}/examples/ecommerce/backend.c
        ${CMAKE_SOURCE_DIR}/runtime/net/socket.c
        ${CMAKE_SOURCE_DIR}/runtime/net/http.c
        ${CMAKE_SOURCE_DIR}/runtime/net/http_server.c
        ${CMAKE_SOURCE_DIR}/runtime/async/future.c
        ${CMAKE_SOURCE_DIR}/runtime/async/async_runtime.c
        ${CMAKE_SOURCE_DIR}/runtime/async/scheduler.c
        ${CMAKE_SOURCE_DIR}/runtime/sync/lockfree_deque.c
        $<$<BOOL:${WIN32}>:${CMAKE_SOURCE_DIR}/runtime/io/iocp.c>
        $<$<BOOL:${UNIX}>:${CMAKE_SOURCE_DIR}/runtime/io/epoll.c>
    )
    target_link_libraries(test_ecommerce_backend PRIVATE
        casprix_compile_options
        $<$<BOOL:${WIN32}>:ws2_32>
        $<$<BOOL:${UNIX}>:pthread>
    )
    target_include_directories(test_ecommerce_backend PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/runtime
    )
    add_test(NAME test_ecommerce_backend COMMAND test_ecommerce_backend)
endif()

# ── Demos (built but not CTest cases) ────────────────────────────────────────
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/demo_http_server.c")
    add_executable(demo_http_server runtime/demo_http_server.c)
    target_link_libraries(demo_http_server PRIVATE casprix_runtime)
    target_include_directories(demo_http_server PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/runtime
    )
endif()

# ── MIR regex compiler test ─────────────────────────────────────────────────
# Links only the MIR subsystem (no lexer/parser/sema) to keep the test focused
# on the regex-to-MIR pipeline. See tests/compiler/test_mir_regex.c.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/compiler/test_mir_regex.c")
    add_executable(test_mir_regex
        compiler/test_mir_regex.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_builder.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_printer.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_opt.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_mem2reg.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_inline.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_async.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_consteval.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_borrow.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_regex.c
    )
    target_include_directories(test_mir_regex PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
    )
    add_test(NAME test_mir_regex COMMAND test_mir_regex)
endif()

# ── MIR SIMD virtualization test ────────────────────────────────────────────
# Exercises VEC_* opcode construction, the simd.c legalizer, and the
# textual output of the x86-64 / AArch64 / scalar backends.  The test
# links only the MIR subsystem (no lexer/parser/sema) plus the opt and
# backend shim so it can run without the full compiler pipeline.
# See tests/compiler/test_simd_virt.c.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/compiler/test_simd_virt.c")
    add_executable(test_simd_virt
        compiler/test_simd_virt.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_builder.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_printer.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_opt.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_mem2reg.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_inline.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_async.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_consteval.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_borrow.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_backend.c
        ${CMAKE_SOURCE_DIR}/src/compiler/opt/simd.c
    )
    target_include_directories(test_simd_virt PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(test_simd_virt PRIVATE
        casprix_compile_options
        casprix_include_dirs
    )
    add_test(NAME test_simd_virt COMMAND test_simd_virt)
endif()

# ── MIR corpus verification (golden .cpx + builder SIMD fixture) ─────────────
if(BUILD_COMPILER AND BUILD_TESTS AND TARGET casprix_compiler)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/ctest/mir_verify")
    add_executable(test_mir_corpus_verify compiler/test_mir_corpus_verify.c)
    target_link_libraries(test_mir_corpus_verify PRIVATE
        casprix_compiler
        casprix_compile_options
        casprix_include_dirs
        $<$<NOT:$<BOOL:${MSVC}>>:m>
    )
    add_test(NAME MIR_VERIFY_corpus COMMAND test_mir_corpus_verify)
    set_tests_properties(MIR_VERIFY_corpus PROPERTIES
        ENVIRONMENT "CASPRIX_SOURCE_ROOT=${CMAKE_SOURCE_DIR};MIR_VERIFY_OUT=${CMAKE_BINARY_DIR}/ctest/mir_verify")
endif()

# ── CVM + JIT tier-up test ───────────────────────────────────────────────────
# Tests the register-based MIR interpreter and the JIT bridge (mmap/VirtualAlloc).
# Links the minimal MIR subsystem (builder + printer + module) plus the new
# runtime/vm sources.  No lexer/parser/sema needed.
if(BUILD_COMPILER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_vm_jit.c")
    add_executable(test_vm_jit
        test_vm_jit.c
        # CVM interpreter + JIT bridge
        ${CMAKE_SOURCE_DIR}/runtime/vm/cvm_engine.c
        ${CMAKE_SOURCE_DIR}/runtime/vm/jit_bridge.c
        ${CMAKE_SOURCE_DIR}/runtime/io/direct_io.c
        ${CMAKE_SOURCE_DIR}/runtime/io/fast_format.c
        ${CMAKE_SOURCE_DIR}/runtime/memory/arc.c
        # MIR subsystem (builder, printer, optimizer, borrow checker)
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_builder.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_printer.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_opt.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_mem2reg.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_inline.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_async.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_consteval.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_borrow.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_backend.c
        ${CMAKE_SOURCE_DIR}/src/compiler/opt/simd.c
    )
    target_include_directories(test_vm_jit PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/runtime
    )
    target_link_libraries(test_vm_jit PRIVATE
        casprix_compile_options
        casprix_include_dirs
        $<$<NOT:$<BOOL:${MSVC}>>:m>
    )
    # Win32 needs no extra libs for VirtualAlloc (already in kernel32)
    add_test(NAME CVM_JIT_tierup COMMAND test_vm_jit)
    set_tests_properties(CVM_JIT_tierup PROPERTIES
        TIMEOUT 120
        LABELS "vm;jit")
endif()

# Ensure unsupported MIR is rejected consistently by capability checks and
# both CVM dispatch configurations.
if(BUILD_COMPILER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/compiler/test_mir_backend_capabilities.c")
    set(_cvm_cap_sources
        compiler/test_mir_backend_capabilities.c
        ${CMAKE_SOURCE_DIR}/runtime/vm/cvm_engine.c
        ${CMAKE_SOURCE_DIR}/runtime/vm/jit_bridge.c
        ${CMAKE_SOURCE_DIR}/runtime/io/direct_io.c
        ${CMAKE_SOURCE_DIR}/runtime/io/fast_format.c
        ${CMAKE_SOURCE_DIR}/runtime/memory/arc.c
        ${CMAKE_SOURCE_DIR}/runtime/memory/cycle_gc.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_builder.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_printer.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_opt.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_mem2reg.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_inline.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_async.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_consteval.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_borrow.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_backend.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_backend_caps.c
        ${CMAKE_SOURCE_DIR}/src/compiler/opt/simd.c)
    foreach(_dispatch IN ITEMS computed_goto switch)
        set(_target test_mir_backend_capabilities_${_dispatch})
        add_executable(${_target} ${_cvm_cap_sources})
        target_include_directories(${_target} PRIVATE
            ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/runtime)
        target_link_libraries(${_target} PRIVATE casprix_compile_options casprix_include_dirs
            $<$<NOT:$<BOOL:${MSVC}>>:m>)
        if(_dispatch STREQUAL "switch")
            target_compile_definitions(${_target} PRIVATE CVM_FORCE_SWITCH=1)
        endif()
        add_test(NAME CVM_backend_capabilities_${_dispatch} COMMAND ${_target})
        set_tests_properties(CVM_backend_capabilities_${_dispatch} PROPERTIES
            TIMEOUT 30 LABELS "vm;capability")
    endforeach()
    set(_cvm_support_sources ${_cvm_cap_sources})
    list(REMOVE_AT _cvm_support_sources 0)
    foreach(_dispatch IN ITEMS computed_goto switch)
        set(_target test_cvm_integer_arithmetic_${_dispatch})
        add_executable(${_target}
            compiler/test_cvm_integer_arithmetic.c
            ${_cvm_support_sources})
        target_include_directories(${_target} PRIVATE
            ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/runtime)
        target_link_libraries(${_target} PRIVATE casprix_compile_options casprix_include_dirs
            $<$<NOT:$<BOOL:${MSVC}>>:m>)
        if(_dispatch STREQUAL "switch")
            target_compile_definitions(${_target} PRIVATE CVM_FORCE_SWITCH=1)
        endif()
        add_test(NAME ${_target} COMMAND ${_target})
        set_tests_properties(${_target} PROPERTIES TIMEOUT 30 LABELS "vm;integer")
    endforeach()
    add_executable(test_cvm_class_metadata
        compiler/test_cvm_class_metadata.c ${_cvm_support_sources})
    target_include_directories(test_cvm_class_metadata PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/runtime)
    target_link_libraries(test_cvm_class_metadata PRIVATE casprix_compile_options
        casprix_include_dirs $<$<NOT:$<BOOL:${MSVC}>>:m>)
    add_test(NAME CVM_class_metadata_identity COMMAND test_cvm_class_metadata)
    set_tests_properties(CVM_class_metadata_identity PROPERTIES
        TIMEOUT 30 LABELS "vm;memory;metadata")
endif()

# ── JIT capacity bail-out test ─────────────────────────────────────────────────
# Verifies jit_function_is_supported() refuses functions that exceed the
# trampoline's argument capacity or the fixed register map, so JIT tier-up
# cannot generate crashing code (unsupported → stays interpreted).
if(BUILD_COMPILER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/compiler/test_jit_caps.c")
    add_executable(test_jit_caps
        compiler/test_jit_caps.c
        ${CMAKE_SOURCE_DIR}/runtime/vm/cvm_engine.c
        ${CMAKE_SOURCE_DIR}/runtime/vm/jit_bridge.c
        ${CMAKE_SOURCE_DIR}/runtime/io/direct_io.c
        ${CMAKE_SOURCE_DIR}/runtime/io/fast_format.c
        ${CMAKE_SOURCE_DIR}/runtime/memory/arc.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_builder.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_printer.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_opt.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_mem2reg.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_inline.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_async.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_consteval.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_borrow.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_backend.c
        ${CMAKE_SOURCE_DIR}/src/compiler/opt/simd.c
    )
    target_include_directories(test_jit_caps PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/runtime
    )
    target_link_libraries(test_jit_caps PRIVATE
        casprix_compile_options
        casprix_include_dirs
        $<$<NOT:$<BOOL:${MSVC}>>:m>
    )
    add_test(NAME JIT_capacity_bailout COMMAND test_jit_caps)
    set_tests_properties(JIT_capacity_bailout PROPERTIES TIMEOUT 60 LABELS "vm;jit")
endif()

if(BUILD_COMPILER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/compiler/test_jit_regalloc.c")
    add_executable(test_jit_regalloc
        compiler/test_jit_regalloc.c
        ${CMAKE_SOURCE_DIR}/runtime/vm/cvm_engine.c
        ${CMAKE_SOURCE_DIR}/runtime/vm/jit_bridge.c
        ${CMAKE_SOURCE_DIR}/runtime/io/direct_io.c
        ${CMAKE_SOURCE_DIR}/runtime/io/fast_format.c
        ${CMAKE_SOURCE_DIR}/runtime/memory/arc.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_builder.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_printer.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_opt.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_mem2reg.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_inline.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_async.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_consteval.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_borrow.c
        ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_backend.c
        ${CMAKE_SOURCE_DIR}/src/compiler/opt/simd.c
    )
    target_include_directories(test_jit_regalloc PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/runtime
    )
    target_link_libraries(test_jit_regalloc PRIVATE
        casprix_compile_options
        casprix_include_dirs
        $<$<NOT:$<BOOL:${MSVC}>>:m>
    )
    add_test(NAME JIT_register_allocator COMMAND test_jit_regalloc)
    set_tests_properties(JIT_register_allocator PROPERTIES TIMEOUT 60 LABELS "vm;jit")
endif()

foreach(_jt test_jit_trampoline test_jit_selfcall)
    if(BUILD_COMPILER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/compiler/${_jt}.c")
        add_executable(${_jt}
            compiler/${_jt}.c
            ${CMAKE_SOURCE_DIR}/runtime/vm/cvm_engine.c
            ${CMAKE_SOURCE_DIR}/runtime/vm/jit_bridge.c
            ${CMAKE_SOURCE_DIR}/runtime/io/direct_io.c
            ${CMAKE_SOURCE_DIR}/runtime/io/fast_format.c
            ${CMAKE_SOURCE_DIR}/runtime/memory/arc.c
            ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir.c
            ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_builder.c
            ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_printer.c
            ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_opt.c
            ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_mem2reg.c
            ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_inline.c
            ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_async.c
            ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_consteval.c
            ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_borrow.c
            ${CMAKE_SOURCE_DIR}/src/compiler/ir/mir_backend.c
            ${CMAKE_SOURCE_DIR}/src/compiler/opt/simd.c
        )
        target_include_directories(${_jt} PRIVATE
            ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/runtime)
        target_link_libraries(${_jt} PRIVATE
            casprix_compile_options casprix_include_dirs
            $<$<NOT:$<BOOL:${MSVC}>>:m>)
    endif()
endforeach()
if(TARGET test_jit_trampoline)
    add_test(NAME JIT_trampoline_spill COMMAND test_jit_trampoline)
    set_tests_properties(JIT_trampoline_spill PROPERTIES TIMEOUT 60 LABELS "vm;jit")
endif()
if(TARGET test_jit_selfcall)
    add_test(NAME JIT_self_recursive_call COMMAND test_jit_selfcall)
    set_tests_properties(JIT_self_recursive_call PROPERTIES TIMEOUT 120 LABELS "vm;jit")
endif()

# ── Compiler integration tests ───────────────────────────────────────────────
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_lang_abi.c")
    add_executable(test_lang_abi test_lang_abi.c)
    target_link_libraries(test_lang_abi PRIVATE casprix_runtime)
    target_include_directories(test_lang_abi PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/runtime
    )
    add_test(NAME test_lang_abi COMMAND test_lang_abi)
endif()

# ── Compiler suite runner (C-based) ─────────────────────────────────────────
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/runner.c" AND BUILD_COMPILER)
    add_executable(compiler_suite_runner runner.c)
    set_target_properties(compiler_suite_runner PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    add_dependencies(compiler_suite_runner casprix)
    add_test(NAME compiler_suite
             COMMAND compiler_suite_runner
             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(compiler_suite PROPERTIES
        ENVIRONMENT "CASPRIX_EXE=$<TARGET_FILE:casprix>")
endif()

# ── CPX source tests (run via compiler) ──────────────────────────────────────
# Use repo root as cwd so pipeline_link finds build/libcasprix_runtime.a (or
# CASPRIX_SOURCE_ROOT for inline runtime .c paths). Unique -o avoids parallel
# tests clobbering each other's .asm/.o in a shared directory.
if(BUILD_COMPILER AND TARGET casprix)
    add_executable(test_frontend_lifecycle
        compiler/test_frontend_lifecycle.c
        ${CMAKE_SOURCE_DIR}/runtime/vm/cvm_engine.c
        ${CMAKE_SOURCE_DIR}/runtime/vm/jit_bridge.c
        ${CMAKE_SOURCE_DIR}/runtime/io/direct_io.c
        ${CMAKE_SOURCE_DIR}/runtime/io/fast_format.c
        ${CMAKE_SOURCE_DIR}/runtime/memory/arc.c
    )
    target_include_directories(test_frontend_lifecycle PRIVATE
        ${CMAKE_SOURCE_DIR}/runtime
    )
    target_link_libraries(test_frontend_lifecycle PRIVATE
        casprix_compiler
        casprix_compile_options
        casprix_include_dirs
        $<$<NOT:$<BOOL:${MSVC}>>:m>
        $<$<NOT:$<BOOL:${MSVC}>>:pthread>
    )
    add_test(NAME compiler_frontend_lifecycle
        COMMAND test_frontend_lifecycle
            ${CMAKE_CURRENT_SOURCE_DIR}/compiler/fixture_cvm_scalar_semantics.cpx
            ${CMAKE_CURRENT_SOURCE_DIR}/compiler/test_async.cpx
            ${CMAKE_CURRENT_SOURCE_DIR}/compiler/test_double_free.cpx
            ${CMAKE_CURRENT_SOURCE_DIR}/compiler/test_for_in.cpx
            ${CMAKE_CURRENT_SOURCE_DIR}/compiler/test_import_alias.cpx
            ${CMAKE_CURRENT_SOURCE_DIR}/compiler/fixture_frontend_parse_error.cpx
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(compiler_frontend_lifecycle PROPERTIES
        LABELS "compiler;frontend;lifecycle")

    file(GLOB CPX_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/compiler/test_*.cpx")

    foreach(cpx_test ${CPX_TESTS})
        get_filename_component(test_name ${cpx_test} NAME_WE)
        add_test(
            NAME compiler_${test_name}
            COMMAND $<TARGET_FILE:casprix> ${cpx_test} -o ${CMAKE_BINARY_DIR}/ctest/${test_name}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )
        if(BUILD_RUNTIME AND TARGET casprix_runtime)
            set_tests_properties(compiler_${test_name} PROPERTIES
                ENVIRONMENT "CASPRIX_RUNTIME_LIB=$<TARGET_FILE:casprix_runtime>;CASPRIX_SOURCE_ROOT=${CMAKE_SOURCE_DIR}")
        else()
            set_tests_properties(compiler_${test_name} PROPERTIES
                ENVIRONMENT "CASPRIX_SOURCE_ROOT=${CMAKE_SOURCE_DIR}")
        endif()
    endforeach()

    add_test(
        NAME compiler_c_entrypoint_linkage
        COMMAND ${CMAKE_COMMAND}
            -DCASPRIX_EXE=$<TARGET_FILE:casprix>
            -DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/compiler/fixture_c_entrypoint.cpx
            -DOUTPUT_BASE=${CMAKE_BINARY_DIR}/ctest/c_entrypoint
            -P ${CMAKE_CURRENT_SOURCE_DIR}/compiler/check_c_entrypoint.cmake
    )

    add_test(
        NAME compiler_c_backend_high_level
        COMMAND ${CMAKE_COMMAND}
            -DCASPRIX_EXE=$<TARGET_FILE:casprix>
            -DC_COMPILER=${CMAKE_C_COMPILER}
            -DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/compiler/fixture_c_backend_high_level.cpx
            -DOUTPUT_BASE=${CMAKE_BINARY_DIR}/ctest/c_backend_high_level
            -P ${CMAKE_CURRENT_SOURCE_DIR}/compiler/check_c_backend_high_level.cmake
    )

    # O0 mem2reg regression: dead undefined reaching-definition seeds must not
    # reject ordinary scalar CVM execution, while results remain full-width.
    add_test(
        NAME CVM_o0_scalar_semantics
        COMMAND ${CMAKE_COMMAND}
            -DCASPRIX_EXE=$<TARGET_FILE:casprix>
            -DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/compiler/fixture_cvm_scalar_semantics.cpx
            -P ${CMAKE_CURRENT_SOURCE_DIR}/compiler/check_cvm_scalar_semantics.cmake
    )

    add_test(
        NAME compiler_native_integer_semantics
        COMMAND ${CMAKE_COMMAND}
            -DCASPRIX_EXE=$<TARGET_FILE:casprix>
            -DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/compiler/fixture_native_integer_semantics.cpx
            -DINVALID_SOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/compiler/fixture_native_integer_divzero.cpx
            -DPRINT_SHIM=${CMAKE_CURRENT_SOURCE_DIR}/compiler/integer_semantics_print_shim.c
            -DC_COMPILER=${CMAKE_C_COMPILER}
            -DSANITIZER_FLAGS=${CASPRIX_SANITIZER_FLAGS}
            -DHOST_WINDOWS=${WIN32}
            -DOUTPUT_BASE=${CMAKE_BINARY_DIR}/ctest/native_integer_semantics
            -DWORKING_DIRECTORY=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/compiler/check_native_integer_semantics.cmake
    )
    if(BUILD_RUNTIME AND TARGET casprix_runtime)
        set_tests_properties(compiler_native_integer_semantics PROPERTIES
            ENVIRONMENT "CASPRIX_RUNTIME_LIB=$<TARGET_FILE:casprix_runtime>;CASPRIX_SOURCE_ROOT=${CMAKE_SOURCE_DIR}")
    else()
        set_tests_properties(compiler_native_integer_semantics PROPERTIES
            ENVIRONMENT "CASPRIX_SOURCE_ROOT=${CMAKE_SOURCE_DIR}")
    endif()
    set_tests_properties(compiler_native_integer_semantics PROPERTIES
        TIMEOUT 600 LABELS "compiler;integer;backend")

    set(_native_failure_runtime_arg "-DRUNTIME_LIB=")
    if(BUILD_RUNTIME AND TARGET casprix_runtime)
        set(_native_failure_runtime_arg "-DRUNTIME_LIB=$<TARGET_FILE:casprix_runtime>")
    endif()
    add_test(NAME compiler_native_link_failure_integrity
        COMMAND ${CMAKE_COMMAND}
            -DCASPRIX_EXE=$<TARGET_FILE:casprix>
            -DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/compiler/fixture_native_link_failure.cpx
            -DOUTPUT_BASE=${CMAKE_BINARY_DIR}/ctest/native_sanitizer_failure/output
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -DHOST_WINDOWS=${WIN32}
            -DSANITIZER_CONFIG=${CASPRIX_SANITIZERS}
            ${_native_failure_runtime_arg}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/compiler/check_native_link_failure.cmake
    )
    set_tests_properties(compiler_native_link_failure_integrity PROPERTIES
        TIMEOUT 120 LABELS "compiler;toolchain;native;failure")

    # Permanent negative: conditional move then use after join (use-after-move).
    # Fixture is named fixture_*.cpx so the positive GLOB and suite runner skip it.
    add_test(
        NAME compiler_neg_conditional_use_after_move
        COMMAND ${CMAKE_COMMAND}
            -DCASPRIX_EXE=$<TARGET_FILE:casprix>
            -DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/compiler/fixture_conditional_use_after_move.cpx
            -DEXPECT_REGEX=Use of moved value
            -P ${CMAKE_CURRENT_SOURCE_DIR}/compiler/check_ownership_negative.cmake
    )

    # Disable tests that require the Skia GUI framework when it is not built
    if(NOT ENABLE_SKIA_GUI)
        set(SKIA_TESTS
            test_wt_direct test_tabpanel test_tabpanel2 test_tabpanel3
            test_method_call test_pb
        )
        foreach(_t ${SKIA_TESTS})
            if(TEST compiler_${_t})
                set_tests_properties(compiler_${_t} PROPERTIES DISABLED TRUE)
            endif()
        endforeach()
    endif()
endif()

# ── Semantic / borrow / type-system conformance (fixture-gated) ───────────────
# Register only when helpers and source fixtures exist. Permanent CMake must not
# point at missing files. When local fixtures under tests/borrow,
# tests/type_system, and tests/semantic are present, the historical phase1_*
# names are preserved. When absent (clean/staged export), the tests are omitted
# rather than registered broken.
if(BUILD_COMPILER AND TARGET casprix)
    set(_cpx_sem_pos_helper "${CMAKE_CURRENT_SOURCE_DIR}/semantic/check_positive.cmake")
    set(_cpx_sem_neg_helper "${CMAKE_CURRENT_SOURCE_DIR}/semantic/check_negative.cmake")

    macro(casprix_phase1_neg name relpath regex)
        if(EXISTS "${_cpx_sem_neg_helper}" AND
           EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${relpath}")
            add_test(
                NAME phase1_neg_${name}
                COMMAND ${CMAKE_COMMAND}
                    -DCASPRIX_EXE=$<TARGET_FILE:casprix>
                    -DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/${relpath}
                    -DEXPECT_REGEX=${regex}
                    -DEXTRA_ARGS=${ARGN}
                    -P ${_cpx_sem_neg_helper}
            )
            set_tests_properties(phase1_neg_${name} PROPERTIES LABELS "local_semantic")
        else()
            message(STATUS "  Omitting phase1_neg_${name}: missing fixture or helper")
        endif()
    endmacro()
    macro(casprix_phase1_pos name relpath)
        if(EXISTS "${_cpx_sem_pos_helper}" AND
           EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${relpath}")
            add_test(
                NAME phase1_pos_${name}
                COMMAND ${CMAKE_COMMAND}
                    -DCASPRIX_EXE=$<TARGET_FILE:casprix>
                    -DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/${relpath}
                    -DEXTRA_ARGS=${ARGN}
                    -P ${_cpx_sem_pos_helper}
            )
            set_tests_properties(phase1_pos_${name} PROPERTIES LABELS "local_semantic")
        else()
            message(STATUS "  Omitting phase1_pos_${name}: missing fixture or helper")
        endif()
    endmacro()

    casprix_phase1_neg(mut_borrow_conflict
        borrow/test_mut_borrow_conflict.cpx "mutably while borrowed" --safe-mode)
    casprix_phase1_neg(mut_imm_conflict
        borrow/test_mut_imm_conflict.cpx "while mutably borrowed" --safe-mode)
    casprix_phase1_neg(narrowing_rejected
        type_system/test_narrowing_rejected.cpx "Type mismatch")
    casprix_phase1_neg(signed_unsigned_rejected
        type_system/test_signed_unsigned_rejected.cpx "Type mismatch")

    casprix_phase1_pos(mut_borrow_ok borrow/test_mut_borrow_ok.cpx --safe-mode)
    casprix_phase1_pos(shared_borrows_ok borrow/test_shared_borrows_ok.cpx --safe-mode)
    casprix_phase1_pos(sequential_mut_borrows
        borrow/test_sequential_mut_borrows.cpx --safe-mode)
    casprix_phase1_pos(widening_ok type_system/test_widening_ok.cpx)

    # DEF-010: --safe-mode --check-only must reject conflicting borrows
    casprix_phase1_neg(def010_safe_mode_check_only
        borrow/test_mut_borrow_conflict.cpx "mutably while borrowed" --safe-mode)
endif()

# ── CTest configuration ───────────────────────────────────────────────────────
# Release builds define NDEBUG globally. Unit tests depend on assert(), so
# explicitly keep assertions active for every executable created here.
get_property(CASPRIX_TEST_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_test_target IN LISTS CASPRIX_TEST_TARGETS)
    if(MSVC)
        target_compile_options(${_test_target} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${_test_target} PRIVATE -UNDEBUG)
    endif()
endforeach()

set_property(DIRECTORY PROPERTY TEST_LAUNCH_DIR "${CMAKE_SOURCE_DIR}")

if(BUILD_TESTS)
    add_executable(test_integer_semantics compiler/test_integer_semantics.c)
    target_include_directories(test_integer_semantics PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_integer_semantics PRIVATE casprix_compile_options)
    add_test(NAME integer_semantics_primitives COMMAND test_integer_semantics)
    set_tests_properties(integer_semantics_primitives PROPERTIES LABELS "integer;compiler")

    add_executable(test_integer_optimizer compiler/test_integer_optimizer.c)
    target_include_directories(test_integer_optimizer PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(test_integer_optimizer PRIVATE casprix_compiler casprix_compile_options)
    add_test(NAME integer_ast_optimizer COMMAND test_integer_optimizer)
    set_tests_properties(integer_ast_optimizer PROPERTIES LABELS "integer;compiler;optimizer")
endif()

if(BUILD_COMPILER AND BUILD_TESTS AND TARGET casprix)
    add_test(NAME compiler_integer_literal_ranges
        COMMAND ${CMAKE_COMMAND}
            -DCASPRIX_EXE=$<TARGET_FILE:casprix>
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/ctest/integer_literal_ranges
            -P ${CMAKE_CURRENT_SOURCE_DIR}/compiler/check_integer_literal_ranges.cmake)
    set_tests_properties(compiler_integer_literal_ranges PROPERTIES
        TIMEOUT 60 LABELS "compiler;semantic;integer")

    add_test(NAME compiler_unsupported_integer_widths
        COMMAND ${CMAKE_COMMAND}
            -DCASPRIX_EXE=$<TARGET_FILE:casprix>
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/ctest/unsupported_integer_widths
            -P ${CMAKE_CURRENT_SOURCE_DIR}/compiler/check_unsupported_integer_width.cmake)
    set_tests_properties(compiler_unsupported_integer_widths PROPERTIES
        TIMEOUT 60 LABELS "compiler;semantic;integer")

    add_executable(test_mir_lower_unsupported_width compiler/test_mir_lower_unsupported_width.c)
    target_include_directories(test_mir_lower_unsupported_width PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(test_mir_lower_unsupported_width PRIVATE
        casprix_compiler casprix_compile_options)
    add_test(NAME mir_lower_unsupported_integer_width
        COMMAND test_mir_lower_unsupported_width)
    set_tests_properties(mir_lower_unsupported_integer_width PROPERTIES
        LABELS "compiler;integer;mir")
endif()

if(BUILD_TESTS AND CASPRIX_SANITIZERS)
    add_test(NAME compiler_sanitizer_detection_controls
        COMMAND ${CMAKE_COMMAND}
            -DC_COMPILER=${CMAKE_C_COMPILER}
            -DSANITIZER_CONFIG=${CASPRIX_SANITIZERS}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/compiler
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/ctest/sanitizer_controls
            -P ${CMAKE_CURRENT_SOURCE_DIR}/compiler/check_sanitizer_controls.cmake)
    set_tests_properties(compiler_sanitizer_detection_controls PROPERTIES
        TIMEOUT 120 LABELS "compiler;toolchain;sanitizer")
endif()

message(STATUS "  Test suite:    ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "    Runtime tests:  registered with CTest")
message(STATUS "    Compiler tests: runner.c + CPX test files")

# CMake limits TEST properties to tests declared in the current directory.
# Apply the sanitizer runtime settings here, after platform-conditional test
# registration has completed.
if(CASPRIX_SANITIZERS)
    get_property(_casprix_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_test_target IN LISTS _casprix_test_targets)
        get_target_property(_test_target_type "${_test_target}" TYPE)
        if(_test_target_type STREQUAL "EXECUTABLE")
            target_link_libraries("${_test_target}" PRIVATE casprix_compile_options)
        endif()
    endforeach()

    get_property(_casprix_registered_tests DIRECTORY PROPERTY TESTS)
    foreach(_test IN LISTS _casprix_registered_tests)
        set_property(TEST "${_test}" APPEND PROPERTY ENVIRONMENT
            "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1"
            "UBSAN_OPTIONS=halt_on_error=1")
    endforeach()
endif()
