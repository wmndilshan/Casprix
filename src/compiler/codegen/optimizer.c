// Casprix Compiler Optimizer Implementation

#include "compiler/codegen/optimizer.h"
#include "compiler/ir/integer_semantics.h"
#include "support/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static bool try_eval_integer(Expr* expr, uint64_t* bits) {
    if (!expr || !bits) return false;
    unsigned width = (unsigned)type_size_bytes(expr->data_type) * 8;
    bool is_unsigned = type_is_unsigned(expr->data_type);
    if (!cpx_int_width_valid(width)) return false;
    if (expr->type == EXPR_LITERAL && expr->as.literal.type == TYPE_INT) {
        *bits = (uint64_t)expr->as.literal.value.int_value & cpx_int_mask(width);
        return true;
    }
    if (expr->type == EXPR_UNARY && expr->as.unary.operator == TOKEN_MINUS) {
        uint64_t operand;
        if (!try_eval_integer(expr->as.unary.operand, &operand)) return false;
        *bits = cpx_int_normalize(cpx_int_negate(operand, width), width, !is_unsigned);
        return true;
    }
    if (expr->type == EXPR_UNARY && expr->as.unary.operator == TOKEN_BITNOT) {
        uint64_t operand;
        if (!try_eval_integer(expr->as.unary.operand, &operand)) return false;
        *bits = cpx_int_normalize(cpx_int_bitwise(operand, 0, width, '~'), width, !is_unsigned);
        return true;
    }
    if (expr->type != EXPR_BINARY) return false;
    uint64_t left, right;
    if (!try_eval_integer(expr->as.binary.left, &left) ||
        !try_eval_integer(expr->as.binary.right, &right)) return false;
    switch (expr->as.binary.operator) {
        case TOKEN_PLUS: *bits = cpx_int_normalize(cpx_int_add(left, right, width), width, !is_unsigned); return true;
        case TOKEN_MINUS: *bits = cpx_int_normalize(cpx_int_sub(left, right, width), width, !is_unsigned); return true;
        case TOKEN_STAR: *bits = cpx_int_normalize(cpx_int_mul(left, right, width), width, !is_unsigned); return true;
        case TOKEN_BITAND: *bits = cpx_int_normalize(cpx_int_bitwise(left, right, width, '&'), width, !is_unsigned); return true;
        case TOKEN_PIPE: *bits = cpx_int_normalize(cpx_int_bitwise(left, right, width, '|'), width, !is_unsigned); return true;
        case TOKEN_BITXOR: *bits = cpx_int_normalize(cpx_int_bitwise(left, right, width, '^'), width, !is_unsigned); return true;
        case TOKEN_LSHIFT: case TOKEN_RSHIFT: {
            CpxIntResult shifted = cpx_int_shift(left, cpx_int_bits_to_i64(right), width,
                expr->as.binary.operator == TOKEN_RSHIFT && !is_unsigned,
                expr->as.binary.operator == TOKEN_LSHIFT);
            if (shifted.status != CPX_INT_OK) return false;
            *bits = cpx_int_normalize(shifted.bits, width, !is_unsigned);
            return true;
        }
        case TOKEN_SLASH: case TOKEN_PERCENT: {
            CpxIntResult result = cpx_int_divmod(left, right, width, !is_unsigned,
                expr->as.binary.operator == TOKEN_PERCENT);
            if (result.status != CPX_INT_OK) return false;
            *bits = result.bits; return true;
        }
        default: return false;
    }
}

/* Validate integer subexpressions inside constant boolean expressions too;
 * otherwise the legacy bool evaluator could mistake an invalid child (which
 * returns a zero placeholder) for an ordinary integer constant. */
static bool constant_integer_operations_valid(Expr* expr) {
    if (!expr) return true;
    if (type_is_integer(expr->data_type)) {
        uint64_t ignored;
        return try_eval_integer(expr, &ignored);
    }
    if (expr->type == EXPR_UNARY)
        return constant_integer_operations_valid(expr->as.unary.operand);
    if (expr->type == EXPR_BINARY)
        return constant_integer_operations_valid(expr->as.binary.left) &&
               constant_integer_operations_valid(expr->as.binary.right);
    return true;
}

// Initialize optimizer
void init_optimizer(OptimizerContext* ctx) {
    ctx->optimizations_performed = 0;
    ctx->constants_folded = 0;
    ctx->dead_code_eliminated = 0;
    ctx->expressions_simplified = 0;
    ctx->enable_constant_folding = true;
    ctx->enable_dead_code_elimination = true;
    ctx->enable_common_subexpression = true;
    ctx->enable_inline_expansion = false;  // Conservative default
}

