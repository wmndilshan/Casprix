cmake_minimum_required(VERSION 3.16)

set(NET2_SOURCES
    coro_posix2.c
    cx_task.c
    cx_runtime.c
    cx_io.c
    cx_task_arc.c
    cx_worker.c
)

add_library(cx_net2 STATIC ${NET2_SOURCES})

target_include_directories(cx_net2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(cx_net2 PUBLIC
    casprix_runtime
    pthread
)

# Test executable
add_executable(test_net2 test_net2.c)
target_link_libraries(test_net2 PRIVATE
    cx_net2
    casprix_runtime
    pthread
)

if(UNIX)
    target_compile_definitions(cx_net2 PRIVATE _GNU_SOURCE)
    # target_compile_options(cx_net2 PRIVATE -fsanitize=address)
    # target_link_options(cx_net2 PUBLIC -fsanitize=address)
    # target_compile_options(test_net2 PRIVATE -fsanitize=address)
    # target_link_options(test_net2 PRIVATE -fsanitize=address)
endif()

# Guard coro_posix2.c behind UNIX platform check
if(NOT UNIX)
    list(REMOVE_ITEM NET2_SOURCES coro_posix2.c)
    # On Windows, we'd use coroutine_win32.c or similar if needed
endif()
