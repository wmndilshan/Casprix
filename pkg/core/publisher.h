/**
 * Package Publisher - Create and Publish Packages
 */

#ifndef PKG_PUBLISHER_H
#define PKG_PUBLISHER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Package Creation
 * ======================================================================== */

/**
 * Initialize new package (create casper.json)
 * @param name Package name
 * @param path Directory path (defaults to current directory)
 */
bool pkg_init(const char* name, const char* path);

/**
 * Create package tarball from manifest
 * @param manifest_path Path to casper.json
 * @param output_path Output tarball path (if NULL, auto-generated)
 * Returns: path to created tarball (caller must free)
 */
char* pkg_pack(const char* manifest_path, const char* output_path);

/* ========================================================================
 * Publishing
 * ======================================================================== */

/**
 * Publish package to registry
 * @param tarball Path to package tarball
 * @param registry_url Registry URL (NULL for default)
 * @param api_key Authentication key
 */
bool pkg_publish(const char* tarball, const char* registry_url, const char* api_key);

/**
 * Unpublish package version
 * @param name Package name
 * @param version Version to unpublish
 */
bool pkg_unpublish(const char* name, const char* version);

/* ========================================================================
 * Authentication
 * ======================================================================== */

/**
 * Login to package registry
 * @param username Username or email
 * @param password Password
 * @param registry_url Registry URL (NULL for default)
 * Returns: API key (caller must free)
 */
char* pkg_login(const char* username, const char* password, const char* registry_url);

/**
 * Logout (remove stored credentials)
 */
bool pkg_logout(void);

/**
 * Get stored API key
 * Returns: API key or NULL if not logged in
 */
char* pkg_get_api_key(void);

#ifdef __cplusplus
}
#endif

#endif /* PKG_PUBLISHER_H */
