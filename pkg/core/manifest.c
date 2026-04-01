/**
 * Package Manifest Implementation
 * Simple JSON parser for casper.json
 */

#include "manifest.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Simple JSON helpers */
static char* json_get_string(const char* json, const char* key) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\":", key);
    
    const char* start = strstr(json, search);
    if (!start) return NULL;
    
    start = strchr(start + strlen(search), '"');
    if (!start) return NULL;
    start++;
    
    const char* end = strchr(start, '"');
    if (!end) return NULL;
    
    int len = end - start;
    char* result = (char*)malloc(len + 1);
    strncpy(result, start, len);
    result[len] = '\0';
    
    return result;
}

static int json_count_deps(const char* json, const char* key) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\":{", key);
    
    const char* start = strstr(json, search);
    if (!start) return 0;
    
    start += strlen(search);
    const char* end = strchr(start, '}');
    if (!end) return 0;
    
    int count = 0;
    for (const char* p = start; p < end; p++) {
        if (*p == ',') count++;
    }
    
    return count > 0 ? count + 1 : (start < end && *start != '}' ? 1 : 0);
}

static void json_parse_deps(const char* json, const char* key, 
                            ManifestDependency** deps, int* count) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\":{", key);
    
    const char* start = strstr(json, search);
    if (!start) {
        *count = 0;
        return;
    }
    
    start += strlen(search);
    const char* end = strchr(start, '}');
    if (!end) return;
    
    *count = json_count_deps(json, key);
    if (*count == 0) return;
    
    *deps = (ManifestDependency*)calloc(*count, sizeof(ManifestDependency));
    
    int idx = 0;
    const char* p = start;
    
    while (p < end && idx < *count) {
        /* Skip whitespace */
        while (isspace(*p)) p++;
        if (*p == '}') break;
        
        /* Find name */
        if (*p == '"') {
            const char* name_start = ++p;
            const char* name_end = strchr(p, '"');
            if (!name_end) break;
            
            int name_len = name_end - name_start;
            (*deps)[idx].name = (char*)malloc(name_len + 1);
            strncpy((*deps)[idx].name, name_start, name_len);
            (*deps)[idx].name[name_len] = '\0';
            
            p = name_end + 1;
        }
        
        /* Skip to version */
        p = strchr(p, ':');
        if (!p) break;
        p++;
        
        while (isspace(*p)) p++;
        
        if (*p == '"') {
            const char* ver_start = ++p;
            const char* ver_end = strchr(p, '"');
            if (!ver_end) break;
            
            int ver_len = ver_end - ver_start;
            (*deps)[idx].version_spec = (char*)malloc(ver_len + 1);
            strncpy((*deps)[idx].version_spec, ver_start, ver_len);
            (*deps)[idx].version_spec[ver_len] = '\0';
            
            p = ver_end + 1;
        }
        
        idx++;
        
        /* Skip to next or end */
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
}

PackageManifest* manifest_parse_file(const char* filename) {
    FILE* f = fopen(filename, "r");
    if (!f) return NULL;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char* json = (char*)malloc(size + 1);
    size_t read_bytes = fread(json, 1, size, f);
    json[read_bytes] = '\0';
    fclose(f);
    
    PackageManifest* manifest = manifest_parse_string(json);
    free(json);
    
    return manifest;
}

PackageManifest* manifest_parse_string(const char* json) {
    if (!json) return NULL;
    
    PackageManifest* m = (PackageManifest*)calloc(1, sizeof(PackageManifest));
    
    m->name = json_get_string(json, "name");
    m->version = json_get_string(json, "version");
    m->description = json_get_string(json, "description");
    m->author = json_get_string(json, "author");
    m->license = json_get_string(json, "license");
    m->main = json_get_string(json, "main");
    m->repository = json_get_string(json, "repository");
    
    json_parse_deps(json, "dependencies", &m->dependencies, &m->dependency_count);
    json_parse_deps(json, "devDependencies", &m->dev_dependencies, &m->dev_dependency_count);
    
    return m;
}

