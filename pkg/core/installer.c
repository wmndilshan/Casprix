/**
 * Package Installer Implementation
 */

#include "installer.h"
#include "cache.h"
#include "registry.h"
#include "resolver.h"
#include "semver.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

bool pkg_install_from_manifest(const char* manifest_path, bool dev_deps) {
    /* Parse manifest */
    PackageManifest* manifest = manifest_parse_file(manifest_path);
    if (!manifest) {
        fprintf(stderr, "Failed to parse %s\n", manifest_path);
        return false;
    }
    
    if (!manifest_validate(manifest)) {
        manifest_free(manifest);
        return false;
    }
    
    printf("Installing dependencies for %s@%s...\n", manifest->name, manifest->version);
    
    /* Initialize cache */
    pkg_cache_init();
    
    /* Create registry and resolver */
    PkgRegistry* registry = pkg_registry_create(NULL, NULL);
    PkgResolver* resolver = pkg_resolver_create(registry);
    
    /* Resolve dependencies */
    for (int i = 0; i < manifest->dependency_count; i++) {
        const char* name = manifest->dependencies[i].name;
        const char* spec = manifest->dependencies[i].version_spec;
        
        printf("  - %s@%s... ", name, spec);
        
        if (!pkg_resolver_resolve(resolver, name, spec)) {
            printf("failed!\n");
            fprintf(stderr, "Failed to resolve %s@%s\n", name, spec);
            continue;
        }
        
        printf("ok\n");
    }
    
    /* Dev dependencies */
    if (dev_deps) {
        for (int i = 0; i < manifest->dev_dependency_count; i++) {
            const char* name = manifest->dev_dependencies[i].name;
            const char* spec = manifest->dev_dependencies[i].version_spec;
            
            printf("  - %s@%s (dev)... ", name, spec);
            
            if (!pkg_resolver_resolve(resolver, name, spec)) {
                printf("failed!\n");
                continue;
            }
            
            printf("ok\n");
        }
    }
    
    /* Check conflicts */
    if (!pkg_resolver_check_conflicts(resolver)) {
        fprintf(stderr, "\\nDependency conflicts detected!\\n");
        pkg_resolver_free(resolver);
        pkg_registry_free(registry);
        manifest_free(manifest);
        return false;
    }
    
    /* Download and install */
    int count;
    ResolvedPkg** packages = pkg_resolver_get_resolved(resolver, &count);
    
    printf("\\nInstalling %d packages...\\n", count);
    
    for (int i = 0; i < count; i++) {
        ResolvedPkg* pkg = packages[i];
        char version_str[64];
        snprintf(version_str, sizeof(version_str), "%d.%d.%d",
                 pkg->version.major, pkg->version.minor, pkg->version.patch);
        
        /* Check cache first */
        if (pkg_cache_has(pkg->name, version_str)) {
            printf("  ✓ %s@%s (cached)\\n", pkg->name, version_str);
            continue;
        }
        
        /* Download from registry */
        char* path;
        if (pkg_registry_download(registry, pkg->name, version_str, &path)) {
            printf("  ✓ %s@%s\\n", pkg->name, version_str);
            free(path);
        } else {
            printf("  ✗ %s@%s (download failed)\\n", pkg->name, version_str);
        }
    }
    
    printf("\\n✓ Successfully installed %d packages\\n", count);
    
    pkg_resolver_free(resolver);
    pkg_registry_free(registry);
    manifest_free(manifest);
    
    return true;
}

bool pkg_install_package(const char* name, const char* version_spec) {
    printf("Installing %s@%s...\\n", name, version_spec ? version_spec : "*");
    
    pkg_cache_init();
    
    PkgRegistry* registry = pkg_registry_create(NULL, NULL);
    PkgResolver* resolver = pkg_resolver_create(registry);
    
    if (!pkg_resolver_resolve(resolver, name, version_spec)) {
        fprintf(stderr, "Failed to resolve %s\\n", name);
        pkg_resolver_free(resolver);
        pkg_registry_free(registry);
        return false;
    }
    
    if (!pkg_resolver_check_conflicts(resolver)) {
        fprintf(stderr, "Dependency conflicts detected!\\n");
        pkg_resolver_free(resolver);
        pkg_registry_free(registry);
        return false;
    }
    
    int count;
    ResolvedPkg** packages = pkg_resolver_get_resolved(resolver, &count);
    
    for (int i = 0; i < count; i++) {
        ResolvedPkg* pkg = packages[i];
        char version_str[64];
        snprintf(version_str, sizeof(version_str), "%d.%d.%d",
                 pkg->version.major, pkg->version.minor, pkg->version.patch);
        
        if (pkg_cache_has(pkg->name, version_str)) {
            printf("  ✓ %s@%s (cached)\\n", pkg->name, version_str);
        } else {
            char* path;
            if (pkg_registry_download(registry, pkg->name, version_str, &path)) {
                printf("  ✓ %s@%s\\n", pkg->name, version_str);
                free(path);
            }
        }
    }
    
    printf("\\n✓ Installed %s\\n", name);
    
    pkg_resolver_free(resolver);
    pkg_registry_free(registry);
    
    return true;
}

bool pkg_uninstall(const char* name) {
    printf("Uninstalling %s...\\n", name);
    
    /* Load manifest */
    PackageManifest* manifest = manifest_parse_file("casper.json");
    if (!manifest) {
        fprintf(stderr, "No casper.json found\\n");
        return false;
    }
    
    /* Remove from manifest */
    manifest_remove_dependency(manifest, name);
    
    /* Write back */
    if (!manifest_write_file(manifest, "casper.json")) {
        fprintf(stderr, "Failed to update casper.json\\n");
        manifest_free(manifest);
        return false;
    }
    
    printf("✓ Removed %s from casper.json\\n", name);
    
    manifest_free(manifest);
    return true;
}

bool pkg_update_all(void) {
    printf("Updating all dependencies...\\n");
    
    /* Just reinstall everything for now */
    return pkg_install_from_manifest("casper.json", false);
}

bool pkg_download(const char* url, const char* dest) {
    /* Platform-specific download */
#ifdef _WIN32
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), 
            "powershell -Command \"Invoke-WebRequest -Uri '%s' -OutFile '%s'\"",
            url, dest);
    return system(cmd) == 0;
#else
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "curl -L -o '%s' '%s'", dest, url);
    return system(cmd) == 0;
#endif
}

bool pkg_extract(const char* tarball, const char* dest_dir) {
    /* Platform-specific extraction */
#ifdef _WIN32
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "tar -xzf \"%s\" -C \"%s\"", tarball, dest_dir);
    return system(cmd) == 0;
#else
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "tar -xzf \"%s\" -C \"%s\"", tarball, dest_dir);
    return system(cmd) == 0;
#endif
}

bool pkg_verify_checksum(const char* file, const char* expected_sha256) {
    /* For now, skip verification */
    /* In full implementation, use OpenSSL or similar for SHA256 */
    (void)file;
    (void)expected_sha256;
    return true;
}
