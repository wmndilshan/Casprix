/*
 * Casprix Package Manager - Dependency Resolver
 */

#ifndef PKG_RESOLVER_H
#define PKG_RESOLVER_H

#include "registry.h"
#include <stdbool.h>

// Resolved package (with specific version)
typedef struct {
    char* name;
    PkgVersion version;
    PkgDependency* dependencies;
    int dependency_count;
} ResolvedPkg;

// Dependency graph node
typedef struct DepNode {
    ResolvedPkg* package;
    struct DepNode** dependencies;
    int dep_count;
} DepNode;

// Resolver context
typedef struct {
    PkgRegistry* registry;
    ResolvedPkg** resolved;
    int resolved_count;
    bool* visiting;  // For cycle detection
} PkgResolver;

// Create resolver
PkgResolver* pkg_resolver_create(PkgRegistry* registry);

// Resolve dependencies
bool pkg_resolver_resolve(PkgResolver* resolver, const char* package, const char* version);

// Get resolved packages
ResolvedPkg** pkg_resolver_get_resolved(PkgResolver* resolver, int* count);

// Check for conflicts
bool pkg_resolver_check_conflicts(PkgResolver* resolver);

// Free resolver
void pkg_resolver_free(PkgResolver* resolver);

#endif // PKG_RESOLVER_H
