/**
 * Semantic Versioning Parser for Casperix Package Manager
 * 
 * Implements semver 2.0.0 specification with constraint matching
 */

#ifndef PKG_SEMVER_H
#define PKG_SEMVER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Version Structure
 * ======================================================================== */

typedef struct {
    int major;
    int minor;
    int patch;
    char* prerelease;   /* e.g., "alpha.1", "beta.2" */
    char* build;        /* e.g., "20130313144700" */
} SemVer;

/* ========================================================================
 * Version Parsing
 * ======================================================================== */

/**
 * Parse version string
 * Examples: "1.2.3", "2.0.0-alpha.1", "1.0.0+build.123"
 */
SemVer* semver_parse(const char* version_str);

/**
 * Free version struct
 */
void semver_free(SemVer* ver);

/**
 * Convert version to string
 * Caller must free returned string
 */
char* semver_to_string(SemVer* ver);

/* ========================================================================
 * Version Comparison
 * ======================================================================== */

/**
 * Compare two versions
 * Returns: <0 if a < b, 0 if a == b, >0 if a > b
 */
int semver_compare(SemVer* a, SemVer* b);

/**
 * Check if versions are equal
 */
bool semver_equals(SemVer* a, SemVer* b);

/* ========================================================================
 * Constraint Matching
 * ======================================================================== */

/**
 * Check if version satisfies constraint
 * 
 * Supported constraints:
 *   "1.2.3"     - Exact version
 *   "^1.2.3"    - Compatible (1.2.3 <= x < 2.0.0)
 *   "~1.2.3"    - Approximately (1.2.3 <= x < 1.3.0)
 *   ">=1.2.3"   - Greater or equal
 *   ">1.2.3"    - Greater than
 *   "<=1.2.3"   - Less or equal
 *   "<1.2.3"    - Less than
 *   "1.2.x"     - Wildcard (1.2.0 <= x < 1.3.0)
 *   "*"         - Any version
 */
bool semver_satisfies(SemVer* version, const char* constraint);

/**
 * Find the highest version from a list that satisfies constraint
 * Returns: index of best version, or -1 if none match
 */
int semver_max_satisfying(SemVer** versions, int count, const char* constraint);

#ifdef __cplusplus
}
#endif

#endif /* PKG_SEMVER_H */
