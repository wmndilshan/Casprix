/*
 * Casprix Compiler - Module System Implementation
 */

#include "util/module.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/lexer.h"
#include "compiler/sema/semantic.h"
#include "support/error.h"
#include "support/log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void init_module_registry(ModuleRegistry* registry) {
    registry->modules = NULL;
    registry->count = 0;
    registry->capacity = 0;
}

void free_module_registry(ModuleRegistry* registry) {
    for (int i = 0; i < registry->count; i++) {
        Module* mod = &registry->modules[i];
        free(mod->name);
        free(mod->path);

        // Free statements
        for (int j = 0; j < mod->stmt_count; j++) {
            if (mod->statements[j]) {
                free_stmt(mod->statements[j]);
            }
        }
        free(mod->statements);
    }
    free(registry->modules);
}

static char* read_file_content(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }

    fseek(file, 0L, SEEK_END);
    size_t file_size = ftell(file);
    rewind(file);

    char* buffer = (char*)malloc(file_size + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(buffer, sizeof(char), file_size, file);
    buffer[bytes_read] = '\0';
    fclose(file);

    return buffer;
}

static char* resolve_module_path(const char* module_name) {
    char path_buffer[1024];

    // 1. Try the module name directly as a path (.cpx first, .nd for backward compat)
    snprintf(path_buffer, sizeof(path_buffer), "%s.cpx", module_name);
    FILE* test = fopen(path_buffer, "r");
    if (test) { fclose(test); return strdup(path_buffer); }

    snprintf(path_buffer, sizeof(path_buffer), "%s.nd", module_name);
    test = fopen(path_buffer, "r");
    if (test) { fclose(test); return strdup(path_buffer); }

    // 2. Extract basename for fallback searches
    const char* basename = module_name;
    const char* last_slash = strrchr(module_name, '/');
    if (!last_slash) last_slash = strrchr(module_name, '\\');
    if (last_slash) basename = last_slash + 1;

    // 3. Check CASPRIX_STDLIB / NUWAN_STDLIB environment variable
    const char* stdlib_env = getenv("CASPRIX_STDLIB");
    if (!stdlib_env) stdlib_env = getenv("NUWAN_STDLIB");
    if (stdlib_env) {
        snprintf(path_buffer, sizeof(path_buffer), "%s/%s.cpx", stdlib_env, basename);
        test = fopen(path_buffer, "r");
        if (test) { fclose(test); return strdup(path_buffer); }

        snprintf(path_buffer, sizeof(path_buffer), "%s/%s/%s.cpx", stdlib_env, basename, basename);
        test = fopen(path_buffer, "r");
        if (test) { fclose(test); return strdup(path_buffer); }
    }

    // 4. Check CASPRIX_LIB / NUWAN_LIB environment variable
    const char* lib_env = getenv("CASPRIX_LIB");
    if (!lib_env) lib_env = getenv("NUWAN_LIB");
    if (lib_env) {
        snprintf(path_buffer, sizeof(path_buffer), "%s/%s.cpx", lib_env, basename);
        test = fopen(path_buffer, "r");
        if (test) { fclose(test); return strdup(path_buffer); }

        snprintf(path_buffer, sizeof(path_buffer), "%s/%s/%s.cpx", lib_env, basename, basename);
        test = fopen(path_buffer, "r");
        if (test) { fclose(test); return strdup(path_buffer); }
    }

    // 5. Fallback to hardcoded relative paths (using basename)
    const char* paths[] = {
        "stdlib/%s.cpx",
        "stdlib/%s/%s.cpx",
        "lib/%s.cpx",
        "lib/%s/%s.cpx",
        "%s.cpx",
        "stdlib/%s.nd",
        "stdlib/%s/%s.nd",
        "lib/%s.nd",
        "lib/%s/%s.nd",
        "%s.nd",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        snprintf(path_buffer, sizeof(path_buffer), paths[i], basename, basename);
        test = fopen(path_buffer, "r");
        if (test) {
            fclose(test);
            return strdup(path_buffer);
        }
    }

    return NULL;
}

