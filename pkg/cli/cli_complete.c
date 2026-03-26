/*
 * cli_complete.c — Casprix Package Manager full command implementation
 *
 * Copyright (c) 2026 Casprix Project
 * SPDX-License-Identifier: MIT
 */

#include "cli.h"
#include "../core/manifest.h"
#include "../core/installer.h"
#include "../core/publisher.h"
#include "../core/cache.h"
#include "../core/semver.h"
#include "../core/registry.h"
#include "../core/resolver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VERSION "1.0.0"

static void print_usage() {
    printf("Casperix Package Manager v%s\n\n", VERSION);
    printf("Usage: cpkg <command> [options]\n\n");
    printf("Commands:\n");
    printf("  init [name]              Initialize new package\n");
    printf("  install [package]        Install package(s)\n");
    printf("  update                   Update all dependencies\n");
    printf("  remove <package>         Remove package\n");
    printf("  list                     List installed packages\n");
    printf("  search <query>           Search registry\n");
    printf("  info <package>           Show package info\n");
    printf("\n");
    printf("Publishing:\n");
    printf("  login                    Login to registry\n");
    printf("  logout                   Logout\n");
    printf("  pack                     Create package tarball\n");
    printf("  publish                  Publish package\n");
    printf("  unpublish <ver>          Unpublish version\n");
    printf("\n");
    printf("Cache:\n");
    printf("  cache list               List cached packages\n");
    printf("  cache clean              Clear cache\n");
    printf("\n");
    printf("Other:\n");
    printf("  version                  Show version\n");
    printf("  help                     Show this help\n");
    printf("\n");
}

static int cmd_init(int argc, char** argv) {
    const char* name = (argc > 2) ? argv[2] : NULL;
    
    if (!name) {
        printf("Enter package name: ");
        char buf[256];
        if (fgets(buf, sizeof(buf), stdin)) {
            buf[strcspn(buf, "\r\n")] = '\0';
            name = buf;
        }
    }
    
    return pkg_init(name, ".") ? 0 : 1;
}

static int cmd_install(int argc, char** argv) {
    if (argc < 3) {
        /* Install from casper.json */
        printf("Installing from casper.json...\n");
        return pkg_install_from_manifest("casper.json", false) ? 0 : 1;
    }
    
    /* Install specific package */
    const char* package = argv[2];
    const char* version = NULL;
    
    /* Parse package@version */
    char* at = strchr(package, '@');
    if (at) {
        *at = '\0';
        version = at + 1;
    }
    
    return pkg_install_package(package, version) ? 0 : 1;
}

static int cmd_update(int argc, char** argv) {
    (void)argc; (void)argv;
    return pkg_update_all() ? 0 : 1;
}

static int cmd_remove(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: cpkg remove <package>\n");
        return 1;
    }
    
    return pkg_uninstall(argv[2]) ? 0 : 1;
}

static int cmd_list(int argc, char** argv) {
    (void)argc; (void)argv;
    
    printf("Installed packages:\n\n");
    
    int count;
    char** packages = pkg_cache_list(&count);
    
    if (count == 0) {
        printf("  (none)\n");
    } else {
        for (int i = 0; i < count; i++) {
            printf("  %s\n", packages[i]);
            free(packages[i]);
        }
        free(packages);
    }
    
    printf("\nTotal: %d packages\n", count);
    
    return 0;
}

static int cmd_search(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: cpkg search <query>\n");
        return 1;
    }
    
    const char* query = argv[2];
    printf("Searching for: %s\n\n", query);
    
    PkgRegistry* registry = pkg_registry_create(NULL, NULL);
    
    int count;
    PkgInfo** results = pkg_registry_search(registry, query, &count);
    
    for (int i = 0; i < count; i++) {
        PkgInfo* pkg = results[i];
        printf("  %s@%d.%d.%d - %s\n", 
               pkg->name, pkg->version.major, pkg->version.minor, pkg->version.patch,
               pkg->description ? pkg->description : "");
    }
    
    printf("\nFound %d packages\n", count);
    
    free(results);
    pkg_registry_free(registry);
    
    return 0;
}

static int cmd_info(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: cpkg info <package>\n");
        return 1;
    }
    
    const char* name = argv[2];
    
    PkgRegistry* registry = pkg_registry_create(NULL, NULL);
    PkgInfo* info = pkg_registry_info(registry, name, NULL);
    
    if (!info) {
        fprintf(stderr, "Package not found: %s\n", name);
        pkg_registry_free(registry);
        return 1;
    }
    
    printf("\n%s v%d.%d.%d\n", info->name, 
           info->version.major, info->version.minor, info->version.patch);
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    if (info->description) printf("%s\n\n", info->description);
    if (info->author) printf("Author: %s\n", info->author);
    if (info->license) printf("License: %s\n", info->license);
    if (info->repository) printf("Repository: %s\n", info->repository);
    
    if (info->dependency_count > 0) {
        printf("\nDependencies:\n");
        for (int i = 0; i < info->dependency_count; i++) {
            printf("  - %s %s\n", 
                   info->dependencies[i].name,
                   info->dependencies[i].version_spec);
        }
    }
    
    printf("\n");
    
    pkg_registry_free(registry);
    return 0;
}

