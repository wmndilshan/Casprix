if(NOT DEFINED CASPRIX_EXE OR NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "CASPRIX_EXE, SOURCE_DIR and OUTPUT_DIR are required")
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

set(fixtures
    fixture_unsupported_i128_local.cpx
    fixture_unsupported_u128_parameter.cpx
    fixture_unsupported_i128_struct.cpx
    fixture_unsupported_i128_global.cpx
    fixture_unsupported_i128_return.cpx
    fixture_unsupported_i128_class_field.cpx
    fixture_unsupported_i128_array.cpx
    fixture_unsupported_i128_nested_pointer.cpx
    fixture_unsupported_i128_import.cpx)
foreach(fixture IN LISTS fixtures)
    set(source "${SOURCE_DIR}/tests/compiler/${fixture}")
    foreach(mode IN ITEMS check safe_check mir_check mir_dump execute emit_c native)
        set(output "${OUTPUT_DIR}/${fixture}_${mode}")
        if(mode STREQUAL "check")
            execute_process(COMMAND "${CASPRIX_EXE}" --check-only "${source}"
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 30)
        elseif(mode STREQUAL "safe_check")
            execute_process(COMMAND "${CASPRIX_EXE}" --safe-mode --check-only "${source}"
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 30)
        elseif(mode STREQUAL "mir_check")
            execute_process(COMMAND "${CASPRIX_EXE}" --mir --check-only "${source}"
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 30)
        elseif(mode STREQUAL "mir_dump")
            execute_process(COMMAND "${CASPRIX_EXE}" --mir -o "${output}" "${source}"
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 30)
        elseif(mode STREQUAL "execute")
            execute_process(COMMAND "${CASPRIX_EXE}" --execute "${source}"
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 30)
        elseif(mode STREQUAL "emit_c")
            execute_process(COMMAND "${CASPRIX_EXE}" --emit-c -o "${output}" "${source}"
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 30)
        else()
            execute_process(COMMAND "${CASPRIX_EXE}" -o "${output}" "${source}"
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 30)
        endif()
        if(NOT result EQUAL 65)
            message(FATAL_ERROR "${fixture} ${mode}: expected semantic exit 65, got ${result}\n${err}\n${out}")
        endif()
        if(NOT "${err}${out}" MATCHES "Unsupported integer type '(i128|u128)'")
            message(FATAL_ERROR "${fixture} ${mode}: missing unsupported-width diagnostic\n${err}\n${out}")
        endif()
        if(EXISTS "${output}" OR EXISTS "${output}.exe" OR EXISTS "${output}.c")
            message(FATAL_ERROR "${fixture} ${mode}: emitted an artifact after rejection")
        endif()
    endforeach()
endforeach()

set(sentinel "${OUTPUT_DIR}/preserve-me.exe")
file(WRITE "${sentinel}" "user sentinel\n")
execute_process(COMMAND "${CASPRIX_EXE}" -o "${sentinel}"
    "${SOURCE_DIR}/tests/compiler/fixture_unsupported_i128_local.cpx"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 30)
if(NOT result EQUAL 65)
    message(FATAL_ERROR "sentinel compilation: expected exit 65, got ${result}\n${err}")
endif()
file(READ "${sentinel}" contents)
if(NOT contents STREQUAL "user sentinel\n")
    message(FATAL_ERROR "existing output was modified after semantic rejection")
endif()