// Check if expression is a compile-time constant
bool is_constant_expr(Expr* expr) {
    if (!expr) return false;

    switch (expr->type) {
        case EXPR_LITERAL:
            return true;

        case EXPR_UNARY: {
            UnaryExpr* unary = &expr->as.unary;
            return is_constant_expr(unary->operand);
        }

        case EXPR_BINARY: {
            BinaryExpr* binary = &expr->as.binary;
            return is_constant_expr(binary->left) && is_constant_expr(binary->right);
        }

        default:
            return false;
    }
}

// Evaluate constant integer expression
int64_t evaluate_constant_int(Expr* expr) {
    if (!expr) return 0;

    /* Keep this legacy value-returning API defined even when callers pass an
     * invalid integer expression. fold_constants uses the checked result
     * directly and leaves such an expression intact. */
    if (type_is_integer(expr->data_type)) {
        uint64_t bits;
        if (!try_eval_integer(expr, &bits)) return 0;
        return cpx_int_bits_to_i64(bits);
    }

    switch (expr->type) {
        case EXPR_LITERAL: {
            LiteralExpr* lit = &expr->as.literal;
            if (lit->type == TYPE_INT) {
                return lit->value.int_value;
            }
            return 0;
        }

        case EXPR_UNARY: {
            UnaryExpr* unary = &expr->as.unary;
            int64_t operand = evaluate_constant_int(unary->operand);

            if (unary->operator == TOKEN_MINUS) {
                return -operand;
            }
            if (unary->operator == TOKEN_NOT) {
                return !operand;
            }
            return operand;
        }

        case EXPR_BINARY: {
            BinaryExpr* binary = &expr->as.binary;
            int64_t left = evaluate_constant_int(binary->left);
            int64_t right = evaluate_constant_int(binary->right);

            switch (binary->operator) {
                case TOKEN_PLUS: return left + right;
                case TOKEN_MINUS: return left - right;
                case TOKEN_STAR: return left * right;
                case TOKEN_SLASH: return right != 0 ? left / right : 0;
                case TOKEN_PERCENT: return right != 0 ? left % right : 0;
                case TOKEN_EQUAL: return left == right;
                case TOKEN_NOT_EQUAL: return left != right;
                case TOKEN_LESS: return left < right;
                case TOKEN_LESS_EQUAL: return left <= right;
                case TOKEN_GREATER: return left > right;
                case TOKEN_GREATER_EQUAL: return left >= right;
                case TOKEN_AND: return left && right;
                case TOKEN_OR: return left || right;
                default: return 0;
            }
        }

        default:
            return 0;
    }
}

// Evaluate constant float expression
double evaluate_constant_float(Expr* expr) {
    if (!expr) return 0.0;

    switch (expr->type) {
        case EXPR_LITERAL: {
            LiteralExpr* lit = &expr->as.literal;
            if (lit->type == TYPE_FLOAT) {
                return lit->value.float_value;
            }
            if (lit->type == TYPE_INT) {
                return (double)lit->value.int_value;
            }
            return 0.0;
        }

        case EXPR_UNARY: {
            UnaryExpr* unary = &expr->as.unary;
            double operand = evaluate_constant_float(unary->operand);

            if (unary->operator == TOKEN_MINUS) {
                return -operand;
            }
            return operand;
        }

        case EXPR_BINARY: {
            BinaryExpr* binary = &expr->as.binary;
            double left = evaluate_constant_float(binary->left);
            double right = evaluate_constant_float(binary->right);

            switch (binary->operator) {
                case TOKEN_PLUS: return left + right;
                case TOKEN_MINUS: return left - right;
                case TOKEN_STAR: return left * right;
                case TOKEN_SLASH: return right != 0.0 ? left / right : 0.0;
                default: return 0.0;
            }
        }

        default:
            return 0.0;
    }
}

// Constant folding optimization
Expr* fold_constants(Expr* expr, OptimizerContext* ctx) {
    if (!expr || !ctx->enable_constant_folding) return expr;

    // Recursively fold sub-expressions first
    if (expr->type == EXPR_BINARY) {
        BinaryExpr* binary = &expr->as.binary;
        binary->left = fold_constants(binary->left, ctx);
        binary->right = fold_constants(binary->right, ctx);
    } else if (expr->type == EXPR_UNARY) {
        UnaryExpr* unary = &expr->as.unary;
        unary->operand = fold_constants(unary->operand, ctx);
    }

    // Check if we can fold this expression
    if (!is_constant_expr(expr)) {
        return expr;
    }

    if (expr->data_type == TYPE_BOOL && !constant_integer_operations_valid(expr))
        return expr;

    uint64_t integer_bits = 0;
    bool fold_integer = type_is_integer(expr->data_type) && try_eval_integer(expr, &integer_bits);
    if (type_is_integer(expr->data_type) && !fold_integer) return expr;

    // Create folded literal expression
    Expr* folded = (Expr*)malloc(sizeof(Expr));
    if (!folded) return expr;
    memset(folded, 0, sizeof(*folded));
    folded->type = EXPR_LITERAL;
    folded->line = expr->line;
    folded->column = expr->column;
    folded->data_type = expr->data_type;

    if (type_is_integer(expr->data_type) || expr->data_type == TYPE_BOOL) {
        int64_t value = 0;
        if (fold_integer) value = cpx_int_bits_to_i64(integer_bits);
        else value = evaluate_constant_int(expr);
        folded->as.literal.type = TYPE_INT;
        folded->as.literal.value.int_value = value;
    } else if (expr->data_type == TYPE_FLOAT) {
        double value = evaluate_constant_float(expr);
        folded->as.literal.type = TYPE_FLOAT;
        folded->as.literal.value.float_value = value;
    } else {
        free(folded);
        return expr;  // Can't fold this type
    }

    ctx->constants_folded++;
    ctx->optimizations_performed++;
    free_expr(expr);
    return folded;
}

