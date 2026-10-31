if(NOT DEFINED CASPRIX_EXE OR NOT DEFINED SOURCE_FILE OR NOT DEFINED OUTPUT_BASE)
    message(FATAL_ERROR "CASPRIX_EXE, SOURCE_FILE, and OUTPUT_BASE are required")
endif()

execute_process(
    COMMAND "${CASPRIX_EXE}" --emit-c -k -o "${OUTPUT_BASE}" "${SOURCE_FILE}"
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr
)
if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR "C emission failed:\n${compile_stdout}\n${compile_stderr}")
endif()

set(generated_c "${OUTPUT_BASE}.c")
if(NOT EXISTS "${generated_c}")
    message(FATAL_ERROR "Expected generated C file: ${generated_c}")
endif()

file(READ "${generated_c}" generated_source)
if(generated_source MATCHES "static[ \t\r\n]+int32_t[ \t\r\n]+main")
    message(FATAL_ERROR "Generated main incorrectly has internal linkage")
endif()
if(NOT generated_source MATCHES "int32_t[ \t\r\n]+main\\(void\\)")
    message(FATAL_ERROR "Generated C does not contain an externally linked main(void)")
endif()
