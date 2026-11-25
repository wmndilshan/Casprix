# Permanent negative ownership / semantic check helper.
# Requires: CASPRIX_EXE, SOURCE_FILE, EXPECT_REGEX
# Optional: EXTRA_ARGS (semicolon- or space-separated list via -D),
# EXPECT_EXIT_CODE (defaults to 65)
#
# Passes only when:
#   - the compiler exits non-zero, and
#   - combined stdout/stderr matches EXPECT_REGEX.
# Crashes alone without the diagnostic do not satisfy the regex match.

if(NOT CASPRIX_EXE OR NOT SOURCE_FILE OR NOT EXPECT_REGEX)
    message(FATAL_ERROR "CASPRIX_EXE, SOURCE_FILE, and EXPECT_REGEX are required")
endif()

if(NOT EXISTS "${SOURCE_FILE}")
    message(FATAL_ERROR "SOURCE_FILE does not exist: ${SOURCE_FILE}")
endif()

set(_args ${EXTRA_ARGS})
execute_process(
    COMMAND "${CASPRIX_EXE}" ${_args} --check-only "${SOURCE_FILE}"
    TIMEOUT 15
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
)
set(_combined "${_out}\n${_err}")

if(NOT "${_rc}" MATCHES "^-?[0-9]+$")
    message(FATAL_ERROR "Compiler failed to complete (${_rc}) for ${SOURCE_FILE}.\nOutput:\n${_combined}")
endif()

if(_rc EQUAL 0)
    message(FATAL_ERROR
        "Expected ownership/semantic rejection for ${SOURCE_FILE}, but exit was 0.\n"
        "Output:\n${_combined}")
endif()

if(NOT DEFINED EXPECT_EXIT_CODE)
    set(EXPECT_EXIT_CODE 65)
endif()
if(NOT "${_rc}" STREQUAL "${EXPECT_EXIT_CODE}")
    message(FATAL_ERROR "Expected compiler exit ${EXPECT_EXIT_CODE}, got ${_rc} for ${SOURCE_FILE}.\nOutput:\n${_combined}")
endif()

# Reject obvious infrastructure failures that are not the intended diagnostic.
if(_combined MATCHES "Could not open file" OR
   _combined MATCHES "not found" OR _combined MATCHES "Segmentation")
    message(FATAL_ERROR
        "Negative test failed for environmental/crash reasons (exit ${_rc}), "
        "not an ownership diagnostic.\nOutput:\n${_combined}")
endif()

string(REGEX MATCH "${EXPECT_REGEX}" _matched "${_combined}")
if(NOT _matched)
    message(FATAL_ERROR
        "Expected diagnostic matching /${EXPECT_REGEX}/ for ${SOURCE_FILE}.\n"
        "Exit=${_rc}\nOutput:\n${_combined}")
endif()

message(STATUS "OK negative ownership test ${SOURCE_FILE} (exit ${_rc})")
