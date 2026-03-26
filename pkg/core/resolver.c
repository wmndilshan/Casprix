/*
 * Casprix Package Manager - Dependency Resolver Implementation
 */

#include "resolver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PkgResolver* pkg_resolver_create(PkgRegistry* registry) {
    PkgResolver* resolver = malloc(sizeof(PkgResolver));
    resolver->registry = registry;
    resolver->resolved = NULL;
    resolver->resolved_count = 0;
    resolver->visiting = NULL;
    
    return resolver;
}

// Check if package is already resolved
static bool is_resolved(PkgResolver* resolver, const char* name) {
    for (int i = 0; i < resolver->resolved_count; i++) {
        if (strcmp(resolver->resolved[i]->name, name) == 0) {
            return true;
        }
    }
    return false;
}

// Add to resolved list
static void add_resolved(PkgResolver* resolver, ResolvedPkg* pkg) {
    resolver->resolved = realloc(resolver->resolved, 
                                 (resolver->resolved_count + 1) * sizeof(ResolvedPkg*));
    resolver->resolved[resolver->resolved_count++] = pkg;
}

// Recursive dependency resolution
static bool resolve_recursive(PkgResolver* resolver, const char* package, 
                              const char* version, int depth) {
    // Prevent infinite recursion
    if (depth > 100) {
        fprintf(stderr, "Dependency depth exceeded for %s\n", package);
        return false;
    }
    
    // Already resolved?
    if (is_resolved(resolver, package)) {
        return true;
    }
    
    // Get package info
    PkgInfo* info = pkg_registry_info(resolver->registry, package, version);
    if (!info) {
        fprintf(stderr, "Package not found: %s@%s\n", package, version);
        return false;
    }
    
    // Resolve dependencies first
    for (int i = 0; i < info->dependency_count; i++) {
        PkgDependency* dep = &info->dependencies[i];
        
        if (!resolve_recursive(resolver, dep->name, dep->version_spec, depth + 1)) {
            return false;
        }
    }
    
    // Add to resolved list
    ResolvedPkg* resolved = malloc(sizeof(ResolvedPkg));
    resolved->name = strdup(info->name);
    resolved->version = info->version;
    resolved->dependencies = malloc(info->dependency_count * sizeof(PkgDependency));
    resolved->dependency_count = info->dependency_count;
    
    for (int i = 0; i < info->dependency_count; i++) {
        resolved->dependencies[i].name = strdup(info->dependencies[i].name);
        resolved->dependencies[i].version_spec = strdup(info->dependencies[i].version_spec);
    }
    
    add_resolved(resolver, resolved);
    
    printf("Resolved: %s@%d.%d.%d\n", 
           resolved->name, resolved->version.major, 
           resolved->version.minor, resolved->version.patch);
    
    return true;
}

// Main resolve function
bool pkg_resolver_resolve(PkgResolver* resolver, const char* package, const char* version) {
    return resolve_recursive(resolver, package, version, 0);
}

// Get resolved packages
ResolvedPkg** pkg_resolver_get_resolved(PkgResolver* resolver, int* count) {
    *count = resolver->resolved_count;
    return resolver->resolved;
}

// Check for version conflicts
bool pkg_resolver_check_conflicts(PkgResolver* resolver) {
    // Check if same package appears with different incompatible versions
    for (int i = 0; i < resolver->resolved_count; i++) {
        for (int j = i + 1; j < resolver->resolved_count; j++) {
            ResolvedPkg* a = resolver->resolved[i];
            ResolvedPkg* b = resolver->resolved[j];
            
            if (strcmp(a->name, b->name) == 0) {
                if (pkg_version_compare(&a->version, &b->version) != 0) {
                    fprintf(stderr, "Version conflict: %s@%d.%d.%d vs %d.%d.%d\n",
                            a->name, a->version.major, a->version.minor, a->version.patch,
                            b->version.major, b->version.minor, b->version.patch);
                    return false;
                }
            }
        }
    }
    
    return true;
}

// Free resolver
void pkg_resolver_free(PkgResolver* resolver) {
    if (!resolver) return;
    
    for (int i = 0; i < resolver->resolved_count; i++) {
        ResolvedPkg* pkg = resolver->resolved[i];
        free(pkg->name);
        
        for (int j = 0; j < pkg->dependency_count; j++) {
            free(pkg->dependencies[j].name);
            free(pkg->dependencies[j].version_spec);
        }
        free(pkg->dependencies);
        free(pkg);
    }
    
    free(resolver->resolved);
    free(resolver->visiting);
    free(resolver);
}
