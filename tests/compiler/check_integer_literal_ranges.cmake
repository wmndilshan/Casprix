if(NOT DEFINED CASPRIX_EXE OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "CASPRIX_EXE and OUTPUT_DIR are required")
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

set(cases
    "u64_overflow|func main() -> i64 { let value: u64 = 18446744073709551616u64@ return 0 }|Integer literal exceeds the 64-bit range"
    "positive_i64_overflow|func main() -> i64 { let value: i64 = 9223372036854775808i64@ return 0 }|Integer literal is out of range for its declared type"
    "positive_i8_overflow|func main() -> i64 { let value: i8 = 128i8@ return 0 }|Integer literal is out of range for its declared type"
    "u8_overflow|func main() -> i64 { let value: u8 = 256u8@ return 0 }|Integer literal is out of range for its declared type")

foreach(item IN LISTS cases)
    string(REPLACE "|" ";" fields "${item}")
    list(GET fields 0 case_name)
    list(GET fields 1 source_text)
    list(GET fields 2 diagnostic)
    string(REPLACE "@" ";" source_text "${source_text}")
    set(source "${OUTPUT_DIR}/${case_name}.cpx")
    file(WRITE "${source}" "${source_text}\n")
    execute_process(COMMAND "${CASPRIX_EXE}" --check-only "${source}"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 30)
    if(NOT result EQUAL 65)
        message(FATAL_ERROR "${case_name}: expected rejection status 65, got ${result}\n${err}\n${out}")
    endif()
    if(NOT "${out}${err}" MATCHES "${diagnostic}")
        message(FATAL_ERROR "${case_name}: missing expected range diagnostic '${diagnostic}'\n${err}\n${out}")
    endif()
endforeach()