Module* load_module(ModuleRegistry* registry, const char* module_name, void* unused) {
    (void)unused;  // Deprecated parameter

    // Debug: print the module name we're trying to load
    if (module_name) {
        // Print first 50 chars and hex dump of first 20 bytes for debugging
        char hex_dump[61] = {0};
        int len = (int)strlen(module_name);
        int dump_len = len < 20 ? len : 20;
        for (int i = 0; i < dump_len; i++) {
            sprintf(hex_dump + i * 3, "%02x ", (unsigned char)module_name[i]);
        }
        CPX_DEBUG(CPX_LOG_CAT_PARSER, "Attempting to load module: '%s' (length: %d, hex: %s)",
                 module_name, len, hex_dump);
    } else {
        CPX_ERROR(CPX_LOG_CAT_PARSER, "Module name is NULL");
        return NULL;
    }

    // Check if already loaded
    for (int i = 0; i < registry->count; i++) {
        if (strcmp(registry->modules[i].name, module_name) == 0) {
            CPX_DEBUG(CPX_LOG_CAT_PARSER, "Module '%s' already loaded", module_name);
            return &registry->modules[i];
        }
    }

    // Resolve module path
    char* module_path = resolve_module_path(module_name);
    if (!module_path) {
        CPX_ERROR(CPX_LOG_CAT_PARSER, "Module '%s' not found", module_name);
        return NULL;
    }

    CPX_INFO(CPX_LOG_CAT_PARSER, "Loading module: %s from %s", module_name, module_path);

    // Read file
    char* source = read_file_content(module_path);
    if (!source) {
        CPX_ERROR(CPX_LOG_CAT_PARSER, "Could not read module file: %s", module_path);
        free(module_path);
        return NULL;
    }

    // Parse module
    Lexer lexer;
    init_lexer(&lexer, source);

    Parser parser;
    init_parser(&parser, &lexer);

    int stmt_count;
    Stmt** statements = parse(&parser, &stmt_count);

    if (had_error) {
        CPX_ERROR(CPX_LOG_CAT_PARSER, "Failed to parse module: %s", module_name);
        free(source);
        free(module_path);
        return NULL;
    }

    // Grow registry if needed
    if (registry->count >= registry->capacity) {
        registry->capacity = registry->capacity == 0 ? 8 : registry->capacity * 2;
        registry->modules = GROW_ARRAY(Module, registry->modules,
                                      registry->count, registry->capacity);
    }

    // Add module to registry
    Module* mod = &registry->modules[registry->count++];
    mod->name = strdup(module_name);
    mod->path = module_path;
    mod->statements = statements;
    mod->stmt_count = stmt_count;
    mod->loaded = true;

    free(source);

    CPX_INFO(CPX_LOG_CAT_PARSER, "Module '%s' loaded successfully (%d statements)",
             module_name, stmt_count);

    // Recursively load any imports within this module
    for (int i = 0; i < stmt_count; i++) {
        if (statements[i] && statements[i]->type == STMT_INCLUDE) {
            IncludeStmt* incl = &statements[i]->as.include;
            if (incl->module_name) {
                load_module(registry, incl->module_name, NULL);
            }
        }
    }

    return mod;
}

Module* get_module(ModuleRegistry* registry, const char* name) {
    for (int i = 0; i < registry->count; i++) {
        if (strcmp(registry->modules[i].name, name) == 0) {
            return &registry->modules[i];
        }
    }
    return NULL;
}

bool resolve_modules(ModuleRegistry* registry, Stmt** statements, int count, void* unused) {
    (void)registry;
    (void)statements;
    (void)count;
    (void)unused;  // Deprecated parameter

    // TODO: Implement module dependency resolution
    // For now, just return true
    return true;
}
