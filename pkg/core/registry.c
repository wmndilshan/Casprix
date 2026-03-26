/*
 * Casprix Package Manager - Registry Implementation
 */

#include "registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PkgRegistry* pkg_registry_create(const char* cache_dir, const char* remote_url) {
    PkgRegistry* reg = malloc(sizeof(PkgRegistry));
    reg->cache_dir = strdup(cache_dir ? cache_dir : PKG_CACHE_DIR);
    reg->remote_url = strdup(remote_url ? remote_url : PKG_REGISTRY_URL);
    reg->packages = NULL;
    reg->package_count = 0;
    
    pkg_registry_load_cache(reg);
    
    return reg;
}

// Parse semantic version
static PkgVersion parse_version(const char* str) {
    PkgVersion ver = {0, 0, 0, NULL};
    sscanf(str, "%d.%d.%d", &ver.major, &ver.minor, &ver.patch);
    
    // Check for prerelease (e.g., "1.0.0-beta")
    const char* dash = strchr(str, '-');
    if (dash) {
        ver.prerelease = strdup(dash + 1);
    }
    
    return ver;
}

// Compare versions: -1 if a < b, 0 if equal, 1 if a > b
int pkg_version_compare(PkgVersion* a, PkgVersion* b) {
    if (a->major != b->major) return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor) return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
    
    // Check prerelease
    if (a->prerelease && !b->prerelease) return -1;
    if (!a->prerelease && b->prerelease) return 1;
    if (a->prerelease && b->prerelease) {
        return strcmp(a->prerelease, b->prerelease);
    }
    
    return 0;
}

// Check if version satisfies spec (simplified)
bool pkg_version_satisfies(PkgVersion* version, const char* spec) {
    if (!spec || spec[0] == '\0') return true;
    
    // Handle caret (^) - compatible versions
    if (spec[0] == '^') {
        PkgVersion required = parse_version(spec + 1);
        
        // Major version must match, minor/patch can be higher
        if (version->major != required.major) return false;
        if (version->minor < required.minor) return false;
        if (version->minor == required.minor && version->patch < required.patch) return false;
        
        return true;
    }
    
    // Handle tilde (~) - approximately equivalent
    if (spec[0] == '~') {
        PkgVersion required = parse_version(spec + 1);
        
        // Major and minor must match
        if (version->major != required.major) return false;
        if (version->minor != required.minor) return false;
        if (version->patch < required.patch) return false;
        
        return true;
    }
    
    // Handle >= and <=
    if (strncmp(spec, ">=", 2) == 0) {
        PkgVersion required = parse_version(spec + 2);
        return pkg_version_compare(version, &required) >= 0;
    }
    
    if (strncmp(spec, "<=", 2) == 0) {
        PkgVersion required = parse_version(spec + 2);
        return pkg_version_compare(version, &required) <= 0;
    }
    
    // Exact match
    PkgVersion required = parse_version(spec);
    return pkg_version_compare(version, &required) == 0;
}

// Search packages
PkgInfo** pkg_registry_search(PkgRegistry* reg, const char* query, int* count) {
    PkgInfo** results = NULL;
    *count = 0;
    
    for (int i = 0; i < reg->package_count; i++) {
        if (strstr(reg->packages[i]->name, query) || 
            strstr(reg->packages[i]->description, query)) {
            
            results = realloc(results, (*count + 1) * sizeof(PkgInfo*));
            results[*count] = reg->packages[i];
            (*count)++;
        }
    }
    
    return results;
}

// Get package info
PkgInfo* pkg_registry_info(PkgRegistry* reg, const char* name, const char* version) {
    for (int i = 0; i < reg->package_count; i++) {
        PkgInfo* pkg = reg->packages[i];
        
        if (strcmp(pkg->name, name) == 0) {
            if (!version || pkg_version_satisfies(&pkg->version, version)) {
                return pkg;
            }
        }
    }
    
    // Not found in cache - would fetch from remote here
    return NULL;
}

// Download package
bool pkg_registry_download(PkgRegistry* reg, const char* name, const char* version, char** dest) {
    PkgInfo* info = pkg_registry_info(reg, name, version);
    if (!info) return false;
    
    // Construct destination path
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/%d.%d.%d", 
             reg->cache_dir, name, 
             info->version.major, info->version.minor, info->version.patch);
    
    *dest = strdup(path);
    
    // Would download tarball here using HTTP library
    printf("Would download %s from %s\n", name, info->tarball_url);
    
    return true;
}

// Load cache from disk
void pkg_registry_load_cache(PkgRegistry* reg) {
    char cache_file[512];
    snprintf(cache_file, sizeof(cache_file), "%s/cache.json", reg->cache_dir);
    
    // Would parse JSON here
    // For now, just placeholder
}

// Save cache to disk
void pkg_registry_save_cache(PkgRegistry* reg) {
    char cache_file[512];
    snprintf(cache_file, sizeof(cache_file), "%s/cache.json", reg->cache_dir);
    
    // Would write JSON here
}

// Free registry
void pkg_registry_free(PkgRegistry* reg) {
    if (!reg) return;
    
    free(reg->cache_dir);
    free(reg->remote_url);
    
    for (int i = 0; i < reg->package_count; i++) {
        PkgInfo* pkg = reg->packages[i];
        free(pkg->name);
        free(pkg->description);
        free(pkg->author);
        free(pkg->license);
        free(pkg->tarball_url);
        free(pkg->repository);
        
        if (pkg->version.prerelease) free(pkg->version.prerelease);
        
        for (int j = 0; j < pkg->dependency_count; j++) {
            free(pkg->dependencies[j].name);
            free(pkg->dependencies[j].version_spec);
        }
        free(pkg->dependencies);
        free(pkg);
    }
    
    free(reg->packages);
    free(reg);
}
