# pkg/CMakeLists.txt — Casprix Package Manager
# Can be used as a standalone build or included from the root CMakeLists.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CASPRIX_PKG_STANDALONE)
    # Included from parent — targets are already defined
    return()
endif()

# Standalone build
project(casprix-pkg VERSION 1.0.0 LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(casprix-pkg
    core/registry.c
    core/resolver.c
    core/manifest.c
    core/cache.c
    core/installer.c
    core/publisher.c
    core/semver.c
    cli/main.c
    cli/cli_complete.c
)

target_include_directories(casprix-pkg PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}       # pkg/  (for cli/cli.h etc.)
    ${CMAKE_CURRENT_SOURCE_DIR}/core  # pkg/core/ (manifest.h, registry.h, …)
    ${CMAKE_CURRENT_SOURCE_DIR}/..    # project root
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

if(NOT MSVC)
    target_link_libraries(casprix-pkg PRIVATE m)
endif()

install(TARGETS casprix-pkg DESTINATION bin)
