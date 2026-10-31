if(NOT DEFINED CASPRIX_EXE OR NOT DEFINED C_COMPILER OR
   NOT DEFINED SOURCE_FILE OR NOT DEFINED OUTPUT_BASE)
    message(FATAL_ERROR "CASPRIX_EXE, C_COMPILER, SOURCE_FILE, and OUTPUT_BASE are required")
endif()

execute_process(
    COMMAND "${CASPRIX_EXE}" "${SOURCE_FILE}" --emit-c --c-entry cpx_test_main --output "${OUTPUT_BASE}"
    RESULT_VARIABLE emit_result
    OUTPUT_VARIABLE emit_stdout
    ERROR_VARIABLE emit_stderr
)
if(NOT emit_result EQUAL 0)
    message(FATAL_ERROR "C emission failed:\n${emit_stdout}\n${emit_stderr}")
endif()

set(generated_c "${OUTPUT_BASE}.c")
if(NOT EXISTS "${generated_c}")
    message(FATAL_ERROR "Expected generated C file: ${generated_c}")
endif()

file(READ "${generated_c}" generated_source)
if(NOT generated_source MATCHES "Flag_Flag__ctor0")
    message(FATAL_ERROR "First overloaded constructor has no unique C symbol")
endif()
if(NOT generated_source MATCHES "Flag_Flag__ctor1")
    message(FATAL_ERROR "Second overloaded constructor has no unique C symbol")
endif()
if(NOT generated_source MATCHES "void[ \t\r\n]+cpx_test_main\\(void\\* context\\)")
    message(FATAL_ERROR "Requested library entry point was not emitted")
endif()

execute_process(
    COMMAND "${C_COMPILER}" -std=c11 -fsyntax-only "${generated_c}"
    RESULT_VARIABLE c_result
    OUTPUT_VARIABLE c_stdout
    ERROR_VARIABLE c_stderr
)
if(NOT c_result EQUAL 0)
    message(FATAL_ERROR "Generated C is invalid:\n${c_stdout}\n${c_stderr}")
endif()
