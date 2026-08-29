# Casprix Test Suite
cmake_minimum_required(VERSION 3.16)

# ── Runtime unit tests ──────────────────────────────────────────────────────
set(RUNTIME_TEST_SRCS
    runtime/test_stdlib.c
    runtime/test_scheduler.c
    runtime/test_networking.c
    runtime/test_net_hardening.c
    runtime/test_arc_gc.c
    runtime/benchmark.c
)

if(WIN32)
    list(APPEND RUNTIME_TEST_SRCS runtime/test_coroutine.c)
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
    file(GLOB CPX_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/compiler/test_*.cpx")

    # Semantic-only fixtures: these import a lib/ module that binds to an
    # external C runtime not shipped in this repo (e.g. lib/nn/nn.cpx -> the
    # NUWAN LLM runtime). A full compile passes semantic analysis but cannot
    # link, so verify them with --check-only, matching the compiler_suite
    # runner (runner.c) which already uses --check-only for every fixture.
    set(CPX_CHECK_ONLY_TESTS test_nn_syntax)

    foreach(cpx_test ${CPX_TESTS})
        get_filename_component(test_name ${cpx_test} NAME_WE)
        if(test_name IN_LIST CPX_CHECK_ONLY_TESTS)
            add_test(
                NAME compiler_${test_name}
                COMMAND $<TARGET_FILE:casprix> --check-only ${cpx_test}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            )
        else()
            add_test(
                NAME compiler_${test_name}
                COMMAND $<TARGET_FILE:casprix> ${cpx_test} -o ${CMAKE_BINARY_DIR}/ctest/${test_name}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            )
        endif()
        if(BUILD_RUNTIME AND TARGET casprix_runtime)
            set_tests_properties(compiler_${test_name} PROPERTIES
                ENVIRONMENT "CASPRIX_RUNTIME_LIB=$<TARGET_FILE:casprix_runtime>;CASPRIX_SOURCE_ROOT=${CMAKE_SOURCE_DIR}")
        else()
            set_tests_properties(compiler_${test_name} PROPERTIES
                ENVIRONMENT "CASPRIX_SOURCE_ROOT=${CMAKE_SOURCE_DIR}")
        endif()
    endforeach()

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

# ── CTest configuration ───────────────────────────────────────────────────────
set_property(DIRECTORY PROPERTY TEST_LAUNCH_DIR "${CMAKE_SOURCE_DIR}")

message(STATUS "  Test suite:    ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "    Runtime tests:  registered with CTest")
message(STATUS "    Compiler tests: runner.c + CPX test files")
