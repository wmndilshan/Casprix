if(NOT DEFINED CASPRIX_EXE OR NOT DEFINED SOURCE_FILE OR NOT DEFINED OUTPUT_BASE OR
   NOT DEFINED SOURCE_ROOT OR NOT DEFINED HOST_WINDOWS)
    message(FATAL_ERROR "CASPRIX_EXE, SOURCE_FILE, OUTPUT_BASE, SOURCE_ROOT and HOST_WINDOWS are required")
endif()
function(run_failing_link label backend runtime_lib expected_text)
    set(base "${OUTPUT_BASE}_${label}")
    if(HOST_WINDOWS)
        set(final "${base}.exe")
    else()
        set(final "${base}")
    endif()
    set(sentinel "Casprix existing output sentinel ${label}\n")
    file(WRITE "${final}" "${sentinel}")
    if(backend STREQUAL "mir")
        set(backend_flag --native)
    else()
        set(backend_flag --legacy-backend)
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E env
        "CASPRIX_CC=${CASPRIX_EXE}"
        "CASPRIX_RUNTIME_LIB=${runtime_lib}"
        "CASPRIX_SOURCE_ROOT=${SOURCE_ROOT}"
        "${CASPRIX_EXE}" ${backend_flag} -O0 -o "${base}" "${SOURCE_FILE}"
        WORKING_DIRECTORY "${SOURCE_ROOT}"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 60)
    if(result EQUAL 0)
        message(FATAL_ERROR "${label}: external tool failure unexpectedly produced success")
    endif()
    file(READ "${final}" actual)
    if(NOT actual STREQUAL sentinel)
        message(FATAL_ERROR "${label}: failed compilation modified the pre-existing executable")
    endif()
    file(GLOB leftovers "${base}.casprix-tmp-*")
    if(leftovers)
        message(FATAL_ERROR "${label}: failed compilation left temporary executable(s): ${leftovers}")
    endif()
    if(NOT "${out}${err}" MATCHES "${expected_text}")
        message(FATAL_ERROR "${label}: missing expected diagnostic '${expected_text}':\n${err}\n${out}")
    endif()
endfunction()

get_filename_component(OUTPUT_BASE_DIR "${OUTPUT_BASE}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTPUT_BASE_DIR}")
run_failing_link("mir_external_failure" "mir" "${RUNTIME_LIB}" "failed|capability error")
run_failing_link("legacy_external_failure" "legacy" "${RUNTIME_LIB}" "failed|capability error")

if(DEFINED SANITIZER_CONFIG AND NOT SANITIZER_CONFIG STREQUAL "")
    set(wrong_archive "${OUTPUT_BASE}_wrong_runtime.a")
    file(WRITE "${wrong_archive}" "not linked: metadata rejection expected\n")
    file(WRITE "${wrong_archive}.casprix-sanitizers"
        "casprix-runtime-v1\nsanitizers=deliberately-wrong\nabi=wrong-target\n")
    run_failing_link("mismatched_runtime" "mir" "${wrong_archive}" "runtime compatibility error")
endif()
