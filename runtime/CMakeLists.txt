# Casprix Runtime - standalone CMakeLists
# Can be included from the root CMakeLists or built independently
cmake_minimum_required(VERSION 3.16)

option(ENABLE_SKIA_GUI "Prefer the full Skia backend; OFF still builds the fallback GDI-backed skia_gui target" OFF)
option(USE_SKIA_BACKEND "Use real Skia C++ backend (requires vendored Skia libraries)" OFF)

if(NOT TARGET casprix_runtime)
    project(casprix_runtime C)
endif()

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

set(SKIA_GUI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/skia/scene_graph.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/skia/layout.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/skia/events.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/skia/widgets.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/skia/charts.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/skia/effects.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/skia/shapes.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/skia/style.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/skia/text.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/skia/animation.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/skia/frame_loop.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/skia/skia_window_win32.c"
)
if(ANDROID AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/skia/frame_loop_android.c")
    list(APPEND SKIA_GUI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/skia/frame_loop_android.c")
endif()

set(SKIA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/skia")
if(ENABLE_SKIA_GUI AND USE_SKIA_BACKEND AND EXISTS "${SKIA_DIR}/lib/skia.lib")
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    add_library(skia_c STATIC "${CMAKE_CURRENT_SOURCE_DIR}/skia/skia_c.cpp")
    target_include_directories(skia_c PRIVATE
        "${SKIA_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/skia"
    )
    target_link_libraries(skia_c PRIVATE "${SKIA_DIR}/lib/skia.lib")
else()
    add_library(skia_c STATIC "${CMAKE_CURRENT_SOURCE_DIR}/skia/skia_c_gdi.c")
    target_include_directories(skia_c PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/skia"
    )
endif()

add_library(skia_gui STATIC ${SKIA_GUI_SOURCES})
target_include_directories(skia_gui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/skia"
    "${CMAKE_CURRENT_SOURCE_DIR}/android"
)
target_link_libraries(skia_gui PUBLIC
    skia_c
    $<$<BOOL:${WIN32}>:user32 gdi32 ole32 oleaut32>
)

if(TARGET casprix_runtime)
    # Already defined by parent CMakeLists - just add sources and link the GUI stack
    target_sources(casprix_runtime PRIVATE ${RUNTIME_CORE_SRCS})
    target_include_directories(casprix_runtime PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/skia
        ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(casprix_runtime PUBLIC skia_gui)
else()
    # Standalone build
    add_library(casprix_runtime STATIC ${RUNTIME_CORE_SRCS} ${RUNTIME_AI_SRCS})
    target_include_directories(casprix_runtime PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/skia
        ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(casprix_runtime PUBLIC skia_gui)
    set_target_properties(casprix_runtime PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
    )
endif()

# net2/ archived to archive/net2/ — removed from the active build.
# See archive/net2/README.md. To revisit: add_subdirectory(${CMAKE_SOURCE_DIR}/archive/net2)
