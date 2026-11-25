foreach(level IN ITEMS 0 2)
    execute_process(
        COMMAND "${CASPRIX_EXE}" --execute --opt-level=${level} "${SOURCE_FILE}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 30
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "CVM scalar fixture failed at O${level} (status ${result}): ${error}\n${output}")
    endif()
    foreach(expected IN ITEMS 1234567890132 42 96 64 45 34 720 117)
        string(FIND "${output}" "${expected}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "CVM scalar fixture at O${level} omitted full-width result ${expected}:\n${output}")
        endif()
    endforeach()
endforeach()
