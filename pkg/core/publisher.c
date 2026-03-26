/**
 * Package Publisher Implementation
 */

#include "publisher.h"
#include "manifest.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

bool pkg_init(const char* name, const char* path) {
    const char* dir = path ? path : ".";
    
    printf("Initializing package: %s\n", name ? name : "my-package");
    
    /* Create casper.json */
    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/casper.json", dir);
    
    PackageManifest* manifest = manifest_create_default(name);
    
    if (!manifest_write_file(manifest, manifest_path)) {
        fprintf(stderr, "Failed to create casper.json\n");
        manifest_free(manifest);
        return false;
    }
    
    printf("Created casper.json\n");
    
    /* Create src directory */
    char src_dir[1024];
    snprintf(src_dir, sizeof(src_dir), "%s/src", dir);
    
#ifdef _WIN32
    _mkdir(src_dir);
#else
    mkdir(src_dir, 0755);
#endif
    
    /* Create main.cpx */
    char main_file[1024];
    snprintf(main_file, sizeof(main_file), "%s/src/main.cpx", dir);
    
    FILE* f = fopen(main_file, "w");
    if (f) {
        fprintf(f, "// %s - Main entry point\n\n", name ? name : "My Package");
        fprintf(f, "fn main() {\n");
        fprintf(f, "    println(\"Hello from %s!\")\n", name ? name : "my package");
        fprintf(f, "}\n");
        fclose(f);
        printf("Created src/main.cpx\n");
    }
    
    /* Create README */
    char readme_path[1024];
    snprintf(readme_path, sizeof(readme_path), "%s/README.md", dir);
    
    f = fopen(readme_path, "w");
    if (f) {
        fprintf(f, "# %s\n\n", name ? name : "My Package");
        fprintf(f, "%s\n\n", manifest->description);
        fprintf(f, "## Installation\n\n");
        fprintf(f, "```bash\n");
        fprintf(f, "cpkg install %s\n", name ? name : "my-package");
        fprintf(f, "```\n\n");
        fprintf(f, "## Usage\n\n");
        fprintf(f, "```casperix\n");
        fprintf(f, "import \"%s\"\n", name ? name : "my-package");
        fprintf(f, "```\n\n");
        fprintf(f, "## License\n\n");
        fprintf(f, "%s\n", manifest->license);
        fclose(f);
        printf("Created README.md\n");
    }
    
    printf("\n✓ Package initialized successfully!\n");
    
    manifest_free(manifest);
    return true;
}

char* pkg_pack(const char* manifest_path, const char* output_path) {
    const char* manifest_file = manifest_path ? manifest_path : "casper.json";
    
    /* Parse manifest */
    PackageManifest* manifest = manifest_parse_file(manifest_file);
    if (!manifest) {
        fprintf(stderr, "Failed to parse %s\n", manifest_file);
        return NULL;
    }
    
    if (!manifest_validate(manifest)) {
        manifest_free(manifest);
        return NULL;
    }
    
    /* Generate output filename */
    char* tarball;
    if (output_path) {
        tarball = strdup(output_path);
    } else {
        tarball = (char*)malloc(512);
        snprintf(tarball, 512, "%s-%s.tar.gz", manifest->name, manifest->version);
    }
    
    printf("Creating package: %s\n", tarball);
    
    /* Create tarball */
#ifdef _WIN32
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), 
            "tar -czf \"%s\" --exclude=node_modules --exclude=.git --exclude=*.tar.gz .",
            tarball);
#else
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), 
            "tar -czf \"%s\" --exclude=node_modules --exclude=.git --exclude=*.tar.gz .",
            tarball);
#endif
    
    if (system(cmd) != 0) {
        fprintf(stderr, "Failed to create tarball\n");
        free(tarball);
        manifest_free(manifest);
        return NULL;
    }
    
    printf("✓ Created %s\n", tarball);
    
    manifest_free(manifest);
    return tarball;
}

bool pkg_publish(const char* tarball, const char* registry_url, const char* api_key) {
    const char* url = registry_url ? registry_url : "https://registry.casperix.org";
    
    if (!api_key) {
        fprintf(stderr, "Error: Not logged in. Run 'cpkg login' first.\n");
        return false;
    }
    
    printf("Publishing to %s...\n", url);
    
    /* Use curl to upload */
#ifdef _WIN32
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
            "curl -X POST -H \"Authorization: Bearer %s\" -F \"package=@%s\" %s/api/publish",
            api_key, tarball, url);
#else
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
            "curl -X POST -H \"Authorization: Bearer %s\" -F \"package=@%s\" %s/api/publish",
            api_key, tarball, url);
#endif
    
    int result = system(cmd);
    
    if (result == 0) {
        printf("\n✓ Package published successfully!\n");
        return true;
    } else {
        fprintf(stderr, "\n✗ Failed to publish package\n");
        return false;
    }
}

bool pkg_unpublish(const char* name, const char* version) {
    printf("Unpublishing %s@%s...\n", name, version);
    
    char* api_key = pkg_get_api_key();
    if (!api_key) {
        fprintf(stderr, "Error: Not logged in\n");
        return false;
    }
    
    /* Call registry API */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
            "curl -X DELETE -H \"Authorization: Bearer %s\" "
            "https://registry.casperix.org/api/packages/%s/%s",
            api_key, name, version);
    
    int result = system(cmd);
    free(api_key);
    
    if (result == 0) {
        printf("✓ Unpublished %s@%s\n", name, version);
        return true;
    }
    
    return false;
}

char* pkg_login(const char* username, const char* password, const char* registry_url) {
    const char* url = registry_url ? registry_url : "https://registry.casperix.org";
    
    printf("Logging in to %s...\n", url);
    
    /* For now, return a dummy API key */
    /* In full implementation, POST credentials to /api/login */
    
    char* api_key = strdup("demo_api_key_12345");
    
    /* Save API key */
    const char* home;
#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    
    if (home) {
        char auth_path[1024];
        snprintf(auth_path, sizeof(auth_path), "%s/.cpkg/auth.txt", home);
        
        FILE* f = fopen(auth_path, "w");
        if (f) {
            fprintf(f, "%s\n", api_key);
            fclose(f);
        }
    }
    
    printf("✓ Successfully logged in as %s\n", username);
    
    return api_key;
}

bool pkg_logout(void) {
    printf("Logging out...\n");
    
    const char* home;
#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    
    if (home) {
        char auth_path[1024];
        snprintf(auth_path, sizeof(auth_path), "%s/.cpkg/auth.txt", home);
        remove(auth_path);
    }
    
    printf("✓ Logged out\n");
    return true;
}

char* pkg_get_api_key(void) {
    const char* home;
#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    
    if (!home) return NULL;
    
    char auth_path[1024];
    snprintf(auth_path, sizeof(auth_path), "%s/.cpkg/auth.txt", home);
    
    FILE* f = fopen(auth_path, "r");
    if (!f) return NULL;
    
    char key[256];
    if (fgets(key, sizeof(key), f)) {
        /* Remove newline */
        key[strcspn(key, "\r\n")] = '\0';
        fclose(f);
        return strdup(key);
    }
    
    fclose(f);
    return NULL;
}
