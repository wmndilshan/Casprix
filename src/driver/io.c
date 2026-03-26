/**
 * io.c - File I/O utilities for the Casprix compiler driver
 */
#include "driver/io.h"
#include "driver/cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "support/log.h"

/* ============================================================================
 * Source file reading
 * ========================================================================== */

char* driver_read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        CPX_LOG(CPX_LOG_ERROR, CPX_LOG_CAT_IO, "Could not open file: %s", path);
        exit(74);
    }

    fseek(file, 0L, SEEK_END);
    size_t file_size = (size_t)ftell(file);
    rewind(file);

    char* buffer = (char*)malloc(file_size + 1);
    if (!buffer) {
        CPX_LOG(CPX_LOG_ERROR, CPX_LOG_CAT_MEMORY, "Not enough memory to read: %s", path);
        fclose(file);
        exit(74);
    }

    size_t bytes_read = fread(buffer, sizeof(char), file_size, file);
    if (bytes_read < file_size) {
        CPX_LOG(CPX_LOG_ERROR, CPX_LOG_CAT_IO, "Could not read file: %s", path);
        free(buffer);
        fclose(file);
        exit(74);
    }

    buffer[bytes_read] = '\0';
    fclose(file);
    return buffer;
}

/* ============================================================================
 * Output path helpers
 * ========================================================================== */

char* driver_get_output_name(const char* input_path) {
    if (g_config.output_name) {
        return strdup(g_config.output_name);
    }

    const char* last_slash = strrchr(input_path, '/');
    if (!last_slash) last_slash = strrchr(input_path, '\\');
    const char* base = last_slash ? last_slash + 1 : input_path;

    size_t len   = strlen(base);
    char*  name  = (char*)malloc(len + 5);
    memcpy(name, base, len);
    name[len] = '\0';

    if (len > 4 && strcmp(base + len - 4, ".cpx") == 0) {
        name[len - 4] = '\0';
    } else if (len > 3 && strcmp(base + len - 3, ".nd") == 0) {
        name[len - 3] = '\0';
    }
    return name;
}

const char* driver_get_output_extension(void) {
    switch (g_config.output_kind) {
        case OUTPUT_VM:     return ".cpxbc";
        case OUTPUT_JIT:    return ".cpxjit";
        case OUTPUT_C:      return ".c";
        case OUTPUT_NATIVE:
        default:            return ".asm";
    }
}

char* driver_get_output_file_path(const char* output_name) {
    const char* ext    = driver_get_output_extension();
    char*       result = (char*)malloc(strlen(output_name) + strlen(ext) + 1);
    sprintf(result, "%s%s", output_name, ext);
    return result;
}

const char* driver_selected_output_name(void) {
    switch (g_config.output_kind) {
        case OUTPUT_VM:     return "VM bytecode";
        case OUTPUT_JIT:    return "JIT package";
        case OUTPUT_C:      return "C source";
        case OUTPUT_NATIVE:
        default:
            return (g_config.native_codegen == NATIVE_CODEGEN_MIR &&
                    !g_config.allow_legacy_backend_fallback)
                   ? "native assembly (MIR backend)"
                   : "native assembly (legacy backend)";
    }
}
