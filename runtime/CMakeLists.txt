# Casprix Runtime — standalone CMakeLists
# Can be included from the root CMakeLists or built independently
cmake_minimum_required(VERSION 3.16)

# Collect all runtime C sources (excluding ai submodules handled separately)
file(GLOB_RECURSE RUNTIME_CORE_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/memory/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/async/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/concurrent/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/thread/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sync/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/io/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/file/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/net/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/math/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/binding/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/object.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime.c"
)

file(GLOB_RECURSE RUNTIME_AI_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/ai/llm/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ai/nn/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ai/ml/*.c"
)

if(TARGET casprix_runtime)
    # Already defined by parent CMakeLists — just add sources
    target_sources(casprix_runtime PRIVATE ${RUNTIME_CORE_SRCS})
else()
    # Standalone build
    project(casprix_runtime C)
    add_library(casprix_runtime STATIC ${RUNTIME_CORE_SRCS} ${RUNTIME_AI_SRCS})
    target_include_directories(casprix_runtime PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
    )
    set_target_properties(casprix_runtime PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
    )
endif()
