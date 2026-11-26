#include "compiler/codegen/optimizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static Expr* integer_literal(int64_t value, DataType type) {
    LiteralExpr literal = {0};
    literal.type = TYPE_INT;
    literal.value.int_value = value;
    Expr* expr = create_literal_expr(literal, 4, 7);
    expr->data_type = type;
    return expr;
}

static Expr* integer_binary(int64_t lhs, int64_t rhs, CpxTokenType op, DataType type) {
    Expr* expr = create_binary_expr(integer_literal(lhs, type), op,
                                    integer_literal(rhs, type), 4, 7);
    expr->data_type = type;
    return expr;
}

int main(void) {
    OptimizerContext ctx;
    init_optimizer(&ctx);

    Expr* folded = fold_constants(integer_binary(127, 1, TOKEN_PLUS, TYPE_I8), &ctx);
    CHECK(folded->type == EXPR_LITERAL);
    CHECK(folded->data_type == TYPE_I8);
    CHECK(folded->as.literal.value.int_value == -128);
    CHECK(folded->line == 4 && folded->column == 7);
    free_expr(folded);

    folded = fold_constants(integer_binary(255, 1, TOKEN_PLUS, TYPE_U8), &ctx);
    CHECK(folded->type == EXPR_LITERAL);
    CHECK(folded->as.literal.value.int_value == 0);
    free_expr(folded);

    folded = fold_constants(integer_binary(0xF0, 0x0F, TOKEN_BITXOR, TYPE_U8), &ctx);
    CHECK(folded->type == EXPR_LITERAL && folded->as.literal.value.int_value == 0xFF);
    free_expr(folded);

    Expr* bitnot = create_unary_expr(TOKEN_BITNOT, integer_literal(0, TYPE_I8), 6, 3);
    bitnot->data_type = TYPE_I8;
    folded = fold_constants(bitnot, &ctx);
    CHECK(folded->type == EXPR_LITERAL && folded->as.literal.value.int_value == -1);
    free_expr(folded);

    folded = fold_constants(integer_binary(-8, 1, TOKEN_RSHIFT, TYPE_I8), &ctx);
    CHECK(folded->type == EXPR_LITERAL && folded->as.literal.value.int_value == -4);
    free_expr(folded);

    Expr* invalid = integer_binary(1, 0, TOKEN_SLASH, TYPE_I32);
    Expr* preserved = fold_constants(invalid, &ctx);
    CHECK(preserved == invalid && preserved->type == EXPR_BINARY);
    free_expr(preserved);

    Expr* invalid_child = integer_binary(1, 0, TOKEN_SLASH, TYPE_I32);
    Expr* comparison = create_binary_expr(invalid_child, TOKEN_EQUAL,
        integer_literal(1, TYPE_I32), 5, 2);
    comparison->data_type = TYPE_BOOL;
    preserved = fold_constants(comparison, &ctx);
    CHECK(preserved == comparison && preserved->type == EXPR_BINARY);
    free_expr(preserved);

    invalid = integer_binary(-128, -1, TOKEN_PERCENT, TYPE_I8);
    preserved = fold_constants(invalid, &ctx);
    CHECK(preserved == invalid && preserved->type == EXPR_BINARY);
    free_expr(preserved);

    invalid = integer_binary(1, 8, TOKEN_LSHIFT, TYPE_I8);
    preserved = fold_constants(invalid, &ctx);
    CHECK(preserved == invalid && preserved->type == EXPR_BINARY);
    free_expr(preserved);

    char* name = malloc(sizeof("call_result"));
    strcpy(name, "call_result");
    Expr* side_effecting = create_binary_expr(create_variable_expr(name, 2, 1),
        TOKEN_STAR, integer_literal(0, TYPE_I32), 2, 1);
    side_effecting->data_type = TYPE_I32;
    Expr* simplified = simplify_expr(side_effecting, &ctx);
    CHECK(simplified == side_effecting && simplified->type == EXPR_BINARY);
    free_expr(simplified);

    printf("integer AST optimizer: %d assertions, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
