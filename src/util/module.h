#ifndef MODULE_H
#define MODULE_H

#include "casprix/common.h"
#include "compiler/frontend/ast.h"
#include "support/log.h"

typedef struct {
    char* name;
    char* path;
    Stmt** statements;
    int stmt_count;
    bool loaded;
} Module;

typedef struct {
    Module* modules;
    int count;
    int capacity;
    char* entry_path;
} ModuleRegistry;

// Module system
void init_module_registry(ModuleRegistry* registry);
void free_module_registry(ModuleRegistry* registry);
void module_registry_set_entry_path(ModuleRegistry* registry, const char* entry_path);

/* Load a module from file (logger parameter deprecated - uses global) */
Module* load_module(ModuleRegistry* registry, const char* module_name, void* unused);

/* Resolve module dependencies (logger parameter deprecated - uses global) */
bool resolve_modules(ModuleRegistry* registry, Stmt** statements, int count, void* unused);

// Get module by name
Module* get_module(ModuleRegistry* registry, const char* name);

#endif // MODULE_H


