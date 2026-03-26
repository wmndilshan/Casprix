/*
 * Casprix Package Manager - Core Registry Implementation
 */

#ifndef PKG_REGISTRY_H
#define PKG_REGISTRY_H

#include <stdbool.h>

#define PKG_CACHE_DIR ".casprix/packages"
#define PKG_REGISTRY_URL "https://packages.casprix.org"

// Package version
typedef struct {
    int major;
    int minor;
    int patch;
    char* prerelease;
} PkgVersion;

// Package dependency
typedef struct {
    char* name;
    char* version_spec;  // e.g., "^1.2.0", ">=2.0.0"
} PkgDependency;

// Package information
typedef struct {
    char* name;
    PkgVersion version;
    char* description;
    char* author;
    char* license;
    PkgDependency* dependencies;
    int dependency_count;
    char* tarball_url;
    char* repository;
} PkgInfo;

// Package registry
typedef struct {
    char* cache_dir;
    char* remote_url;
    PkgInfo** packages;
    int package_count;
} PkgRegistry;

// Initialize registry
PkgRegistry* pkg_registry_create(const char* cache_dir, const char* remote_url);

// Search for packages
PkgInfo** pkg_registry_search(PkgRegistry* reg, const char* query, int* count);

// Get package info
PkgInfo* pkg_registry_info(PkgRegistry* reg, const char* name, const char* version);

// Download package
bool pkg_registry_download(PkgRegistry* reg, const char* name, const char* version, char** dest);

// Load local cache
void pkg_registry_load_cache(PkgRegistry* reg);

// Save cache
void pkg_registry_save_cache(PkgRegistry* reg);

// Free registry
void pkg_registry_free(PkgRegistry* reg);

// Version comparison
int pkg_version_compare(PkgVersion* a, PkgVersion* b);
bool pkg_version_satisfies(PkgVersion* version, const char* spec);

#endif // PKG_REGISTRY_H
