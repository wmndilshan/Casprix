#include "compiler/sema/type_compat.h"
#include <stddef.h>

int type_int_rank(DataType t) {
    switch (t) {
        case TYPE_I8:  case TYPE_U8:  return 1;
        case TYPE_I16: case TYPE_U16: return 2;
        case TYPE_I32: case TYPE_U32: case TYPE_CHAR: return 3;
        case TYPE_I64: case TYPE_U64: return 4;
        case TYPE_I128: case TYPE_U128: return 5;
        default: return -1;
    }
}

int type_float_rank(DataType t) {
    switch (t) {
        case TYPE_F16: case TYPE_BF16: return 1;
        case TYPE_F32: return 2;
        case TYPE_F64: return 3;
        default: return -1;
    }
}

bool type_equal(DataType a, DataType b) {
    return a == b;
}

static bool is_signed_int(DataType t) {
    return type_is_signed(t) || t == TYPE_INT || t == TYPE_CHAR;
}

static bool is_unsigned_int(DataType t) {
    return type_is_unsigned(t);
}

TypeConvKind type_conversion_kind(DataType from, DataType to) {
    if (from == TYPE_ERROR || to == TYPE_ERROR)
        return TYPE_CONV_ERROR_RECOVERY;

    if (from == to)
        return TYPE_CONV_IDENTICAL;

    /* Unresolved generics must not silently match concrete types. */
    if (from == TYPE_GENERIC || to == TYPE_GENERIC) {
        if (from == TYPE_GENERIC && to == TYPE_GENERIC)
            return TYPE_CONV_IDENTICAL;
        return TYPE_CONV_INCOMPATIBLE;
    }

    if (from == TYPE_DYN || to == TYPE_DYN)
        return TYPE_CONV_INCOMPATIBLE;

    /* Integer widening, same signedness (char treated as signed i32-rank). */
    {
        int rf = type_int_rank(from);
        int rt = type_int_rank(to);
        if (rf >= 0 && rt >= 0) {
            bool from_u = is_unsigned_int(from);
            bool to_u = is_unsigned_int(to);
            /* char ↔ signed integers: allow as implicit at same or wider rank */
            if (from == TYPE_CHAR && is_signed_int(to) && rt >= rf)
                return TYPE_CONV_IMPLICIT;
            if (to == TYPE_CHAR && is_signed_int(from) && rf <= rt)
                return (rf == rt) ? TYPE_CONV_IDENTICAL : TYPE_CONV_IMPLICIT;

            if (from_u != to_u && from != TYPE_CHAR && to != TYPE_CHAR)
                return TYPE_CONV_EXPLICIT_ONLY; /* signedness change */

            if (rt > rf)
                return TYPE_CONV_IMPLICIT; /* widen */
            if (rt == rf)
                return TYPE_CONV_IDENTICAL;
            return TYPE_CONV_EXPLICIT_ONLY; /* narrow */
        }
    }

    /* Float widening */
    {
        int rf = type_float_rank(from);
        int rt = type_float_rank(to);
        if (rf >= 0 && rt >= 0) {
            if (rt > rf) return TYPE_CONV_IMPLICIT;
            if (rt == rf) return TYPE_CONV_IDENTICAL;
            return TYPE_CONV_EXPLICIT_ONLY;
        }
    }

    /* Integer → float: allow when float rank can hold the integer magnitude
     * conservatively (i32/i16/i8/u16/u8 → f32/f64; i64/u32 → f64 only). */
    if (type_is_integer(from) && type_is_float(to)) {
        int ir = type_int_rank(from);
        int fr = type_float_rank(to);
        if (fr >= 3) /* f64 */
            return TYPE_CONV_IMPLICIT;
        if (fr >= 2 && ir >= 0 && ir <= 3) /* f32 from up to i32-rank */
            return TYPE_CONV_IMPLICIT;
        return TYPE_CONV_EXPLICIT_ONLY;
    }

    /* Float → integer always explicit */
    if (type_is_float(from) && type_is_integer(to))
        return TYPE_CONV_EXPLICIT_ONLY;

    /* string ↔ strbuf */
    if ((from == TYPE_STRING && to == TYPE_STRBUF) ||
        (from == TYPE_STRBUF && to == TYPE_STRING))
        return TYPE_CONV_IMPLICIT;

    /* class/struct → ref (implicit reborrow of instance) */
    if ((from == TYPE_CLASS || from == TYPE_STRUCT) && to == TYPE_REF)
        return TYPE_CONV_IMPLICIT;

    /* Integer 0 / integer → pointer for FFI null (explicit-preferring but
     * historically allowed; keep as IMPLICIT for rawptr/ptr only). */
    if (type_is_integer(from) && (to == TYPE_RAWPTR || to == TYPE_PTR))
        return TYPE_CONV_IMPLICIT;
    if (type_is_integer(to) && (from == TYPE_RAWPTR || from == TYPE_PTR))
        return TYPE_CONV_EXPLICIT_ONLY;

    /* Pointer family */
    if (type_is_pointer(from) && type_is_pointer(to)) {
        if (from == to) return TYPE_CONV_IDENTICAL;
        /* rawptr ↔ ptr: explicit; ref is not freely interchangeable with rawptr */
        if ((from == TYPE_RAWPTR && to == TYPE_PTR) ||
            (from == TYPE_PTR && to == TYPE_RAWPTR))
            return TYPE_CONV_IMPLICIT;
        return TYPE_CONV_EXPLICIT_ONLY;
    }

    return TYPE_CONV_INCOMPATIBLE;
}

bool type_implicitly_convertible(DataType from, DataType to) {
    TypeConvKind k = type_conversion_kind(from, to);
    return k == TYPE_CONV_IDENTICAL || k == TYPE_CONV_IMPLICIT ||
           k == TYPE_CONV_ERROR_RECOVERY;
}

bool type_assignable(DataType from, DataType to) {
    return type_implicitly_convertible(from, to);
}

bool types_compatible(DataType t1, DataType t2) {
    /* Historical call sites pass (actual, expected) or (a, b) symmetrically
     * for some checks. Assignment uses (initializer, decl_type) → from, to.
     * We keep a directed check: t1 assignable to t2. For symmetric numeric
     * "are these related" call sites that swapped args, also allow t2→t1
     * only when both are numeric and either direction is implicit identical
     * — but NOT for narrowing. Directed assignable is the Phase 1 contract. */
    return type_assignable(t1, t2);
}

bool typeinfo_assignable(const TypeInfo* from, const TypeInfo* to) {
    if (!from || !to) return true; /* missing info → fall back to DataType only */
    if (from->base != to->base) {
        return type_assignable(from->base, to->base);
    }
    if (from->base == TYPE_ARRAY || from->base == TYPE_SLICE ||
        from->base == TYPE_STATIC_ARRAY) {
        if (from->element_type && to->element_type) {
            if (!typeinfo_assignable(from->element_type, to->element_type))
                return false;
        }
        if (from->base == TYPE_STATIC_ARRAY &&
            from->static_size > 0 && to->static_size > 0 &&
            from->static_size != to->static_size)
            return false;
        return true;
    }
    if (from->base == TYPE_FUNC) {
        if (from->param_count != to->param_count) return false;
        if (from->return_type && to->return_type &&
            !typeinfo_assignable(from->return_type, to->return_type))
            return false;
        for (int i = 0; i < from->param_count; i++) {
            if (from->param_types && to->param_types &&
                from->param_types[i] && to->param_types[i] &&
                !typeinfo_assignable(to->param_types[i], from->param_types[i]))
                return false; /* params are contravariant-ish: expected→actual */
        }
        return true;
    }
    return true;
}