// Algebraic simplification
Expr* simplify_expr(Expr* expr, OptimizerContext* ctx) {
    if (!expr) return expr;

    if (expr->type != EXPR_BINARY) {
        return expr;
    }

    BinaryExpr* binary = &expr->as.binary;

    // Recursively simplify sub-expressions
    binary->left = simplify_expr(binary->left, ctx);
    binary->right = simplify_expr(binary->right, ctx);
    if (!binary->left || !binary->right) return expr;

    // Check for algebraic identities
    bool left_is_zero = false;
    bool left_is_one = false;
    bool right_is_zero = false;
    bool right_is_one = false;

    if (binary->left->type == EXPR_LITERAL) {
        LiteralExpr* lit = &binary->left->as.literal;
        if (lit->type == TYPE_INT) {
            left_is_zero = lit->value.int_value == 0;
            left_is_one = lit->value.int_value == 1;
        }
    }

    if (binary->right->type == EXPR_LITERAL) {
        LiteralExpr* lit = &binary->right->as.literal;
        if (lit->type == TYPE_INT) {
            right_is_zero = lit->value.int_value == 0;
            right_is_one = lit->value.int_value == 1;
        }
    }

    /* Return one child while reclaiming the discarded literal and wrapper.
     * The returned child remains owned by the caller. */
    #define RETURN_CHILD(child, discarded) do { \
        Expr* kept = (child); \
        Expr* drop = (discarded); \
        if (kept == binary->left) binary->left = NULL; else binary->right = NULL; \
        if (drop == binary->left) binary->left = NULL; else binary->right = NULL; \
        free_expr(drop); \
        free_expr(expr); \
        return kept; \
    } while (0)

    // Simplification rules:
    // x + 0 = x
    if (binary->operator == TOKEN_PLUS && right_is_zero) {
        ctx->expressions_simplified++;
        ctx->optimizations_performed++;
        RETURN_CHILD(binary->left, binary->right);
    }

    // 0 + x = x
    if (binary->operator == TOKEN_PLUS && left_is_zero) {
        ctx->expressions_simplified++;
        ctx->optimizations_performed++;
        RETURN_CHILD(binary->right, binary->left);
    }

    // x - 0 = x
    if (binary->operator == TOKEN_MINUS && right_is_zero) {
        ctx->expressions_simplified++;
        ctx->optimizations_performed++;
        RETURN_CHILD(binary->left, binary->right);
    }

    // x * 1 = x
    if (binary->operator == TOKEN_STAR && right_is_one) {
        ctx->expressions_simplified++;
        ctx->optimizations_performed++;
        RETURN_CHILD(binary->left, binary->right);
    }

    // 1 * x = x
    if (binary->operator == TOKEN_STAR && left_is_one) {
        ctx->expressions_simplified++;
        ctx->optimizations_performed++;
        RETURN_CHILD(binary->right, binary->left);
    }

    // x / 1 = x
    if (binary->operator == TOKEN_SLASH && right_is_one) {
        ctx->expressions_simplified++;
        ctx->optimizations_performed++;
        RETURN_CHILD(binary->left, binary->right);
    }

    #undef RETURN_CHILD

    return expr;
}

// Strength reduction (replace expensive operations with cheaper ones)
Expr* reduce_strength(Expr* expr, OptimizerContext* ctx) {
    if (!expr || expr->type != EXPR_BINARY) {
        return expr;
    }

    BinaryExpr* binary = &expr->as.binary;

    // Recursively reduce sub-expressions
    binary->left = reduce_strength(binary->left, ctx);
    binary->right = reduce_strength(binary->right, ctx);

    // x * 2 = x + x (addition is faster than multiplication)
    if (binary->operator == TOKEN_STAR && binary->right->type == EXPR_LITERAL) {
        LiteralExpr* lit = &binary->right->as.literal;
        if (lit->type == TYPE_INT && lit->value.int_value == 2) {
            binary->operator = TOKEN_PLUS;
            // Note: Would need to duplicate left expression
            ctx->optimizations_performed++;
        }
    }

    // x / 2 = x >> 1 (for integers, bit shift is faster)
    // Similar optimizations for powers of 2

    return expr;
}

