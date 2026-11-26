#ifndef CASPRIX_INTEGER_SEMANTICS_H
#define CASPRIX_INTEGER_SEMANTICS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum CpxIntStatus {
    CPX_INT_OK = 0,
    CPX_INT_UNSUPPORTED_WIDTH,
    CPX_INT_DIVISION_BY_ZERO,
    CPX_INT_REMAINDER_BY_ZERO,
    CPX_INT_SIGNED_OVERFLOW,
    CPX_INT_INVALID_SHIFT
} CpxIntStatus;

typedef struct CpxIntResult {
    uint64_t bits;
    CpxIntStatus status;
} CpxIntResult;

static inline uint64_t cpx_int_i64_to_bits(int64_t value) {
    /* Encode the language's two's-complement bit pattern without relying on
     * the host representation of negative signed integers. */
    return value >= 0 ? (uint64_t)value
                      : UINT64_MAX - (uint64_t)(-(value + 1));
}

static inline int64_t cpx_int_bits_to_i64(uint64_t bits) {
    if (bits <= (uint64_t)INT64_MAX) return (int64_t)bits;
    return -1 - (int64_t)(UINT64_MAX - bits);
}

static inline bool cpx_int_width_valid(unsigned width) {
    return width == 8 || width == 16 || width == 32 || width == 64;
}

static inline uint64_t cpx_int_mask(unsigned width) {
    if (!cpx_int_width_valid(width)) return 0;
    return width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
}

static inline CpxIntResult cpx_int_result(uint64_t bits, unsigned width) {
    CpxIntResult result = {0, CPX_INT_OK};
    if (!cpx_int_width_valid(width)) result.status = CPX_INT_UNSUPPORTED_WIDTH;
    else result.bits = bits & cpx_int_mask(width);
    return result;
}

static inline uint64_t cpx_int_zero_extend(uint64_t bits, unsigned width) {
    return cpx_int_width_valid(width) ? bits & cpx_int_mask(width) : bits;
}

static inline uint64_t cpx_int_sign_extend(uint64_t bits, unsigned width) {
    if (!cpx_int_width_valid(width)) return bits;
    uint64_t mask = cpx_int_mask(width);
    bits &= mask;
    if (bits & (UINT64_C(1) << (width - 1))) bits |= ~mask;
    return bits;
}

static inline uint64_t cpx_int_normalize(uint64_t bits, unsigned width, bool is_signed) {
    if (!cpx_int_width_valid(width)) return bits;
    return is_signed ? cpx_int_sign_extend(bits, width)
                     : cpx_int_zero_extend(bits, width);
}

static inline uint64_t cpx_int_negate(uint64_t bits, unsigned width) {
    if (!cpx_int_width_valid(width)) return bits;
    return (UINT64_C(0) - bits) & cpx_int_mask(width);
}

static inline uint64_t cpx_int_add(uint64_t a, uint64_t b, unsigned width) {
    if (!cpx_int_width_valid(width)) return 0;
    return (a + b) & cpx_int_mask(width);
}

static inline uint64_t cpx_int_sub(uint64_t a, uint64_t b, unsigned width) {
    if (!cpx_int_width_valid(width)) return 0;
    return (a - b) & cpx_int_mask(width);
}

static inline uint64_t cpx_int_mul(uint64_t a, uint64_t b, unsigned width) {
    if (!cpx_int_width_valid(width)) return 0;
    return (a * b) & cpx_int_mask(width);
}

static inline CpxIntResult cpx_int_add_checked(uint64_t a, uint64_t b, unsigned width) {
    if (!cpx_int_width_valid(width)) return (CpxIntResult){0, CPX_INT_UNSUPPORTED_WIDTH};
    return cpx_int_result(cpx_int_add(a, b, width), width);
}

static inline CpxIntResult cpx_int_sub_checked(uint64_t a, uint64_t b, unsigned width) {
    if (!cpx_int_width_valid(width)) return (CpxIntResult){0, CPX_INT_UNSUPPORTED_WIDTH};
    return cpx_int_result(cpx_int_sub(a, b, width), width);
}

static inline CpxIntResult cpx_int_mul_checked(uint64_t a, uint64_t b, unsigned width) {
    if (!cpx_int_width_valid(width)) return (CpxIntResult){0, CPX_INT_UNSUPPORTED_WIDTH};
    return cpx_int_result(cpx_int_mul(a, b, width), width);
}

