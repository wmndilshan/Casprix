# ============================================================================
# ARCHIVED — this file is NOT included by any active build.
#
# net2/ was an experimental reactor + stackful-coroutine async networking
# rewrite. It was archived (not deleted) because it has zero consumers,
# unfinished protocol support, a known cx_runtime.c build break, and a
# double-wake race in its scheduler. See ./README.md for details.
#
# runtime/net/ is the actively-maintained networking implementation.
#
# The target definitions below are kept verbatim, commented out, so whoever
# revisits this can see the original wiring. To build it again: fix the
# breaks documented in README.md, then from a parent CMakeLists add
#   add_subdirectory(${CMAKE_SOURCE_DIR}/archive/net2)
# and un-comment the block below.
# ============================================================================

# cmake_minimum_required(VERSION 3.16)
#
# set(NET2_SOURCES
#     coro_posix2.c
#     cx_task.c
#     cx_runtime.c
#     cx_io.c
#     cx_task_arc.c
#     cx_worker.c
# )
#
# add_library(cx_net2 STATIC ${NET2_SOURCES})
#
# target_include_directories(cx_net2 PUBLIC
#     ${CMAKE_CURRENT_SOURCE_DIR}
#     ${CMAKE_CURRENT_SOURCE_DIR}/..
#     ${CMAKE_SOURCE_DIR}/include
# )
#
# target_link_libraries(cx_net2 PUBLIC
#     casprix_runtime
#     pthread
# )
#
# # Test executable
# add_executable(test_net2 test_net2.c)
# target_link_libraries(test_net2 PRIVATE
#     cx_net2
#     casprix_runtime
#     pthread
# )
#
# if(UNIX)
#     target_compile_definitions(cx_net2 PRIVATE _GNU_SOURCE)
# endif()
#
# # Guard coro_posix2.c behind UNIX platform check
# if(NOT UNIX)
#     list(REMOVE_ITEM NET2_SOURCES coro_posix2.c)
# endif()
