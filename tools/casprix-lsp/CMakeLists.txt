# ============================================================================
#  casprix-lsp — Language Server (v1: diagnostics + symbols + go-to-definition)
# ============================================================================
#
#  Only built when BUILD_LSP is ON (default: same as BUILD_COMPILER) because it
#  links the casprix_compiler static library.

if(NOT TARGET casprix_compiler)
    message(WARNING "casprix-lsp: casprix_compiler target not available; skipping")
    return()
endif()

add_executable(casprix-lsp
    main.c
    jsonrpc.c
    document.c
    analysis.c
    ast_query.c
    lsp_server.c
    ${CMAKE_SOURCE_DIR}/third_party/cJSON/cJSON.c
)

target_include_directories(casprix-lsp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/cJSON
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(casprix-lsp PRIVATE
    casprix_compiler
    casprix_compile_options
    casprix_include_dirs
    $<$<NOT:$<BOOL:${MSVC}>>:m>
)

# cJSON is third-party C — do not apply -Werror-ish project options to it.
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/third_party/cJSON/cJSON.c
    PROPERTIES COMPILE_OPTIONS "-w"
)

set_target_properties(casprix-lsp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

install(TARGETS casprix-lsp DESTINATION bin)
