# Casprix Test Suite
cmake_minimum_required(VERSION 3.16)

# ── Runtime unit tests ──────────────────────────────────────────────────────
set(RUNTIME_TEST_SRCS
    runtime/test_stdlib.c
    runtime/test_coroutine.c
    runtime/test_scheduler.c
    runtime/test_networking.c
    runtime/benchmark.c
)

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
find_program(CASPRIX_EXE casprix HINTS ${CMAKE_BINARY_DIR} ${CMAKE_BINARY_DIR}/build)
if(CASPRIX_EXE)
    file(GLOB CPX_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/compiler/test_*.cpx")
    foreach(cpx_test ${CPX_TESTS})
        get_filename_component(test_name ${cpx_test} NAME_WE)
        add_test(
            NAME compiler_${test_name}
            COMMAND ${CASPRIX_EXE} ${cpx_test}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/compiler
        )
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
