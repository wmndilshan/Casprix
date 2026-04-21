# Casprix Test Suite
cmake_minimum_required(VERSION 3.16)

# ── Runtime unit tests ──────────────────────────────────────────────────────
set(RUNTIME_TEST_SRCS
    runtime/test_stdlib.c
    runtime/test_scheduler.c
    runtime/test_networking.c
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
