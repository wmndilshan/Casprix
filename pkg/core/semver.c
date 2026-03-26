/**
 * Semantic Versioning Implementation
 */

#include "semver.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ========================================================================
 * Version Parsing
 * ======================================================================== */

SemVer* semver_parse(const char* version_str) {
    if (!version_str) return NULL;
    
    SemVer* ver = (SemVer*)calloc(1, sizeof(SemVer));
    
    /* Skip leading 'v' if present */
    const char* s = version_str;
    if (*s == 'v' || *s == 'V') s++;
    
    /* Parse major.minor.patch */
    if (sscanf(s, "%d.%d.%d", &ver->major, &ver->minor, &ver->patch) < 3) {
        /* Try just major.minor */
        if (sscanf(s, "%d.%d", &ver->major, &ver->minor) < 2) {
            /* Try just major */
            if (sscanf(s, "%d", &ver->major) < 1) {
                free(ver);
                return NULL;
            }
        }
    }
    
    /* Find prerelease (after '-') */
    const char* dash = strchr(s, '-');
    const char* plus = strchr(s, '+');
    
    if (dash) {
        const char* end = plus ? plus : s + strlen(s);
        int len = end - dash - 1;
        ver->prerelease = (char*)malloc(len + 1);
        strncpy(ver->prerelease, dash + 1, len);
        ver->prerelease[len] = '\0';
    }
    
    /* Find build metadata (after '+') */
    if (plus) {
        ver->build = strdup(plus + 1);
    }
    
    return ver;
}

void semver_free(SemVer* ver) {
    if (!ver) return;
    free(ver->prerelease);
    free(ver->build);
    free(ver);
}

char* semver_to_string(SemVer* ver) {
    if (!ver) return NULL;
    
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "%d.%d.%d", 
                      ver->major, ver->minor, ver->patch);
    
    if (ver->prerelease) {
        len += snprintf(buf + len, sizeof(buf) - len, "-%s", ver->prerelease);
    }
    
    if (ver->build) {
        snprintf(buf + len, sizeof(buf) - len, "+%s", ver->build);
    }
    
    return strdup(buf);
}

/* ========================================================================
 * Version Comparison
 * ======================================================================== */

int semver_compare(SemVer* a, SemVer* b) {
    if (!a || !b) return 0;
    
    /* Compare major */
    if (a->major != b->major) return a->major - b->major;
    
    /* Compare minor */
    if (a->minor != b->minor) return a->minor - b->minor;
    
    /* Compare patch */
    if (a->patch != b->patch) return a->patch - b->patch;
    
    /* Compare prerelease (versions without prerelease are > versions with) */
    if (!a->prerelease && !b->prerelease) return 0;
    if (!a->prerelease) return 1;   /* a is stable, b is prerelease */
    if (!b->prerelease) return -1;  /* a is prerelease, b is stable */
    
    return strcmp(a->prerelease, b->prerelease);
}

bool semver_equals(SemVer* a, SemVer* b) {
    return semver_compare(a, b) == 0;
}

/* ========================================================================
 * Constraint Matching
 * ======================================================================== */

static bool satisfies_caret(SemVer* ver, SemVer* base) {
    /* ^1.2.3 means >= 1.2.3 and < 2.0.0 */
    if (ver->major != base->major) return false;
    
    if (base->major > 0) {
        /* Normal version: allow minor/patch changes */
        return semver_compare(ver, base) >= 0 && ver->major == base->major;
    } else if (base->minor > 0) {
        /* 0.x.y: allow patch changes only */
        return semver_compare(ver, base) >= 0 && 
               ver->major == 0 && ver->minor == base->minor;
    } else {
        /* 0.0.x: exact match only */
        return semver_equals(ver, base);
    }
}

static bool satisfies_tilde(SemVer* ver, SemVer* base) {
    /* ~1.2.3 means >= 1.2.3 and < 1.3.0 */
    if (ver->major != base->major) return false;
    if (ver->minor != base->minor) return false;
    
    return semver_compare(ver, base) >= 0;
}

bool semver_satisfies(SemVer* version, const char* constraint) {
    if (!version || !constraint) return false;
    
    /* Wildcard - any version */
    if (strcmp(constraint, "*") == 0) return true;
    
    /* Skip whitespace */
    while (isspace(*constraint)) constraint++;
    
    /* Caret constraint (^1.2.3) */
    if (*constraint == '^') {
        SemVer* base = semver_parse(constraint + 1);
        bool result = satisfies_caret(version, base);
        semver_free(base);
        return result;
    }
    
    /* Tilde constraint (~1.2.3) */
    if (*constraint == '~') {
        SemVer* base = semver_parse(constraint + 1);
        bool result = satisfies_tilde(version, base);
        semver_free(base);
        return result;
    }
    
    /* Greater than or equal (>=1.2.3) */
    if (strncmp(constraint, ">=", 2) == 0) {
        SemVer* base = semver_parse(constraint + 2);
        bool result = semver_compare(version, base) >= 0;
        semver_free(base);
        return result;
    }
    
    /* Greater than (>1.2.3) */
    if (*constraint == '>') {
        SemVer* base = semver_parse(constraint + 1);
        bool result = semver_compare(version, base) > 0;
        semver_free(base);
        return result;
    }
    
    /* Less than or equal (<=1.2.3) */
    if (strncmp(constraint, "<=", 2) == 0) {
        SemVer* base = semver_parse(constraint + 2);
        bool result = semver_compare(version, base) <= 0;
        semver_free(base);
        return result;
    }
    
    /* Less than (<1.2.3) */
    if (*constraint == '<') {
        SemVer* base = semver_parse(constraint + 1);
        bool result = semver_compare(version, base) < 0;
        semver_free(base);
        return result;
    }
    
    /* Wildcard version (1.2.x) */
    if (strchr(constraint, 'x') || strchr(constraint, 'X')) {
        char buf[64];
        strncpy(buf, constraint, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        
        /* Replace x with 0 for parsing */
        for (char* p = buf; *p; p++) {
            if (*p == 'x' || *p == 'X') *p = '0';
        }
        
        SemVer* base = semver_parse(buf);
        bool result = false;
        
        if (strchr(constraint, 'x') - constraint < 2) {
            /* x.y.z - any version */
            result = true;
        } else if (strchr(constraint, 'x') - constraint < 4) {
            /* 1.x.y - match major */
            result = (version->major == base->major);
        } else {
            /* 1.2.x - match major.minor */
            result = (version->major == base->major && 
                     version->minor == base->minor);
        }
        
        semver_free(base);
        return result;
    }
    
    /* Exact version */
    SemVer* base = semver_parse(constraint);
    bool result = semver_equals(version, base);
    semver_free(base);
    return result;
}

int semver_max_satisfying(SemVer** versions, int count, const char* constraint) {
    int best_idx = -1;
    
    for (int i = 0; i < count; i++) {
        if (!semver_satisfies(versions[i], constraint)) continue;
        
        if (best_idx == -1 || 
            semver_compare(versions[i], versions[best_idx]) > 0) {
            best_idx = i;
        }
    }
    
    return best_idx;
}
