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
44]=])
get_filename_component(output_dir "${OUTPUT_BASE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")

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
            set(run_path "${base}.exe")
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
            set(run_path "${base}.exe")
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
