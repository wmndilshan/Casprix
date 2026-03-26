/**
 * Local Package Cache Manager
 */

#ifndef PKG_CACHE_H
#define PKG_CACHE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Cache Management
 * ======================================================================== */

/**
 * Get cache directory path
 * Returns: ~/.cpkg/cache/ on Unix, %USERPROFILE%\.cpkg\cache\ on Windows
 */
const char* pkg_cache_dir(void);

/**
 * Initialize cache directory (create if needed)
 */
bool pkg_cache_init(void);

/**
 * Check if package version is cached
 */
bool pkg_cache_has(const char* name, const char* version);

/**
 * Get path to cached package
 * Caller must free returned string
 */
char* pkg_cache_get_path(const char* name, const char* version);

/**
 * Store package in cache
 * @param tarball Path to package tarball
 * Returns: path to cached directory
 */
char* pkg_cache_put(const char* name, const char* version, const char* tarball);

/**
 * Remove package from cache
 */
bool pkg_cache_remove(const char* name, const char* version);

/**
 * Clean entire cache
 */
bool pkg_cache_clean(void);

/**
 * List all cached packages
 * Returns: array of "name@version" strings
 */
char** pkg_cache_list(int* count);

/**
 * Get cache size in bytes
 */
long long pkg_cache_size(void);

#ifdef __cplusplus
}
#endif

#endif /* PKG_CACHE_H */
