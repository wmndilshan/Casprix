if(NOT DEFINED C_COMPILER OR NOT DEFINED SANITIZER_CONFIG OR
   NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "C_COMPILER, SANITIZER_CONFIG, SOURCE_DIR and OUTPUT_DIR are required")
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

function(build_and_expect source sanitizer expected_text run_env)
    get_filename_component(name "${source}" NAME_WE)
    set(exe "${OUTPUT_DIR}/${name}")
    execute_process(COMMAND "${C_COMPILER}" -O0 "-fsanitize=${sanitizer}"
        -fno-omit-frame-pointer "${source}" -o "${exe}"
        RESULT_VARIABLE compile_result OUTPUT_VARIABLE compile_out ERROR_VARIABLE compile_err
        TIMEOUT 60)
    if(NOT compile_result EQUAL 0)
        message(FATAL_ERROR "Could not build ${name} sanitizer control (${compile_result}):\n${compile_err}\n${compile_out}")
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E env ${run_env} "${exe}"
        RESULT_VARIABLE run_result OUTPUT_VARIABLE run_out ERROR_VARIABLE run_err TIMEOUT 30)
    if(run_result EQUAL 0)
        message(FATAL_ERROR "${name} unexpectedly passed; requested sanitizer did not report the control defect")
    endif()
    if(NOT "${run_out}${run_err}" MATCHES "${expected_text}")
        message(FATAL_ERROR "${name} failed without expected ${expected_text} diagnostic:\n${run_err}\n${run_out}")
    endif()
endfunction()

if(SANITIZER_CONFIG MATCHES "address")
    build_and_expect("${SOURCE_DIR}/sanitizer_control_asan.c" "address"
        "AddressSanitizer" "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1")
    build_and_expect("${SOURCE_DIR}/sanitizer_control_lsan.c" "address"
        "LeakSanitizer|detected memory leaks" "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1")
endif()
if(SANITIZER_CONFIG MATCHES "undefined")
    build_and_expect("${SOURCE_DIR}/sanitizer_control_ubsan.c" "undefined"
        "runtime error: signed integer overflow" "UBSAN_OPTIONS=halt_on_error=1")
endif()
