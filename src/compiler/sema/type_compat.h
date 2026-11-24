/*
 * Casprix — shared type compatibility / convertibility APIs (Phase 1).
 *
 * Distinguishes identity, assignment compatibility, and implicit numeric
 * convertibility so semantic analysis is not a single boolean free-for-all.
 */
#ifndef CASPRIX_TYPE_COMPAT_H
#define CASPRIX_TYPE_COMPAT_H

#include "compiler/frontend/ast.h"
#include <stdbool.h>

typedef enum {
    TYPE_CONV_IDENTICAL = 0,   /* same DataType */
    TYPE_CONV_IMPLICIT,        /* safe widening / defined implicit conversion */
    TYPE_CONV_EXPLICIT_ONLY,   /* requires an explicit cast */
    TYPE_CONV_INCOMPATIBLE,    /* never convertible */
    TYPE_CONV_ERROR_RECOVERY,  /* involves TYPE_ERROR — suppress cascade only */
} TypeConvKind;

/* Coarse DataType equality (no TypeInfo). */
bool type_equal(DataType a, DataType b);

/* Assignment / argument / return compatibility (implicit conversions only). */
bool type_assignable(DataType from, DataType to);

/* Classify the conversion from `from` to `to`. */
TypeConvKind type_conversion_kind(DataType from, DataType to);

/* True when `from` may implicitly convert to `to` (widening etc.). */
bool type_implicitly_convertible(DataType from, DataType to);

/*
 * Legacy name used throughout semantic.c.
 * Equivalent to type_assignable, except TYPE_ERROR pairs return true for
 * cascade suppression (callers must still honor had_error / refuse codegen).
 */
bool types_compatible(DataType t1, DataType t2);

/* Structured TypeInfo checks (element types, function signatures, etc.). */
bool typeinfo_assignable(const TypeInfo* from, const TypeInfo* to);

/* Integer / float rank helpers used by conversion policy. */
int type_int_rank(DataType t);   /* -1 if not integer */
int type_float_rank(DataType t); /* -1 if not float */

#endif /* CASPRIX_TYPE_COMPAT_H */
