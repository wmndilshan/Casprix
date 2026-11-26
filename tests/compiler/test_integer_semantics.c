#include "compiler/ir/integer_semantics.h"
#include <inttypes.h>
#include <stdio.h>

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_width(unsigned w) {
    uint64_t mask = cpx_int_mask(w), sign = UINT64_C(1) << (w - 1);
    CHECK(cpx_int_width_valid(w));
    CHECK(cpx_int_normalize(mask, w, false) == mask);
    CHECK(cpx_int_normalize(sign, w, true) == (w == 64 ? sign : (UINT64_MAX ^ (sign - 1))));
    CHECK(cpx_int_add(mask, 1, w) == 0);
    CpxIntResult checked_add = cpx_int_add_checked(mask, 1, w);
    CHECK(checked_add.status == CPX_INT_OK && checked_add.bits == 0);
    CHECK(cpx_int_sub(0, 1, w) == mask);
    CHECK(cpx_int_mul(mask, mask, w) == 1);
    CHECK(cpx_int_add(sign - 1, 1, w) == sign);
    CHECK(cpx_int_sub(0, sign, w) == sign);
    CHECK(cpx_int_mul(3, 5, w) == 15);
    CHECK(cpx_int_negate(1, w) == mask);
    CHECK(cpx_int_negate(sign, w) == sign);
    CHECK(cpx_int_bitwise(0, 0, w, '~') == mask);
    CHECK(cpx_int_bitwise(mask, sign, w, '&') == sign);
    CHECK(cpx_int_bitwise(sign, 1, w, '|') == (sign | 1));
    CHECK(cpx_int_bitwise(mask, 1, w, '^') == (mask ^ 1));
    CHECK(cpx_int_zero_extend(sign, w) == sign);
    CHECK(cpx_int_sign_extend(sign, w) == cpx_int_normalize(sign, w, true));
    CHECK(cpx_int_bits_to_i64(cpx_int_i64_to_bits(INT64_MIN)) == INT64_MIN);
    CHECK(cpx_int_compare(sign, 0, w, true) < 0);
    CHECK(cpx_int_compare(mask, 0, w, false) > 0);
    CpxIntResult d = cpx_int_divmod((uint64_t)-84, 7, w, true, false);
    CHECK(d.status == CPX_INT_OK);
    CHECK(d.bits == cpx_int_normalize((uint64_t)-12, w, true));
    CpxIntResult r = cpx_int_divmod((uint64_t)-85, 7, w, true, true);
    CHECK(r.status == CPX_INT_OK && r.bits == cpx_int_normalize(UINT64_MAX, w, true));
    d = cpx_int_divmod(mask, 2, w, false, false);
    CHECK(d.status == CPX_INT_OK && d.bits == mask / 2);
    r = cpx_int_divmod(mask, 10, w, false, true);
    CHECK(r.status == CPX_INT_OK && r.bits == mask % 10);
    CHECK(cpx_int_divmod(1, 0, w, true, false).status == CPX_INT_DIVISION_BY_ZERO);
    CHECK(cpx_int_divmod(1, 0, w, false, true).status == CPX_INT_REMAINDER_BY_ZERO);
    CHECK(cpx_int_divmod(sign, mask, w, true, false).status == CPX_INT_SIGNED_OVERFLOW);
    CHECK(cpx_int_divmod(sign, mask, w, true, true).status == CPX_INT_SIGNED_OVERFLOW);
    CHECK(cpx_int_shift(1, 0, w, false, true).bits == 1);
    CHECK(cpx_int_shift(1, w - 1, w, false, true).bits == sign);
    CHECK(cpx_int_shift(sign - 1, 1, w, false, true).bits == mask - 1);
    CHECK(cpx_int_shift(sign, 1, w, true, false).bits == (sign | (sign >> 1)));
    CHECK(cpx_int_shift(sign, 1, w, false, false).bits == sign >> 1);
    CHECK(cpx_int_shift(1, -1, w, false, true).status == CPX_INT_INVALID_SHIFT);
    CHECK(cpx_int_shift(1, w, w, false, true).status == CPX_INT_INVALID_SHIFT);
    CHECK(cpx_int_shift(1, (int64_t)w + 1, w, false, true).status == CPX_INT_INVALID_SHIFT);
    CHECK(cpx_int_shift(1, INT64_MAX, w, false, true).status == CPX_INT_INVALID_SHIFT);
    CHECK(cpx_int_shift(1, INT64_MIN, w, false, true).status == CPX_INT_INVALID_SHIFT);
}

int main(void) {
    test_width(8); test_width(16); test_width(32); test_width(64);
    CHECK(!cpx_int_width_valid(128));
    CHECK(!cpx_int_width_valid(0));
    CHECK(cpx_int_divmod(1, 1, 128, false, false).status == CPX_INT_UNSUPPORTED_WIDTH);
    CHECK(cpx_int_shift(1, 0, 0, false, false).status == CPX_INT_UNSUPPORTED_WIDTH);
    CHECK(cpx_int_add_checked(1, 1, 128).status == CPX_INT_UNSUPPORTED_WIDTH);
    CHECK(cpx_int_sub_checked(1, 1, 0).status == CPX_INT_UNSUPPORTED_WIDTH);
    CHECK(cpx_int_mul_checked(1, 1, 128).status == CPX_INT_UNSUPPORTED_WIDTH);
    CHECK(cpx_int_negate_checked(1, 0).status == CPX_INT_UNSUPPORTED_WIDTH);
    CHECK(cpx_int_bitwise_checked(1, 1, 128, '&').status == CPX_INT_UNSUPPORTED_WIDTH);
    printf("integer semantics: %d assertions, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