// Check if statement is unreachable
bool is_unreachable_code(Stmt* stmt) {
    // Code after a return statement is unreachable
    // This is a simplified check
    return false;  // Implementation would track control flow
}

// Dead code elimination
void eliminate_dead_code(Stmt** statements, int* count, OptimizerContext* ctx) {
    if (!ctx->enable_dead_code_elimination) return;

    int write_index = 0;
    bool prev_was_return = false;

    for (int i = 0; i < *count; i++) {
        Stmt* stmt = statements[i];

        // Skip unreachable code after return
        if (prev_was_return) {
            ctx->dead_code_eliminated++;
            ctx->optimizations_performed++;
            continue;
        }

        statements[write_index++] = stmt;

        // Track if this was a return statement
        if (stmt->type == STMT_RETURN) {
            prev_was_return = true;
        }
    }

    *count = write_index;
}

// Optimize statements
static void optimize_stmt(Stmt* stmt, OptimizerContext* ctx) {
    if (!stmt) return;

    switch (stmt->type) {
        case STMT_DECLARATION: {
            DeclarationStmt* decl = &stmt->as.declaration;
            if (decl->initializer) {
                decl->initializer = fold_constants(decl->initializer, ctx);
                decl->initializer = simplify_expr(decl->initializer, ctx);
            }
            break;
        }

        case STMT_ASSIGNMENT: {
            AssignmentStmt* assign = &stmt->as.assignment;
            assign->value = fold_constants(assign->value, ctx);
            assign->value = simplify_expr(assign->value, ctx);
            break;
        }

        case STMT_IF: {
            IfStmt* if_stmt = &stmt->as.if_stmt;
            if_stmt->condition = fold_constants(if_stmt->condition, ctx);

            // Optimize then and else branches
            if (if_stmt->then_branch) {
                optimize_stmt(if_stmt->then_branch, ctx);
            }
            if (if_stmt->else_branch) {
                optimize_stmt(if_stmt->else_branch, ctx);
            }
            break;
        }

        case STMT_WHILE: {
            WhileStmt* while_stmt = &stmt->as.while_stmt;
            while_stmt->condition = fold_constants(while_stmt->condition, ctx);

            if (while_stmt->body) {
                optimize_stmt(while_stmt->body, ctx);
            }
            break;
        }

        case STMT_FOR: {
            ForStmt* for_stmt = &stmt->as.for_stmt;
            // Initializer is an Expr*, not Stmt*, so fold it
            if (for_stmt->initializer) {
                for_stmt->initializer = fold_constants(for_stmt->initializer, ctx);
                for_stmt->initializer = simplify_expr(for_stmt->initializer, ctx);
            }
            if (for_stmt->condition) {
                for_stmt->condition = fold_constants(for_stmt->condition, ctx);
            }
            if (for_stmt->increment) {
                optimize_stmt(for_stmt->increment, ctx);
            }

            if (for_stmt->body) {
                optimize_stmt(for_stmt->body, ctx);
            }
            break;
        }

        case STMT_RETURN: {
            ReturnStmt* ret = &stmt->as.return_stmt;
            if (ret->value) {
                ret->value = fold_constants(ret->value, ctx);
                ret->value = simplify_expr(ret->value, ctx);
            }
            break;
        }

        case STMT_PRINT: {
            PrintStmt* print = &stmt->as.print;
            print->expression = fold_constants(print->expression, ctx);
            print->expression = simplify_expr(print->expression, ctx);
            break;
        }

        default:
            break;
    }
}

// Main optimization pass
void optimize_program(Stmt** statements, int count, OptimizerContext* ctx) {
    // Pass 1: Constant folding and algebraic simplification
    for (int i = 0; i < count; i++) {
        optimize_stmt(statements[i], ctx);
    }

    // Pass 2: Dead code elimination
    eliminate_dead_code(statements, &count, ctx);
}

// Print optimization statistics
void print_optimizer_stats(OptimizerContext* ctx) {
    printf("\n=== Optimizer Statistics ===\n");
    printf("Total optimizations: %d\n", ctx->optimizations_performed);
    printf("Constants folded: %d\n", ctx->constants_folded);
    printf("Expressions simplified: %d\n", ctx->expressions_simplified);
    printf("Dead code eliminated: %d\n", ctx->dead_code_eliminated);
    printf("============================\n\n");
}
