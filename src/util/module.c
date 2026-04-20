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

static char* cpx_strdup(const char* value) {
    size_t len;
    char* copy;

    if (!value) return NULL;
    len = strlen(value);
    copy = (char*)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, value, len + 1);
    return copy;
}

void init_module_registry(ModuleRegistry* registry) {
    registry->modules = NULL;
    registry->count = 0;
    registry->capacity = 0;
    registry->entry_path = NULL;
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
    free(registry->entry_path);
    registry->entry_path = NULL;
}

void module_registry_set_entry_path(ModuleRegistry* registry, const char* entry_path) {
    if (!registry) return;
    free(registry->entry_path);
    registry->entry_path = cpx_strdup(entry_path);
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

static int file_exists(const char* path) {
    FILE* test = fopen(path, "rb");
    if (!test) return 0;
    fclose(test);
    return 1;
}

static void path_parent_dir(const char* path, char* out, size_t outsz) {
    const char* slash;
    size_t len;

    if (!out || outsz == 0) return;
    out[0] = '.';
    out[1] = '\0';
    if (!path || !*path) return;

    strncpy(out, path, outsz - 1);
    out[outsz - 1] = '\0';

    slash = strrchr(out, '/');
#ifdef _WIN32
    {
        const char* backslash = strrchr(out, '\\');
        if (backslash && (!slash || backslash > slash)) slash = backslash;
    }
#endif
    if (!slash) {
        out[0] = '.';
        out[1] = '\0';
        return;
    }

    len = (size_t)(slash - out);
    if (len == 0) {
        out[1] = '\0';
    } else {
        out[len] = '\0';
    }
}

static int path_is_absolute(const char* path) {
    if (!path || !*path) return 0;
#ifdef _WIN32
    if ((path[0] && path[1] == ':') ||
        (path[0] == '\\' && path[1] == '\\') ||
        (path[0] == '/' && path[1] == '/')) {
        return 1;
    }
#endif
    return path[0] == '/';
}

static int try_module_candidates(const char* base_dir, const char* module_name, char* path_buffer, size_t path_buffer_sz) {
    const char* basename = module_name;
    const char* last_slash = strrchr(module_name, '/');
    FILE* test;

    if (!last_slash) last_slash = strrchr(module_name, '\\');
    if (last_slash) basename = last_slash + 1;

    if (base_dir && *base_dir) {
        snprintf(path_buffer, path_buffer_sz, "%s/%s.cpx", base_dir, module_name);
        test = fopen(path_buffer, "r");
        if (test) { fclose(test); return 1; }

        snprintf(path_buffer, path_buffer_sz, "%s/%s.nd", base_dir, module_name);
        test = fopen(path_buffer, "r");
        if (test) { fclose(test); return 1; }

        snprintf(path_buffer, path_buffer_sz, "%s/%s.cpx", base_dir, basename);
        test = fopen(path_buffer, "r");
        if (test) { fclose(test); return 1; }

        snprintf(path_buffer, path_buffer_sz, "%s/%s/%s.cpx", base_dir, basename, basename);
        test = fopen(path_buffer, "r");
        if (test) { fclose(test); return 1; }

        snprintf(path_buffer, path_buffer_sz, "%s/lib/%s.cpx", base_dir, basename);
        test = fopen(path_buffer, "r");
        if (test) { fclose(test); return 1; }

        snprintf(path_buffer, path_buffer_sz, "%s/lib/%s/%s.cpx", base_dir, basename, basename);
        test = fopen(path_buffer, "r");
        if (test) { fclose(test); return 1; }

        snprintf(path_buffer, path_buffer_sz, "%s/stdlib/%s.cpx", base_dir, basename);
        test = fopen(path_buffer, "r");
        if (test) { fclose(test); return 1; }

        snprintf(path_buffer, path_buffer_sz, "%s/stdlib/%s/%s.cpx", base_dir, basename, basename);
        test = fopen(path_buffer, "r");
        if (test) { fclose(test); return 1; }
    }

    return 0;
}

static int try_anchor_and_parents(const char* anchor_path, const char* module_name, char* path_buffer, size_t path_buffer_sz) {
    char dir[1024];

    if (!anchor_path || !*anchor_path) return 0;

    if (file_exists(anchor_path)) {
        path_parent_dir(anchor_path, dir, sizeof(dir));
    } else {
        strncpy(dir, anchor_path, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
    }

    for (int depth = 0; depth < 12; depth++) {
        char* slash;

        if (try_module_candidates(dir, module_name, path_buffer, path_buffer_sz)) {
            return 1;
        }

        slash = strrchr(dir, '/');
#ifdef _WIN32
        {
            char* backslash = strrchr(dir, '\\');
            if (backslash && (!slash || backslash > slash)) slash = backslash;
        }
#endif
        if (!slash) break;
        if (slash == dir) {
            dir[1] = '\0';
        } else if (slash > dir && slash[-1] == ':') {
            slash[1] = '\0';
        } else {
            *slash = '\0';
        }
    }

    return 0;
}

static char* resolve_module_path(const char* module_name, const char* anchor_path, const char* entry_path) {
    char path_buffer[1024];
    const char* stdlib_env;
    const char* lib_env;
    const char* cwd = ".";
    FILE* test;

    if (!module_name || !*module_name) return NULL;

    // 1. Try the module name directly as a path (.cpx first, .nd for backward compat)
    snprintf(path_buffer, sizeof(path_buffer), "%s.cpx", module_name);
    test = fopen(path_buffer, "r");
    if (test) { fclose(test); return cpx_strdup(path_buffer); }

    snprintf(path_buffer, sizeof(path_buffer), "%s.nd", module_name);
    test = fopen(path_buffer, "r");
    if (test) { fclose(test); return cpx_strdup(path_buffer); }

    if (path_is_absolute(module_name) && file_exists(module_name)) {
        return cpx_strdup(module_name);
    }

    // 2. Try relative to the importing file and then the entry source path.
    if (try_anchor_and_parents(anchor_path, module_name, path_buffer, sizeof(path_buffer))) {
        return cpx_strdup(path_buffer);
    }
    if (try_anchor_and_parents(entry_path, module_name, path_buffer, sizeof(path_buffer))) {
        return cpx_strdup(path_buffer);
    }

    // 3. Check CASPRIX_STDLIB / NUWAN_STDLIB environment variable
    stdlib_env = getenv("CASPRIX_STDLIB");
    if (!stdlib_env) stdlib_env = getenv("NUWAN_STDLIB");
    if (stdlib_env) {
        if (try_module_candidates(stdlib_env, module_name, path_buffer, sizeof(path_buffer))) {
            return cpx_strdup(path_buffer);
        }
    }

    // 4. Check CASPRIX_LIB / NUWAN_LIB environment variable
    lib_env = getenv("CASPRIX_LIB");
    if (!lib_env) lib_env = getenv("NUWAN_LIB");
    if (lib_env) {
        if (try_module_candidates(lib_env, module_name, path_buffer, sizeof(path_buffer))) {
            return cpx_strdup(path_buffer);
        }
    }

    // 5. Fallback to current working directory and its parents.
    if (try_anchor_and_parents(cwd, module_name, path_buffer, sizeof(path_buffer))) {
        return cpx_strdup(path_buffer);
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
        CPX_LOG(CPX_LOG_DEBUG, CPX_LOG_CAT_PARSER, "Attempting to load module: '%s' (length: %d, hex: %s)",
                 module_name, len, hex_dump);
    } else {
        CPX_LOG(CPX_LOG_ERROR, CPX_LOG_CAT_PARSER, "Module name is NULL");
        return NULL;
    }

    // Check if already loaded
    for (int i = 0; i < registry->count; i++) {
        if (strcmp(registry->modules[i].name, module_name) == 0) {
            CPX_LOG(CPX_LOG_DEBUG, CPX_LOG_CAT_PARSER, "Module '%s' already loaded", module_name);
            return &registry->modules[i];
        }
    }

    // Resolve module path
    char* module_path = resolve_module_path(module_name, (const char*)unused, registry->entry_path);
    if (!module_path) {
        CPX_LOG(CPX_LOG_ERROR, CPX_LOG_CAT_PARSER, "Module '%s' not found", module_name);
        return NULL;
    }

    CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_PARSER, "Loading module: %s from %s", module_name, module_path);

    // Read file
    char* source = read_file_content(module_path);
    if (!source) {
        CPX_LOG(CPX_LOG_ERROR, CPX_LOG_CAT_PARSER, "Could not read module file: %s", module_path);
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
        CPX_LOG(CPX_LOG_ERROR, CPX_LOG_CAT_PARSER, "Failed to parse module: %s", module_name);
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
    mod->name = cpx_strdup(module_name);
    mod->path = module_path;
    mod->statements = statements;
    mod->stmt_count = stmt_count;
    mod->loaded = true;

    free(source);

    CPX_LOG(CPX_LOG_INFO,  CPX_LOG_CAT_PARSER, "Module '%s' loaded successfully (%d statements)",
             module_name, stmt_count);

    // Recursively load any imports within this module
    for (int i = 0; i < stmt_count; i++) {
        if (statements[i] && statements[i]->type == STMT_INCLUDE) {
            IncludeStmt* incl = &statements[i]->as.include;
            if (incl->module_name) {
                load_module(registry, incl->module_name, mod->path);
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