static inline CpxIntResult cpx_int_negate_checked(uint64_t value, unsigned width) {
    if (!cpx_int_width_valid(width)) return (CpxIntResult){0, CPX_INT_UNSUPPORTED_WIDTH};
    return cpx_int_result(cpx_int_negate(value, width), width);
}

static inline uint64_t cpx_int_bitwise(uint64_t a, uint64_t b,
                                       unsigned width, char op) {
    if (!cpx_int_width_valid(width)) return 0;
    uint64_t mask = cpx_int_mask(width);
    if (op == '&') return (a & b) & mask;
    if (op == '|') return (a | b) & mask;
    if (op == '^') return (a ^ b) & mask;
    if (op == '~') return (~a) & mask;
    return 0;
}

static inline CpxIntResult cpx_int_bitwise_checked(uint64_t a, uint64_t b,
                                                    unsigned width, char op) {
    if (!cpx_int_width_valid(width)) return (CpxIntResult){0, CPX_INT_UNSUPPORTED_WIDTH};
    return cpx_int_result(cpx_int_bitwise(a, b, width, op), width);
}

static inline int cpx_int_compare(uint64_t a, uint64_t b, unsigned width, bool is_signed) {
    if (!cpx_int_width_valid(width)) return 0;
    uint64_t mask = cpx_int_mask(width);
    a &= mask; b &= mask;
    if (!is_signed) return (a > b) - (a < b);
    uint64_t sign = UINT64_C(1) << (width - 1);
    bool an = (a & sign) != 0, bn = (b & sign) != 0;
    if (an != bn) return an ? -1 : 1;
    return (a > b) - (a < b);
}

static inline uint64_t cpx_int_arshift(uint64_t bits, unsigned count, unsigned width) {
    if (!cpx_int_width_valid(width) || count >= width) return 0;
    uint64_t mask = cpx_int_mask(width);
    bits &= mask;
    if (count == 0) return bits;
    uint64_t shifted = bits >> count;
    if (bits & (UINT64_C(1) << (width - 1)))
        shifted |= mask ^ (mask >> count);
    return shifted & mask;
}

static inline CpxIntResult cpx_int_divmod(uint64_t lhs, uint64_t rhs,
                                           unsigned width, bool is_signed,
                                           bool remainder) {
    CpxIntResult out = {0, CPX_INT_OK};
    if (!cpx_int_width_valid(width)) {
        out.status = CPX_INT_UNSUPPORTED_WIDTH; return out;
    }
    uint64_t mask = cpx_int_mask(width), sign = UINT64_C(1) << (width - 1);
    lhs &= mask; rhs &= mask;
    if (!rhs) {
        out.status = remainder ? CPX_INT_REMAINDER_BY_ZERO : CPX_INT_DIVISION_BY_ZERO;
        return out;
    }
    if (!is_signed) {
        out.bits = remainder ? lhs % rhs : lhs / rhs;
        return out;
    }
    bool lhs_neg = (lhs & sign) != 0, rhs_neg = (rhs & sign) != 0;
    if (lhs == sign && rhs == mask) {
        out.status = CPX_INT_SIGNED_OVERFLOW; return out;
    }
    uint64_t lhs_mag = lhs_neg ? ((UINT64_C(0) - lhs) & mask) : lhs;
    uint64_t rhs_mag = rhs_neg ? ((UINT64_C(0) - rhs) & mask) : rhs;
    uint64_t magnitude = remainder ? lhs_mag % rhs_mag : lhs_mag / rhs_mag;
    bool negative = remainder ? lhs_neg : (lhs_neg != rhs_neg);
    uint64_t result_bits = negative ? ((UINT64_C(0) - magnitude) & mask) : magnitude;
    out.bits = cpx_int_normalize(result_bits, width, true);
    return out;
}

static inline CpxIntResult cpx_int_shift(uint64_t value, int64_t count,
                                         unsigned width, bool arithmetic,
                                         bool left) {
    CpxIntResult out = {0, CPX_INT_OK};
    if (!cpx_int_width_valid(width)) {
        out.status = CPX_INT_UNSUPPORTED_WIDTH; return out;
    }
    if (count < 0 || (uint64_t)count >= width) {
        out.status = CPX_INT_INVALID_SHIFT; return out;
    }
    unsigned n = (unsigned)count;
    uint64_t mask = cpx_int_mask(width);
    value &= mask;
    if (left) out.bits = (value << n) & mask;
    else out.bits = arithmetic ? cpx_int_arshift(value, n, width) : (value >> n);
    return out;
}

#endif
