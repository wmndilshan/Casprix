if(NOT DEFINED CASPRIX_EXE OR NOT DEFINED SOURCE_FILE OR
   NOT DEFINED C_COMPILER OR NOT DEFINED OUTPUT_BASE)
    message(FATAL_ERROR "CASPRIX_EXE, SOURCE_FILE, C_COMPILER, OUTPUT_BASE are required")
endif()

set(expected [=[-12
-12
12
-12
-1
1
-85
9223372036854775807
5
18446744073709551615
true
9223372036854775807
-4
44
-128
0
-32768
0
-2147483648
0
-9223372036854775808
0
-128
24464
-4
64
-128
65535
-12
-1
6553
5
127
144
true
true
-300
60000
45
30
5
1
-9223372036854775808
18446744073709551615]=])
get_filename_component(output_dir "${OUTPUT_BASE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
set(native_executable_suffix "")
if(DEFINED HOST_WINDOWS AND HOST_WINDOWS)
    set(native_executable_suffix ".exe")
elseif(WIN32)
    set(native_executable_suffix ".exe")
endif()
set(sanitizer_flags)
if(DEFINED SANITIZER_FLAGS AND NOT SANITIZER_FLAGS STREQUAL "")
    separate_arguments(sanitizer_flags NATIVE_COMMAND "${SANITIZER_FLAGS}")
endif()

foreach(level IN ITEMS 0 2)
    foreach(backend IN ITEMS legacy cvm generated_c mir_native)
        set(base "${OUTPUT_BASE}_${backend}_o${level}")
        if(backend STREQUAL "legacy")
            execute_process(
                COMMAND "${CASPRIX_EXE}" --opt-level=${level} -o "${base}" "${SOURCE_FILE}"
                WORKING_DIRECTORY "${WORKING_DIRECTORY}"
                RESULT_VARIABLE compile_result OUTPUT_VARIABLE compile_out ERROR_VARIABLE compile_err
                TIMEOUT 30
            )
            if(NOT compile_result EQUAL 0)
                message(FATAL_ERROR "Legacy O${level} compilation failed (${compile_result}):\n${compile_err}\n${compile_out}")
            endif()
            set(run_path "${base}${native_executable_suffix}")
        elseif(backend STREQUAL "cvm")
            execute_process(
                COMMAND "${CASPRIX_EXE}" --execute --opt-level=${level} "${SOURCE_FILE}"
                WORKING_DIRECTORY "${WORKING_DIRECTORY}"
                RESULT_VARIABLE run_result OUTPUT_VARIABLE actual ERROR_VARIABLE run_err
                TIMEOUT 30
            )
            if(NOT run_result EQUAL 0)
                message(FATAL_ERROR "CVM O${level} failed (${run_result}):\n${run_err}\n${actual}")
            endif()
        elseif(backend STREQUAL "generated_c")
            execute_process(
                COMMAND "${CASPRIX_EXE}" --emit-c --opt-level=${level} -o "${base}" "${SOURCE_FILE}"
                WORKING_DIRECTORY "${WORKING_DIRECTORY}"
                RESULT_VARIABLE emit_result OUTPUT_VARIABLE emit_out ERROR_VARIABLE emit_err
                TIMEOUT 30
            )
            if(NOT emit_result EQUAL 0)
                message(FATAL_ERROR "Generated C O${level} emission failed (${emit_result}):\n${emit_err}\n${emit_out}")
            endif()
            execute_process(
                COMMAND "${C_COMPILER}" -O${level} "${base}.c"
                        ${sanitizer_flags}
                        "${PRINT_SHIM}" -o "${base}.exe"
                WORKING_DIRECTORY "${WORKING_DIRECTORY}"
                RESULT_VARIABLE compile_result OUTPUT_VARIABLE compile_out ERROR_VARIABLE compile_err
                TIMEOUT 30
            )
            if(NOT compile_result EQUAL 0)
                message(FATAL_ERROR "Generated C O${level} compilation failed (${compile_result}):\n${compile_err}\n${compile_out}")
            endif()
            set(run_path "${base}.exe")
        elseif(backend STREQUAL "mir_native")
            execute_process(
                COMMAND "${CASPRIX_EXE}" --native --opt-level=${level} -o "${base}" "${SOURCE_FILE}"
                WORKING_DIRECTORY "${WORKING_DIRECTORY}"
                RESULT_VARIABLE compile_result OUTPUT_VARIABLE compile_out ERROR_VARIABLE compile_err
                TIMEOUT 30
            )
            if(NOT compile_result EQUAL 0)
                message(FATAL_ERROR "C-mediated MIR-native O${level} compilation failed (${compile_result}):\n${compile_err}\n${compile_out}")
            endif()
            set(run_path "${base}${native_executable_suffix}")
        endif()

        if(NOT backend STREQUAL "cvm")
            execute_process(
                COMMAND "${run_path}"
                WORKING_DIRECTORY "${WORKING_DIRECTORY}"
                RESULT_VARIABLE run_result OUTPUT_VARIABLE actual ERROR_VARIABLE run_err
                TIMEOUT 30
            )
            if(NOT run_result EQUAL 0)
                message(FATAL_ERROR "${backend} O${level} execution failed (${run_result}):\n${run_err}\n${actual}")
            endif()
        endif()

        string(REPLACE "\r\n" "\n" actual "${actual}")
        string(REGEX REPLACE "\n[ \t]*Prepared MIR:.*$" "" actual "${actual}")
        string(REPLACE "True" "true" actual "${actual}")
        string(REPLACE "False" "false" actual "${actual}")
        string(STRIP "${actual}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "${backend} O${level} output mismatch.\nExpected:\n${expected}\nActual:\n${actual}")
        endif()
    endforeach()
endforeach()

if(DEFINED INVALID_SOURCE_FILE)
    set(invalid_cases division_zero remainder_zero signed_div_overflow signed_mod_overflow
        negative_shift shift_at_width shift_above_width unsigned_division_zero
        unsigned_remainder_zero unused_division zero_product_division narrow_shift_at_width
        large_shift unused_remainder unused_shift nested_division control_flow_division)
    foreach(invalid_case IN LISTS invalid_cases)
        if(invalid_case STREQUAL "division_zero")
            set(invalid_source "${INVALID_SOURCE_FILE}")
        elseif(invalid_case STREQUAL "remainder_zero")
            set(invalid_text [=[func op(a: i64, b: i64) -> i64 { return a % b }
func main() -> i64 { print(op(10i64, 0i64)); return 0 }
]=])
        elseif(invalid_case STREQUAL "signed_div_overflow")
            set(invalid_text [=[func minval() -> i64 { return -9223372036854775807i64 - 1i64 }
func op(a: i64, b: i64) -> i64 { return a / b }
func main() -> i64 { print(op(minval(), -1i64)); return 0 }
]=])
        elseif(invalid_case STREQUAL "signed_mod_overflow")
            set(invalid_text [=[func minval() -> i64 { return -9223372036854775807i64 - 1i64 }
func op(a: i64, b: i64) -> i64 { return a % b }
func main() -> i64 { print(op(minval(), -1i64)); return 0 }
]=])
        elseif(invalid_case STREQUAL "negative_shift")
            set(invalid_text [=[func op(a: i64, n: i64) -> i64 { return a << n }
func main() -> i64 { print(op(1i64, -1i64)); return 0 }
]=])
        elseif(invalid_case STREQUAL "shift_at_width")
            set(invalid_text [=[func op(a: i64, n: i64) -> i64 { return a >> n }
func main() -> i64 { print(op(1i64, 64i64)); return 0 }
]=])
        elseif(invalid_case STREQUAL "shift_above_width")
            set(invalid_text [=[func op(a: i64, n: i64) -> i64 { return a << n }
func main() -> i64 { print(op(1i64, 65i64)); return 0 }
]=])
        elseif(invalid_case STREQUAL "unsigned_division_zero")
            set(invalid_text [=[func op(a: u64, b: u64) -> u64 { return a / b }
func main() -> i64 { print(op(10u64, 0u64)); return 0 }
]=])
        elseif(invalid_case STREQUAL "unsigned_remainder_zero")
            set(invalid_text [=[func op(a: u64, b: u64) -> u64 { return a % b }
func main() -> i64 { print(op(10u64, 0u64)); return 0 }
]=])
        elseif(invalid_case STREQUAL "unused_division")
            set(invalid_text [=[func op(a: i64, b: i64) -> i64 { let ignored: i64 = a / b; print(1); return 0 }
func main() -> i64 { print(op(1i64, 0i64)); return 0 }
]=])
        elseif(invalid_case STREQUAL "zero_product_division")
            set(invalid_text [=[func op(a: i64, b: i64) -> i64 { return a / b }
func main() -> i64 { print(op(1i64, 0i64) * 0i64); return 0 }
]=])
        elseif(invalid_case STREQUAL "narrow_shift_at_width")
            set(invalid_text [=[func op(a: i8, n: i8) -> i8 { return a << n }
func main() -> i64 { print(op(1i8, 8i8)); return 0 }
]=])
        elseif(invalid_case STREQUAL "large_shift")
            set(invalid_text [=[func max_u64() -> u64 { return 0u64 - 1u64 }
func op(a: u64, n: u64) -> u64 { return a >> n }
func main() -> i64 { print(op(1u64, max_u64())); return 0 }
]=])
        elseif(invalid_case STREQUAL "unused_remainder")
            set(invalid_text [=[func op(a: i64, b: i64) -> i64 { let ignored: i64 = a % b; print(1); return 0 }
func main() -> i64 { print(op(1i64, 0i64)); return 0 }
]=])
        elseif(invalid_case STREQUAL "unused_shift")
            set(invalid_text [=[func op(a: i64, n: i64) -> i64 { let ignored: i64 = a << n; print(1); return 0 }
func main() -> i64 { print(op(1i64, 64i64)); return 0 }
]=])
        elseif(invalid_case STREQUAL "nested_division")
            set(invalid_text [=[func inner(a: i64, b: i64) -> i64 { return a / b }
func outer(a: i64, b: i64) -> i64 { return inner(a, b) }
func main() -> i64 { print(outer(1i64, 0i64)); return 0 }
]=])
        elseif(invalid_case STREQUAL "control_flow_division")
            set(invalid_text [=[func op(a: i64, b: i64) -> i64 { return a / b }
func main() -> i64 { mut enter: bool = true; if enter { print(op(1i64, 0i64)) } return 0 }
]=])
        endif()
        if(NOT invalid_case STREQUAL "division_zero")
            set(invalid_source "${OUTPUT_BASE}_invalid_${invalid_case}.cpx")
            file(WRITE "${invalid_source}" "${invalid_text}")
        endif()
    foreach(level IN ITEMS 0 2)
        foreach(backend IN ITEMS legacy cvm generated_c mir_native)
            set(base "${OUTPUT_BASE}_invalid_${invalid_case}_${backend}_o${level}")
            if(backend STREQUAL "legacy")
                execute_process(COMMAND "${CASPRIX_EXE}" --opt-level=${level} -o "${base}" "${invalid_source}"
                    WORKING_DIRECTORY "${WORKING_DIRECTORY}" RESULT_VARIABLE compile_result
                    OUTPUT_VARIABLE compile_out ERROR_VARIABLE compile_err TIMEOUT 30)
                if(NOT compile_result EQUAL 0)
                    message(FATAL_ERROR "Legacy invalid O${level} compile failed: ${compile_err}${compile_out}")
                endif()
                set(run_path "${base}${native_executable_suffix}")
            elseif(backend STREQUAL "cvm")
                execute_process(COMMAND "${CASPRIX_EXE}" --execute --opt-level=${level} "${invalid_source}"
                    WORKING_DIRECTORY "${WORKING_DIRECTORY}" RESULT_VARIABLE run_result
                    OUTPUT_VARIABLE run_out ERROR_VARIABLE run_err TIMEOUT 30)
            elseif(backend STREQUAL "generated_c")
                execute_process(COMMAND "${CASPRIX_EXE}" --emit-c --opt-level=${level} -o "${base}" "${invalid_source}"
                    WORKING_DIRECTORY "${WORKING_DIRECTORY}" RESULT_VARIABLE emit_result
                    OUTPUT_VARIABLE emit_out ERROR_VARIABLE emit_err TIMEOUT 30)
                if(NOT emit_result EQUAL 0)
                    message(FATAL_ERROR "Generated C invalid O${level} emission failed: ${emit_err}${emit_out}")
                endif()
                # Sanitizer flags instrument the emitted C as well as its final link.
                execute_process(COMMAND "${C_COMPILER}" -O${level} "${base}.c"
                    ${sanitizer_flags} "${PRINT_SHIM}" -o "${base}.exe"
                    WORKING_DIRECTORY "${WORKING_DIRECTORY}" RESULT_VARIABLE compile_result
                    OUTPUT_VARIABLE compile_out ERROR_VARIABLE compile_err TIMEOUT 30)
                if(NOT compile_result EQUAL 0)
                    message(FATAL_ERROR "Generated C invalid O${level} compile failed: ${compile_err}${compile_out}")
                endif()
                set(run_path "${base}.exe")
            else()
                execute_process(COMMAND "${CASPRIX_EXE}" --native --opt-level=${level} -o "${base}" "${invalid_source}"
                    WORKING_DIRECTORY "${WORKING_DIRECTORY}" RESULT_VARIABLE compile_result
                    OUTPUT_VARIABLE compile_out ERROR_VARIABLE compile_err TIMEOUT 30)
                if(NOT compile_result EQUAL 0)
                    message(FATAL_ERROR "MIR-native invalid O${level} compile failed: ${compile_err}${compile_out}")
                endif()
                set(run_path "${base}${native_executable_suffix}")
            endif()

            if(NOT backend STREQUAL "cvm")
                execute_process(COMMAND "${run_path}" WORKING_DIRECTORY "${WORKING_DIRECTORY}"
                    RESULT_VARIABLE run_result OUTPUT_VARIABLE run_out ERROR_VARIABLE run_err TIMEOUT 30)
            endif()
            if(run_result EQUAL 0)
                message(FATAL_ERROR "${backend} O${level} unexpectedly succeeded for division by zero")
            endif()
            set(failure_text "${run_err}\n${run_out}")
            if(backend STREQUAL "cvm")
                if(invalid_case STREQUAL "remainder_zero" OR invalid_case STREQUAL "unsigned_remainder_zero" OR invalid_case STREQUAL "unused_remainder")
                    set(expected_trap_code 11)
                elseif(invalid_case STREQUAL "signed_div_overflow" OR invalid_case STREQUAL "signed_mod_overflow")
                    set(expected_trap_code 12)
                elseif(invalid_case MATCHES "shift")
                    set(expected_trap_code 13)
                else()
                    set(expected_trap_code 10)
                endif()
                if(NOT failure_text MATCHES "program trapped \\(code ${expected_trap_code}\\)")
                    message(FATAL_ERROR "CVM O${level} reported the wrong trap for ${invalid_case}: ${failure_text}")
                endif()
            else()
                if(invalid_case STREQUAL "remainder_zero" OR invalid_case STREQUAL "unsigned_remainder_zero" OR invalid_case STREQUAL "unused_remainder")
                    set(expected_failure "Casprix arithmetic failure: remainder by zero")
                elseif(invalid_case STREQUAL "signed_div_overflow" OR invalid_case STREQUAL "signed_mod_overflow")
                    set(expected_failure "Casprix arithmetic failure: signed division overflow")
                elseif(invalid_case MATCHES "shift")
                    set(expected_failure "Casprix arithmetic failure: invalid shift count")
                else()
                    set(expected_failure "Casprix arithmetic failure: division by zero")
                endif()
                if(NOT failure_text MATCHES "${expected_failure}")
                    message(FATAL_ERROR "${backend} O${level} ${invalid_case} lacked '${expected_failure}': ${failure_text}")
                endif()
            endif()
        endforeach()
    endforeach()
    endforeach()
endif()