static int cmd_login(int argc, char** argv) {
    (void)argc; (void)argv;
    
    printf("Username: ");
    char username[256];
    if (!fgets(username, sizeof(username), stdin)) return 1;
    username[strcspn(username, "\r\n")] = '\0';
    
    printf("Password: ");
    char password[256];
    if (!fgets(password, sizeof(password), stdin)) return 1;
    password[strcspn(password, "\r\n")] = '\0';
    
    char* api_key = pkg_login(username, password, NULL);
    
    if (api_key) {
        free(api_key);
        return 0;
    }
    
    return 1;
}

static int cmd_logout(int argc, char** argv) {
    (void)argc; (void)argv;
    return pkg_logout() ? 0 : 1;
}

static int cmd_pack(int argc, char** argv) {
    const char* output = (argc > 2) ? argv[2] : NULL;
    
    char* tarball = pkg_pack("casper.json", output);
    
    if (tarball) {
        free(tarball);
        return 0;
    }
    
    return 1;
}

static int cmd_publish(int argc, char** argv) {
    (void)argc; (void)argv;
    
    /* Pack first */
    char* tarball = pkg_pack("casper.json", NULL);
    if (!tarball) {
        fprintf(stderr, "Failed to create package\n");
        return 1;
    }
    
    /* Get API key */
    char* api_key = pkg_get_api_key();
    if (!api_key) {
        fprintf(stderr, "Not logged in. Run 'cpkg login' first.\n");
        free(tarball);
        return 1;
    }
    
    /* Publish */
    bool result = pkg_publish(tarball, NULL, api_key);
    
    free(tarball);
    free(api_key);
    
    return result ? 0 : 1;
}

static int cmd_unpublish(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: cpkg unpublish <version>\n");
        return 1;
    }
    
    /* Get package name from manifest */
    PackageManifest* manifest = manifest_parse_file("casper.json");
    if (!manifest) {
        fprintf(stderr, "No casper.json found\n");
        return 1;
    }
    
    bool result = pkg_unpublish(manifest->name, argv[2]);
    
    manifest_free(manifest);
    return result ? 0 : 1;
}

static int cmd_cache(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: cpkg cache <list|clean>\n");
        return 1;
    }
    
    const char* subcmd = argv[2];
    
    if (strcmp(subcmd, "list") == 0) {
        int count;
        char** packages = pkg_cache_list(&count);
        
        printf("Cached packages:\n");
        for (int i = 0; i < count; i++) {
            printf("  %s\n", packages[i]);
            free(packages[i]);
        }
        free(packages);
        
        printf("\nTotal: %d packages\n", count);
        return 0;
    }
    else if (strcmp(subcmd, "clean") == 0) {
        printf("Cleaning cache...\n");
        return pkg_cache_clean() ? 0 : 1;
    }
    else {
        fprintf(stderr, "Unknown cache command: %s\n", subcmd);
        return 1;
    }
}

static int cmd_version(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("cpkg version %s\n", VERSION);
    return 0;
}

int cpkg_run(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }
    
    const char* cmd = argv[1];
    
    /* Initialize cache */
    pkg_cache_init();
    
    /* Command dispatch */
    if (strcmp(cmd, "init") == 0) {
        return cmd_init(argc, argv);
    }
    else if (strcmp(cmd, "install") == 0 || strcmp(cmd, "i") == 0) {
        return cmd_install(argc, argv);
    }
    else if (strcmp(cmd, "update") == 0) {
        return cmd_update(argc, argv);
    }
    else if (strcmp(cmd, "remove") == 0 || strcmp(cmd, "rm") == 0) {
        return cmd_remove(argc, argv);
    }
    else if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0) {
        return cmd_list(argc, argv);
    }
    else if (strcmp(cmd, "search") == 0) {
        return cmd_search(argc, argv);
    }
    else if (strcmp(cmd, "info") == 0) {
        return cmd_info(argc, argv);
    }
    else if (strcmp(cmd, "login") == 0) {
        return cmd_login(argc, argv);
    }
    else if (strcmp(cmd, "logout") == 0) {
        return cmd_logout(argc, argv);
    }
    else if (strcmp(cmd, "pack") == 0) {
        return cmd_pack(argc, argv);
    }
    else if (strcmp(cmd, "publish") == 0) {
        return cmd_publish(argc, argv);
    }
    else if (strcmp(cmd, "unpublish") == 0) {
        return cmd_unpublish(argc, argv);
    }
    else if (strcmp(cmd, "cache") == 0) {
        return cmd_cache(argc, argv);
    }
    else if (strcmp(cmd, "version") == 0 || strcmp(cmd, "-v") == 0 || strcmp(cmd, "--version") == 0) {
        return cmd_version(argc, argv);
    }
    else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        print_usage();
        return 0;
    }
    else {
        fprintf(stderr, "Unknown command: %s\n\n", cmd);
        print_usage();
        return 1;
    }
}
