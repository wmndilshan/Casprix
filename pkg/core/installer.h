/**
 * Package Installer - Download, Extract, Link
 */

#ifndef PKG_INSTALLER_H
#define PKG_INSTALLER_H

#include "manifest.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Installation
 * ======================================================================== */

/**
 * Install package from manifest
 * @param manifest_path Path to casper.json
 * @param dev_deps Also install devDependencies
 */
bool pkg_install_from_manifest(const char* manifest_path, bool dev_deps);

/**
 * Install specific package
 * @param name Package name
 * @param version_spec Version constraint (e.g., "^1.2.0")
 */
bool pkg_install_package(const char* name, const char* version_spec);

/**
 * Uninstall package
 */
bool pkg_uninstall(const char* name);

/**
 * Update all dependencies to latest versions
 */
bool pkg_update_all(void);

/* ========================================================================
 * Download & Extract
 * ======================================================================== */

/**
 * Download package tarball from URL
 * @param url Download URL
 * @param dest Destination file path
 */
bool pkg_download(const char* url, const char* dest);

/**
 * Extract tarball to directory
 * @param tarball Path to .tar.gz file
 * @param dest_dir Destination directory
 */
bool pkg_extract(const char* tarball, const char* dest_dir);

/**
 * Verify package checksum
 * @param file Package file path
 * @param expected_sha256 Expected SHA256 hash
 */
bool pkg_verify_checksum(const char* file, const char* expected_sha256);

#ifdef __cplusplus
}
#endif

#endif /* PKG_INSTALLER_H */
