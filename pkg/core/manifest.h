/**
 * Package Manifest Parser for casper.json
 */

#ifndef PKG_MANIFEST_H
#define PKG_MANIFEST_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Manifest Structure
 * ======================================================================== */

typedef struct {
    char* name;
    char* version_spec;
} ManifestDependency;

typedef struct {
    char* name;
    char* version;
    char* description;
    char* author;
    char* license;
    char* main;                      /* Entry point file */
    char* repository;                  /* Git repository URL */
    
    ManifestDependency* dependencies;
    int dependency_count;
    
    ManifestDependency* dev_dependencies;
    int dev_dependency_count;
    
    char** files;                    /* File patterns to include */
    int file_count;
    
    char** keywords;
    int keyword_count;
} PackageManifest;

/* ========================================================================
 * Manifest Operations
 * ======================================================================== */

/**
 * Parse manifest from JSON file
 */
PackageManifest* manifest_parse_file(const char* filename);

/**
 * Parse manifest from JSON string
 */
PackageManifest* manifest_parse_string(const char* json);

/**
 * Write manifest to file
 */
bool manifest_write_file(PackageManifest* manifest, const char* filename);

/**
 * Create default manifest template
 */
PackageManifest* manifest_create_default(const char* name);

/**
 * Free manifest
 */
void manifest_free(PackageManifest* manifest);

/**
 * Validate manifest
 * Returns true if valid, prints errors to stderr
 */
bool manifest_validate(PackageManifest* manifest);

/**
 * Add dependency
 */
void manifest_add_dependency(PackageManifest* manifest, 
                             const char* name, const char* version_spec,
                             bool is_dev);

/**
 * Remove dependency
 */
void manifest_remove_dependency(PackageManifest* manifest, const char* name);

#ifdef __cplusplus
}
#endif

#endif /* PKG_MANIFEST_H */