bool manifest_write_file(PackageManifest* manifest, const char* filename) {
    FILE* f = fopen(filename, "w");
    if (!f) return false;
    
    fprintf(f, "{\n");
    fprintf(f, "  \"name\": \"%s\",\n", manifest->name ? manifest->name : "my-package");
    fprintf(f, "  \"version\": \"%s\",\n", manifest->version ? manifest->version : "1.0.0");
    
    if (manifest->description)
        fprintf(f, "  \"description\": \"%s\",\n", manifest->description);
    if (manifest->author)
        fprintf(f, "  \"author\": \"%s\",\n", manifest->author);
    if (manifest->license)
        fprintf(f, "  \"license\": \"%s\",\n", manifest->license);
    if (manifest->main)
        fprintf(f, "  \"main\": \"%s\",\n", manifest->main);
    
    /* Dependencies */
    fprintf(f, "  \"dependencies\": {");
    if (manifest->dependency_count > 0) {
        fprintf(f, "\n");
        for (int i = 0; i < manifest->dependency_count; i++) {
            fprintf(f, "    \"%s\": \"%s\"%s\n",
                   manifest->dependencies[i].name,
                   manifest->dependencies[i].version_spec,
                   i < manifest->dependency_count - 1 ? "," : "");
        }
        fprintf(f, "  },\n");
    } else {
        fprintf(f, "},\n");
    }
    
    /* Dev dependencies */
    fprintf(f, "  \"devDependencies\": {");
    if (manifest->dev_dependency_count > 0) {
        fprintf(f, "\n");
        for (int i = 0; i < manifest->dev_dependency_count; i++) {
            fprintf(f, "    \"%s\": \"%s\"%s\n",
                   manifest->dev_dependencies[i].name,
                   manifest->dev_dependencies[i].version_spec,
                   i < manifest->dev_dependency_count - 1 ? "," : "");
        }
        fprintf(f, "  }\n");
    } else {
        fprintf(f, "}\n");
    }
    
    fprintf(f, "}\n");
    fclose(f);
    
    return true;
}

PackageManifest* manifest_create_default(const char* name) {
    PackageManifest* m = (PackageManifest*)calloc(1, sizeof(PackageManifest));
    
    m->name = strdup(name ?name : "my-package");
    m->version = strdup("1.0.0");
    m->description = strdup("A Casprix package");
    m->license = strdup("MIT");
    m->main = strdup("src/main.cpx");
    
    return m;
}

void manifest_free(PackageManifest* manifest) {
    if (!manifest) return;
    
    free(manifest->name);
    free(manifest->version);
    free(manifest->description);
    free(manifest->author);
    free(manifest->license);
    free(manifest->main);
    free(manifest->repository);
    
    for (int i = 0; i < manifest->dependency_count; i++) {
        free(manifest->dependencies[i].name);
        free(manifest->dependencies[i].version_spec);
    }
    free(manifest->dependencies);
    
    for (int i = 0; i < manifest->dev_dependency_count; i++) {
        free(manifest->dev_dependencies[i].name);
        free(manifest->dev_dependencies[i].version_spec);
    }
    free(manifest->dev_dependencies);
    
    for (int i = 0; i < manifest->file_count; i++) {
        free(manifest->files[i]);
    }
    free(manifest->files);
    
    for (int i = 0; i < manifest->keyword_count; i++) {
        free(manifest->keywords[i]);
    }
    free(manifest->keywords);
    
    free(manifest);
}

bool manifest_validate(PackageManifest* manifest) {
    if (!manifest) {
        fprintf(stderr, "Manifest is NULL\n");
        return false;
    }
    
    if (!manifest->name || strlen(manifest->name) == 0) {
        fprintf(stderr, "Package name is required\n");
        return false;
    }
    
    if (!manifest->version || strlen(manifest->version) == 0) {
        fprintf(stderr, "Package version is required\n");
        return false;
    }
    
    return true;
}

void manifest_add_dependency(PackageManifest* manifest, 
                             const char* name, const char* version_spec,
                             bool is_dev) {
    if (!manifest || !name || !version_spec) return;
    
    ManifestDependency** deps = is_dev ? &manifest->dev_dependencies : &manifest->dependencies;
    int* count = is_dev ? &manifest->dev_dependency_count : &manifest->dependency_count;
    
    *deps = (ManifestDependency*)realloc(*deps, (*count + 1) * sizeof(ManifestDependency));
    (*deps)[*count].name = strdup(name);
    (*deps)[*count].version_spec = strdup(version_spec);
    (*count)++;
}

void manifest_remove_dependency(PackageManifest* manifest, const char* name) {
    if (!manifest || !name) return;
    
    /* Remove from dependencies */
    for (int i = 0; i < manifest->dependency_count; i++) {
        if (strcmp(manifest->dependencies[i].name, name) == 0) {
            free(manifest->dependencies[i].name);
            free(manifest->dependencies[i].version_spec);
            
            for (int j = i; j < manifest->dependency_count - 1; j++) {
                manifest->dependencies[j] = manifest->dependencies[j + 1];
            }
            manifest->dependency_count--;
            return;
        }
    }
    
    /* Remove from dev dependencies */
    for (int i = 0; i < manifest->dev_dependency_count; i++) {
        if (strcmp(manifest->dev_dependencies[i].name, name) == 0) {
            free(manifest->dev_dependencies[i].name);
            free(manifest->dev_dependencies[i].version_spec);
            
            for (int j = i; j < manifest->dev_dependency_count - 1; j++) {
                manifest->dev_dependencies[j] = manifest->dev_dependencies[j + 1];
            }
            manifest->dev_dependency_count--;
            return;
        }
    }
}
