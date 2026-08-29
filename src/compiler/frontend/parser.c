#include "compiler/frontend/parser.h"
#include "support/error.h"
#include <limits.h>

static void advance(Parser* parser);
static bool check(Parser* parser, TokenType type);
static bool match(Parser* parser, TokenType type);
static void consume(Parser* parser, TokenType type, const char* message);
static void synchronize(Parser* parser);
static void synchronize_block(Parser* parser);
static char* copy_identifier(Token* token);
static bool token_text_equals(Token* token, const char* text);
static Expr* error_expression(int line, int col);
static void report_unsupported_surface(Parser* parser, Token token, const char* message);

// Forward declarations
static Expr* expression(Parser* parser);
static Expr* unary(Parser* parser);
static Expr* primary(Parser* parser);
static Expr* postfix(Parser* parser);
static Expr* postfix_with_expr(Parser* parser, Expr* expr);
static Stmt* statement(Parser* parser);
static Stmt* declaration(Parser* parser);
static Stmt* include_statement(Parser* parser);
static Stmt* extern_declaration(Parser* parser);
static Expr* lambda_expression(Parser* parser);
static Stmt* block_statement(Parser* parser);
static Stmt* implicit_block_statement(Parser* parser, int line, int col);
static Stmt* trait_statement(Parser* parser);

void init_parser(Parser* parser, Lexer* lexer) {
    parser->lexer = lexer;
    parser->had_error = false;
    parser->panic_mode = false;
    // Store source pointer for manual extraction if needed
    parser->source = lexer->start; // lexer->start points to source start
    advance(parser);
}

static void error_at(Parser* parser, Token* token, const char* message) {
    if (parser->panic_mode) return;
    parser->panic_mode = true;
    
    report_error(token->line, token->column, message);
    parser->had_error = true;
}

static void error_at_current(Parser* parser, const char* message) {
    error_at(parser, &parser->current, message);
}

static void advance(Parser* parser) {
    parser->previous = parser->current;
    
    for (;;) {
        parser->current = scan_token(parser->lexer);
        if (parser->current.type != TOKEN_ERROR) break;
        
        error_at_current(parser, parser->current.start);
    }
}

static bool check(Parser* parser, TokenType type) {
    return parser->current.type == type;
}

static bool match(Parser* parser, TokenType type) {
    if (!check(parser, type)) return false;
    advance(parser);
    return true;
}

static void consume(Parser* parser, TokenType type, const char* message) {
    if (parser->current.type == type) {
        advance(parser);
        return;
    }

    error_at_current(parser, message);
}

/* Close a generic type-argument or type-parameter list.
 *
 * The lexer greedily tokenizes ">>" as a single TOKEN_RSHIFT (the right-shift
 * operator), but in a nested generic such as `Array<MapEntry<K, V>>` the ">>"
 * is really two separate closing angle brackets. When a generic-close site
 * expects a single '>' and instead finds an RSHIFT, split it in place: consume
 * the logical first '>' and rewrite parser->current into a TOKEN_GREATER that
 * covers the remaining '>' so the enclosing generic level can close normally.
 * ">>>" (RSHIFT followed by GREATER) chains through this the same way.
 *
 * This is scoped to type-list parsing only; the real ">>" operator is handled
 * separately in shift() and never routes through here. */
static void consume_generic_close(Parser* parser, const char* message) {
    if (parser->current.type == TOKEN_GREATER) {
        advance(parser);
        return;
    }

    if (parser->current.type == TOKEN_RSHIFT) {
        /* Rewrite the pending ">>" token into a single ">" starting one column
         * later, without pulling a fresh token from the lexer. previous is set
         * so callers that inspect it after the close still see a '>'. */
        parser->previous = parser->current;
        parser->previous.type = TOKEN_GREATER;
        parser->previous.length = 1;

        parser->current.type = TOKEN_GREATER;
        parser->current.start += 1;
        parser->current.length = 1;
        parser->current.column += 1;
        return;
    }

    error_at_current(parser, message);
}

static void synchronize(Parser* parser) {
    parser->panic_mode = false;
    
    // Guarantee progress by advancing at least once
    advance(parser);
    
    while (parser->current.type != TOKEN_EOF) {
        if (parser->previous.type == TOKEN_SEMICOLON) return;
        
        switch (parser->current.type) {
            case TOKEN_FUNC:
            case TOKEN_IF:
            case TOKEN_FOR:
            case TOKEN_WHILE:
            case TOKEN_PRINT:
            case TOKEN_RETURN:
                return;
            default:
                ; // Do nothing
        }
        
        advance(parser);
    }
}

static void synchronize_block(Parser* parser) {
    parser->panic_mode = false;
    
    // Guarantee progress by advancing at least once
    advance(parser);
    
    while (parser->current.type != TOKEN_EOF) {
        if (parser->current.type == TOKEN_RBRACE) return;
        if (parser->previous.type == TOKEN_SEMICOLON) return;
        
        switch (parser->current.type) {
            case TOKEN_FUNC:
            case TOKEN_LET:
            case TOKEN_MUT:
            case TOKEN_CONST:
            case TOKEN_IF:
            case TOKEN_WHILE:
            case TOKEN_FOR:
            case TOKEN_RETURN:
            case TOKEN_PRINT:
            case TOKEN_MATCH:
            case TOKEN_TRY:
            case TOKEN_THROW:
            case TOKEN_CLASS:
            case TOKEN_STRUCT:
            case TOKEN_ENUM_KW:
            case TOKEN_UNION_KW:
            case TOKEN_TRAIT:
            case TOKEN_IMPL:
            case TOKEN_EXTERN:
            case TOKEN_PUBLIC:
            case TOKEN_PRIVATE:
            case TOKEN_PROTECTED:
            case TOKEN_STATIC:
            case TOKEN_ABSTRACT:
                return;
            default:
                break;
        }
        
        advance(parser);
    }
}

static Token peek_next_token(Parser* parser) {
    Lexer peek_lexer;
    Token consumed;

    init_lexer(&peek_lexer, parser->current.start);
    consumed = scan_token(&peek_lexer);
    (void)consumed;
    return scan_token(&peek_lexer);
}

static void parse_dyn_trait_bounds(Parser* parser) {
    consume(parser, TOKEN_LBRACKET, "Expected '[' after 'dyn'");
    do {
        consume(parser, TOKEN_IDENTIFIER, "Expected trait name in dyn trait set");
    } while (match(parser, TOKEN_PLUS));
    consume(parser, TOKEN_RBRACKET, "Expected ']' after dyn trait set");
}

static Expr* error_expression(int line, int col) {
    LiteralExpr literal;
    memset(&literal, 0, sizeof(literal));
    literal.type = TYPE_ERROR;
    return create_literal_expr(literal, line, col);
}

static void report_unsupported_surface(Parser* parser, Token token, const char* message) {
    error_at(parser, &token, message);
    advance(parser);
}

// Check if current token is a type keyword (simple primitive)
static bool is_simple_type_token(TokenType type) {
    switch (type) {
        case TOKEN_INT_TYPE:     case TOKEN_FLOAT_TYPE:
        case TOKEN_STRING_TYPE:  case TOKEN_BOOL_TYPE:
        case TOKEN_VOID_TYPE:    case TOKEN_CHAR_TYPE:
        case TOKEN_STRBUF_TYPE:  case TOKEN_RAWPTR_TYPE:
        case TOKEN_I8_TYPE:      case TOKEN_I16_TYPE:
        case TOKEN_I32_TYPE:     case TOKEN_I64_TYPE:
        case TOKEN_I128_TYPE:    case TOKEN_U8_TYPE:
        case TOKEN_U16_TYPE:     case TOKEN_U32_TYPE:
        case TOKEN_U64_TYPE:     case TOKEN_U128_TYPE:
        case TOKEN_F16_TYPE:     case TOKEN_F32_TYPE:
        case TOKEN_F64_TYPE:     case TOKEN_BF16_TYPE:
            return true;
        default:
            return false;
    }
}

// Map token to DataType for simple types
static DataType token_to_datatype(TokenType type) {
    switch (type) {
        case TOKEN_INT_TYPE:     return TYPE_I32;
        case TOKEN_FLOAT_TYPE:   return TYPE_F64;
        case TOKEN_STRING_TYPE:  return TYPE_STRING;
        case TOKEN_BOOL_TYPE:    return TYPE_BOOL;
        case TOKEN_VOID_TYPE:    return TYPE_VOID;
        case TOKEN_CHAR_TYPE:    return TYPE_CHAR;
        case TOKEN_STRBUF_TYPE:  return TYPE_STRBUF;
        case TOKEN_RAWPTR_TYPE:  return TYPE_RAWPTR;
        case TOKEN_I8_TYPE:      return TYPE_I8;
        case TOKEN_I16_TYPE:     return TYPE_I16;
        case TOKEN_I32_TYPE:     return TYPE_I32;
        case TOKEN_I64_TYPE:     return TYPE_I64;
        case TOKEN_I128_TYPE:    return TYPE_I128;
        case TOKEN_U8_TYPE:      return TYPE_U8;
        case TOKEN_U16_TYPE:     return TYPE_U16;
        case TOKEN_U32_TYPE:     return TYPE_U32;
        case TOKEN_U64_TYPE:     return TYPE_U64;
        case TOKEN_U128_TYPE:    return TYPE_U128;
        case TOKEN_F16_TYPE:     return TYPE_F16;
        case TOKEN_F32_TYPE:     return TYPE_F32;
        case TOKEN_F64_TYPE:     return TYPE_F64;
        case TOKEN_BF16_TYPE:    return TYPE_BF16;
        default:                 return TYPE_ERROR;
    }
}

// Check if current token starts a parameterized type (array<T>, ptr<T>, etc.)
static bool is_parameterized_type_token(TokenType type) {
    switch (type) {
        case TOKEN_ARRAY_TYPE:   case TOKEN_SLICE_TYPE:
        case TOKEN_PTR_TYPE:     case TOKEN_REF_TYPE:
        case TOKEN_TENSOR_TYPE:
            return true;
        default:
            return false;
    }
}

static bool token_text_equals(Token* token, const char* text) {
    size_t len;

    if (!token || !text || !token->start) return false;

    len = strlen(text);
    return token->length == (int)len && memcmp(token->start, text, len) == 0;
}

static bool is_constructor_name(const char* class_name, const char* method_name) {
    if (!class_name || !method_name) return false;
    return strcmp(method_name, "new") == 0 || strcmp(method_name, class_name) == 0;
}

static Expr** parse_call_arguments(Parser* parser, int* out_count, const char* closing_message) {
    int capacity = 8;
    int count = 0;
    Expr** args = ALLOCATE(Expr*, capacity);

    if (!check(parser, TOKEN_RPAREN)) {
        do {
            if (count >= capacity) {
                capacity = GROW_CAPACITY(capacity);
                args = GROW_ARRAY(Expr*, args, count, capacity);
            }
            args[count++] = expression(parser);
        } while (match(parser, TOKEN_COMMA));
    }

    consume(parser, TOKEN_RPAREN, closing_message);
    *out_count = count;
    return args;
}

typedef enum {
    CLASS_FIELD_BINDING_LEGACY,
    CLASS_FIELD_BINDING_LET,
    CLASS_FIELD_BINDING_MUT,
    CLASS_FIELD_BINDING_CONST
} ClassFieldBindingKind;

static FieldDecl* append_class_field_slot(FieldDecl** fields, int* field_count, int* field_capacity) {
    if (*field_count >= *field_capacity) {
        *field_capacity = GROW_CAPACITY(*field_capacity);
        *fields = GROW_ARRAY(FieldDecl, *fields, *field_count, *field_capacity);
    }

    return &(*fields)[(*field_count)++];
}

static void init_class_field_decl(FieldDecl* field, Token* name_token, DataType type,
                                  char* class_name, bool is_static, AccessModifier access,
                                  ClassFieldBindingKind binding_kind) {
    field->name = copy_identifier(name_token);
    field->type = type;
    field->class_name = class_name;
    field->default_value = NULL;
    field->is_const = binding_kind == CLASS_FIELD_BINDING_CONST;
    field->is_mutable = binding_kind == CLASS_FIELD_BINDING_MUT ||
                        binding_kind == CLASS_FIELD_BINDING_LEGACY;
    field->is_static = is_static;
    field->access = access;
    field->ownership = OWNERSHIP_OWNED;
}

// Forward declare parse_type_with_class for recursive use
static DataType parse_type_with_class(Parser* parser, char** out_class_name);

/* Set whenever the type just parsed was a `lambda(...) -> R` function type, so
 * the caller (parameter/variable-decl parsing) can retain the inner arity for
 * arity checking of calls through that binding. Consumed via
 * take_pending_fn_type_info(); NULL when the last type was not a function type. */
static TypeInfo* g_pending_fn_type_info = NULL;

static TypeInfo* take_pending_fn_type_info(void) {
    TypeInfo* ti = g_pending_fn_type_info;
    g_pending_fn_type_info = NULL;
    return ti;
}

/* Build a TypeInfo for a `lambda(P0, P1, ...) -> R` type. The opening `lambda`
 * identifier has already been consumed; parses `( ... )` and an optional
 * `-> R`. Parameter/return class names are not retained (only base DataTypes),
 * matching how the rest of the type system treats function types. */
static TypeInfo* parse_lambda_type_info(Parser* parser) {
    TypeInfo* info = create_type_info(TYPE_FUNC);
    int cap = 4;
    info->param_types = ALLOCATE(TypeInfo*, cap);
    info->param_count = 0;

    if (match(parser, TOKEN_LPAREN)) {
        while (!check(parser, TOKEN_RPAREN) && !check(parser, TOKEN_EOF)) {
            char* pcn = NULL;
            DataType pt = parse_type_with_class(parser, &pcn);
            free(pcn);
            if (info->param_count >= cap) {
                cap *= 2;
                info->param_types = GROW_ARRAY(TypeInfo*, info->param_types,
                                               info->param_count, cap);
            }
            info->param_types[info->param_count++] = create_type_info(pt);
            if (!match(parser, TOKEN_COMMA)) break;
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after lambda type parameters");
    }

    if (match(parser, TOKEN_ARROW)) {
        char* rcn = NULL;
        DataType rt = parse_type_with_class(parser, &rcn);
        free(rcn);
        info->return_type = create_type_info(rt);
    } else {
        info->return_type = create_type_info(TYPE_VOID);
    }
    return info;
}

// Parse a generic type parameter: <T>
// Expects '<' already consumed or about to be consumed
static DataType parse_inner_type_param(Parser* parser, char** out_class_name) {
    consume(parser, TOKEN_LESS, "Expected '<' for type parameter");
    DataType inner = parse_type_with_class(parser, out_class_name);
    consume_generic_close(parser, "Expected '>' after type parameter");
    return inner;
}

static DataType parse_type(Parser* parser) {
    g_pending_fn_type_info = NULL;  /* only the `lambda(...)` branch sets this */
    // Borrow types: &T (immutable borrow), &mut T (mutable borrow)
    if (match(parser, TOKEN_BITAND)) {
        if (match(parser, TOKEN_MUT)) {
            // &mut T — parse inner type but return as reference
            // (ownership tracked separately via Parameter.ownership)
            parse_type(parser);  // consume inner type
            return TYPE_REF;    // uses REF as carrier; ownership = BORROW_MUT
        }
        // &T — immutable borrow
        parse_type(parser);
        return TYPE_REF;        // ownership = BORROW
    }

    // Simple primitive types
    if (is_simple_type_token(parser->current.type)) {
        advance(parser);
        return token_to_datatype(parser->previous.type);
    }

    // Parameterized types: array<T>, slice<T>, ptr<T>, ref<T>, tensor<T>
    if (is_parameterized_type_token(parser->current.type)) {
        TokenType param_token = parser->current.type;
        advance(parser);

        // Parse inner type parameter
        if (check(parser, TOKEN_LESS)) {
            char* inner_class = NULL;
            DataType inner = parse_inner_type_param(parser, &inner_class);
            if (inner_class) free(inner_class);
            (void)inner;  // We don't track inner type in simple parse_type
        }

        switch (param_token) {
            case TOKEN_ARRAY_TYPE:  return TYPE_ARRAY;
            case TOKEN_SLICE_TYPE:  return TYPE_SLICE;
            case TOKEN_PTR_TYPE:    return TYPE_PTR;
            case TOKEN_REF_TYPE:    return TYPE_REF;
            case TOKEN_TENSOR_TYPE: return TYPE_TENSOR;
            default: break;
        }
    }

    // Static array: [T; N]
    if (match(parser, TOKEN_LBRACKET)) {
        // Parse element type
        DataType elem = parse_type(parser);
        (void)elem;
        consume(parser, TOKEN_SEMICOLON, "Expected ';' in static array type [T; N]");
        // Parse size
        consume(parser, TOKEN_INTEGER, "Expected array size");
        consume(parser, TOKEN_RBRACKET, "Expected ']' after array type");
        return TYPE_STATIC_ARRAY;
    }

    // Check for SIMD vector/matrix types (vec2, vec4, mat3, etc. as identifiers)
    if (check(parser, TOKEN_IDENTIFIER)) {
        const char* name = parser->current.start;
        int len = parser->current.length;

        if (token_text_equals(&parser->current, "dyn")) {
            advance(parser);
            if (check(parser, TOKEN_LBRACKET)) {
                parse_dyn_trait_bounds(parser);
            }
            return TYPE_DYN;
        }

        if (token_text_equals(&parser->current, "Int")) {
            advance(parser);
            return TYPE_I32;
        }
        if (token_text_equals(&parser->current, "Float")) {
            advance(parser);
            return TYPE_F64;
        }
        if (token_text_equals(&parser->current, "String")) {
            advance(parser);
            return TYPE_STRING;
        }
        if (token_text_equals(&parser->current, "Bool")) {
            advance(parser);
            return TYPE_BOOL;
        }
        if (token_text_equals(&parser->current, "Void")) {
            advance(parser);
            return TYPE_VOID;
        }
        if (token_text_equals(&parser->current, "lambda")) {
            advance(parser);
            g_pending_fn_type_info = parse_lambda_type_info(parser);
            return TYPE_FUNC;
        }
        if (len == 1 && name[0] >= 'A' && name[0] <= 'Z') {
            advance(parser);
            return TYPE_ERROR;
        }
        if (token_text_equals(&parser->current, "Tuple")) {
            advance(parser);
            if (check(parser, TOKEN_LESS)) {
                advance(parser);
                do {
                    char* tuple_arg_class = NULL;
                    parse_type_with_class(parser, &tuple_arg_class);
                    free(tuple_arg_class);
                } while (match(parser, TOKEN_COMMA));
                consume_generic_close(parser, "Expected '>' after tuple type arguments");
            }
            return TYPE_ERROR;
        }

        // vec2, vec3, vec4, vec8, vec16
        if (len >= 4 && len <= 5 && name[0] == 'v' && name[1] == 'e' && name[2] == 'c') {
            advance(parser);
            if (check(parser, TOKEN_LESS)) {
                char* inner_class = NULL;
                DataType inner = parse_inner_type_param(parser, &inner_class);
                if (inner_class) free(inner_class);
                (void)inner;
            }
            return TYPE_VEC;
        }

        // mat2, mat3, mat4
        if (len == 4 && name[0] == 'm' && name[1] == 'a' && name[2] == 't') {
            advance(parser);
            if (check(parser, TOKEN_LESS)) {
                char* inner_class = NULL;
                DataType inner = parse_inner_type_param(parser, &inner_class);
                if (inner_class) free(inner_class);
                (void)inner;
            }
            return TYPE_MAT;
        }

        // Regular class/struct/enum name, optionally with generic arguments.
        advance(parser);
        if (check(parser, TOKEN_LESS)) {
            advance(parser);
            do {
                char* generic_arg_class = NULL;
                parse_type_with_class(parser, &generic_arg_class);
                free(generic_arg_class);
            } while (match(parser, TOKEN_COMMA));
            consume_generic_close(parser, "Expected '>' after generic type arguments");
        }
        return TYPE_CLASS;
    }

    error_at_current(parser, "Expected type name");
    return TYPE_ERROR;
}

// Parse type and return class name if it's a class/named type
static DataType parse_type_with_class(Parser* parser, char** out_class_name) {
    *out_class_name = NULL;
    g_pending_fn_type_info = NULL;  /* only the `lambda(...)` branch sets this */

    // Borrow types: &T (immutable borrow), &mut T (mutable borrow)
    if (match(parser, TOKEN_BITAND)) {
        bool is_mut = match(parser, TOKEN_MUT);
        DataType inner = parse_type_with_class(parser, out_class_name);
        (void)inner;
        (void)is_mut;
        // The caller must check if & or &mut was used;
        // for now we mark via ownership on the Parameter struct.
        // Return the inner type — ownership annotation is on the param/decl.
        return inner;
    }

    // Simple primitive types
    if (is_simple_type_token(parser->current.type)) {
        advance(parser);
        return token_to_datatype(parser->previous.type);
    }

    // Parameterized types: array<T>, slice<T>, ptr<T>, ref<T>, tensor<T>
    if (is_parameterized_type_token(parser->current.type)) {
        TokenType param_token = parser->current.type;
        advance(parser);

        if (check(parser, TOKEN_LESS)) {
            char* inner_class = NULL;
            DataType inner = parse_inner_type_param(parser, &inner_class);
            if (inner_class) free(inner_class);
            (void)inner;
        }

        switch (param_token) {
            case TOKEN_ARRAY_TYPE:  return TYPE_ARRAY;
            case TOKEN_SLICE_TYPE:  return TYPE_SLICE;
            case TOKEN_PTR_TYPE:    return TYPE_PTR;
            case TOKEN_REF_TYPE:    return TYPE_REF;
            case TOKEN_TENSOR_TYPE: return TYPE_TENSOR;
            default: break;
        }
    }

    // Static array: [T; N]
    if (match(parser, TOKEN_LBRACKET)) {
        DataType elem = parse_type(parser);
        (void)elem;
        consume(parser, TOKEN_SEMICOLON, "Expected ';' in static array type [T; N]");
        consume(parser, TOKEN_INTEGER, "Expected array size");
        consume(parser, TOKEN_RBRACKET, "Expected ']' after array type");
        return TYPE_STATIC_ARRAY;
    }

    // Check for SIMD vector/matrix types as identifiers
    if (check(parser, TOKEN_IDENTIFIER)) {
        const char* name = parser->current.start;
        int len = parser->current.length;

        if (token_text_equals(&parser->current, "dyn")) {
            advance(parser);
            if (check(parser, TOKEN_LBRACKET)) {
                parse_dyn_trait_bounds(parser);
            }
            return TYPE_DYN;
        }

        if (token_text_equals(&parser->current, "Int")) {
            advance(parser);
            return TYPE_I32;
        }
        if (token_text_equals(&parser->current, "Float")) {
            advance(parser);
            return TYPE_F64;
        }
        if (token_text_equals(&parser->current, "String")) {
            advance(parser);
            return TYPE_STRING;
        }
        if (token_text_equals(&parser->current, "Bool")) {
            advance(parser);
            return TYPE_BOOL;
        }
        if (token_text_equals(&parser->current, "Void")) {
            advance(parser);
            return TYPE_VOID;
        }
        if (token_text_equals(&parser->current, "lambda")) {
            advance(parser);
            g_pending_fn_type_info = parse_lambda_type_info(parser);
            return TYPE_FUNC;
        }
        if (len == 1 && name[0] >= 'A' && name[0] <= 'Z') {
            advance(parser);
            return TYPE_ERROR;
        }
        if (token_text_equals(&parser->current, "Tuple")) {
            advance(parser);
            if (check(parser, TOKEN_LESS)) {
                advance(parser);
                do {
                    char* tuple_arg_class = NULL;
                    parse_type_with_class(parser, &tuple_arg_class);
                    free(tuple_arg_class);
                } while (match(parser, TOKEN_COMMA));
                consume_generic_close(parser, "Expected '>' after tuple type arguments");
            }
            return TYPE_ERROR;
        }

        // vec2, vec3, vec4, vec8, vec16
        if (len >= 4 && len <= 5 && name[0] == 'v' && name[1] == 'e' && name[2] == 'c') {
            advance(parser);
            if (check(parser, TOKEN_LESS)) {
                char* inner_class = NULL;
                DataType inner = parse_inner_type_param(parser, &inner_class);
                if (inner_class) free(inner_class);
                (void)inner;
            }
            return TYPE_VEC;
        }

        // mat2, mat3, mat4
        if (len == 4 && name[0] == 'm' && name[1] == 'a' && name[2] == 't') {
            advance(parser);
            if (check(parser, TOKEN_LESS)) {
                char* inner_class = NULL;
                DataType inner = parse_inner_type_param(parser, &inner_class);
                if (inner_class) free(inner_class);
                (void)inner;
            }
            return TYPE_MAT;
        }

        // Regular class/struct/enum type name, optionally with generic arguments.
        advance(parser);
        *out_class_name = copy_identifier(&parser->previous);
        if (check(parser, TOKEN_LESS)) {
            advance(parser);
            do {
                char* generic_arg_class = NULL;
                parse_type_with_class(parser, &generic_arg_class);
                free(generic_arg_class);
            } while (match(parser, TOKEN_COMMA));
            consume_generic_close(parser, "Expected '>' after generic type arguments");
        }
        return TYPE_CLASS;
    }

    error_at_current(parser, "Expected type name");
    return TYPE_ERROR;
}

static char* copy_identifier(Token* token) {
    char* name = ALLOCATE(char, token->length + 1);
    memcpy(name, token->start, token->length);
    name[token->length] = '\0';
    return name;
}

// Lambda expression parsing: |x: Int, y: Int| => x + y  or  |x: Int| -> Int { ... }
// Parse generic type parameters: <T>, <K, V>, <T: Comparable>
static TypeParam* parse_type_params(Parser* parser, int* out_count) {
    int capacity = 4;
    int count = 0;
    TypeParam* params = ALLOCATE(TypeParam, capacity);

    do {
        if (count >= capacity) {
            capacity = GROW_CAPACITY(capacity);
            params = GROW_ARRAY(TypeParam, params, count, capacity);
        }

        consume(parser, TOKEN_IDENTIFIER, "Expected type parameter name");
        params[count].name = copy_identifier(&parser->previous);
        params[count].constraint = NULL;
        params[count].default_type = TYPE_ERROR;
        params[count].default_class = NULL;

        // Check for constraint: T: Comparable
        if (match(parser, TOKEN_COLON)) {
            consume(parser, TOKEN_IDENTIFIER, "Expected constraint type name");
            params[count].constraint = copy_identifier(&parser->previous);
        }

        if (check(parser, TOKEN_IMPLEMENTS)) {
            error_at_current(parser,
                "'implements' is not supported in generic constraints; use ':' for a single trait bound");
            while (!check(parser, TOKEN_COMMA) &&
                   !check(parser, TOKEN_GREATER) &&
                   !check(parser, TOKEN_EOF)) {
                advance(parser);
            }
        }

        count++;
    } while (match(parser, TOKEN_COMMA));

    consume_generic_close(parser, "Expected '>' after type parameters");

    *out_count = count;
    return params;
}

static char** parse_implements_list(Parser* parser, int* out_count) {
    int capacity = 4;
    int count = 0;
    char** names = ALLOCATE(char*, capacity);

    do {
        if (count >= capacity) {
            capacity = GROW_CAPACITY(capacity);
            names = GROW_ARRAY(char*, names, count, capacity);
        }

        consume(parser, TOKEN_IDENTIFIER, "Expected trait name after 'implements'");
        names[count++] = copy_identifier(&parser->previous);
    } while (match(parser, TOKEN_COMMA));

    *out_count = count;
    return names;
}

static Expr* lambda_expression(Parser* parser) {
    int line = parser->previous.line;  // TOKEN_PIPE was consumed
    int col = parser->previous.column;

    // Check for capture mode prefix: |&| |x| => ..., |move| |x| => ..., |copy| |x| => ...
    ClosureCaptureMode capture_mode = CAPTURE_INFER;
    if (check(parser, TOKEN_BITAND) || check(parser, TOKEN_MOVE) || check(parser, TOKEN_COPY)) {
        if (match(parser, TOKEN_BITAND)) {
            capture_mode = CAPTURE_BORROW;
        } else if (match(parser, TOKEN_MOVE)) {
            capture_mode = CAPTURE_MOVE;
        } else if (match(parser, TOKEN_COPY)) {
            capture_mode = CAPTURE_COPY;
        }
        consume(parser, TOKEN_PIPE, "Expected '|' after capture mode");
        // Now consume the opening pipe for actual parameters
        consume(parser, TOKEN_PIPE, "Expected '|' to begin lambda parameters after capture mode");
    }

    // Parse parameters: |param: Type, param2: Type|
    int param_capacity = 8;
    int param_count = 0;
    Parameter* params = ALLOCATE(Parameter, param_capacity);

    // Handle empty parameter list: || => ... or backward-compatible |:| => ...
    if (match(parser, TOKEN_COLON)) {
        /* No parameters. */
    } else if (!check(parser, TOKEN_PIPE)) {
        do {
            if (param_count >= param_capacity) {
                param_capacity = GROW_CAPACITY(param_capacity);
                params = GROW_ARRAY(Parameter, params, param_count, param_capacity);
            }

            consume(parser, TOKEN_IDENTIFIER, "Expected parameter name");
            params[param_count].name = copy_identifier(&parser->previous);

            consume(parser, TOKEN_COLON, "Expected ':' after parameter name");
            char* param_class_name = NULL;
            params[param_count].type = parse_type_with_class(parser, &param_class_name);
            params[param_count].class_name = param_class_name;
            /* Retain a `lambda(...) -> R` signature so calls through this
             * parameter can be arity-checked (see semantic.c). */
            params[param_count].type_info = take_pending_fn_type_info();
            params[param_count].ownership = OWNERSHIP_OWNED;

            param_count++;
        } while (match(parser, TOKEN_COMMA));
    }

    consume(parser, TOKEN_PIPE, "Expected '|' after lambda parameters");

    // Create lambda expression
    Expr* expr = ALLOCATE(Expr, 1);
    memset(expr, 0, sizeof(Expr));
    expr->type = EXPR_LAMBDA;
    expr->line = line;
    expr->column = col;
    expr->data_type = TYPE_FUNC;
    expr->class_name = NULL;

    expr->as.lambda.parameters = params;
    expr->as.lambda.param_count = param_count;
    expr->as.lambda.captured_vars = NULL;
    expr->as.lambda.captured_types = NULL;
    expr->as.lambda.captured_is_mutable = NULL;
    expr->as.lambda.has_mutable_capture = false;
    expr->as.lambda.capture_count = 0;
    expr->as.lambda.capture_mode = capture_mode;
    expr->as.lambda.closure_id = 0;  // Will be assigned during semantic analysis

    // Check for expression form (=>) or block form (-> Type {})
    if (match(parser, TOKEN_FAT_ARROW)) {
        Expr* body_expr = expression(parser);

        if (check(parser, TOKEN_ASSIGN) &&
            body_expr &&
            (body_expr->type == EXPR_VARIABLE ||
             body_expr->type == EXPR_MEMBER_ACCESS ||
             body_expr->type == EXPR_INDEX)) {
            int stmt_capacity = 2;
            Stmt** stmts = ALLOCATE(Stmt*, stmt_capacity);
            Expr* return_expr = clone_expr(body_expr);
            Expr* assign_value;

            advance(parser); /* consume '=' */
            assign_value = expression(parser);

            stmts[0] = create_assignment_stmt(body_expr, assign_value, line, col);
            stmts[1] = create_return_stmt(return_expr, line, col);

            expr->as.lambda.is_expression = false;
            expr->as.lambda.return_type = TYPE_ERROR;
            expr->as.lambda.return_class_name = NULL;
            expr->as.lambda.expr_body = NULL;
            expr->as.lambda.block_body = create_block_stmt(stmts, 2, line, col);
        } else {
            // Expression lambda: |x| => x + 1
            expr->as.lambda.is_expression = true;
            expr->as.lambda.return_type = TYPE_ERROR;  // Will be inferred
            expr->as.lambda.return_class_name = NULL;
            expr->as.lambda.expr_body = body_expr;
            expr->as.lambda.block_body = NULL;
        }
    } else if (match(parser, TOKEN_ARROW)) {
        // Block lambda: |x: Int| -> Int { return x + 1; }
        expr->as.lambda.is_expression = false;

        char* ret_class_name = NULL;
        expr->as.lambda.return_type = parse_type_with_class(parser, &ret_class_name);
        expr->as.lambda.return_class_name = ret_class_name;
        expr->as.lambda.expr_body = NULL;

        consume(parser, TOKEN_LBRACE, "Expected '{' before lambda body");
        expr->as.lambda.block_body = block_statement(parser);
    } else {
        error_at_current(parser, "Expected '=>' or '->' after lambda parameters");
        return NULL;
    }

    return expr;
}

// Expression parsing (precedence climbing)
static Expr* primary(Parser* parser) {
    if (match(parser, TOKEN_INTEGER)) {
        LiteralExpr lit;
        if (parser->previous.literal.int_value < INT32_MIN ||
            parser->previous.literal.int_value > INT32_MAX) {
            lit.type = TYPE_I64;
        } else {
            lit.type = TYPE_INT;
        }
        lit.value.int_value = parser->previous.literal.int_value;
        return create_literal_expr(lit, parser->previous.line, parser->previous.column);
    }
    
    if (match(parser, TOKEN_FLOAT)) {
        LiteralExpr lit;
        lit.type = TYPE_FLOAT;
        lit.value.float_value = parser->previous.literal.float_value;
        return create_literal_expr(lit, parser->previous.line, parser->previous.column);
    }
    
    if (match(parser, TOKEN_STRING)) {
        LiteralExpr lit;
        lit.type = TYPE_STRING;
        lit.value.string_value = parser->previous.literal.string_value;
        return create_literal_expr(lit, parser->previous.line, parser->previous.column);
    }
    
    if (match(parser, TOKEN_TRUE)) {
        LiteralExpr lit;
        lit.type = TYPE_BOOL;
        lit.value.bool_value = true;
        return create_literal_expr(lit, parser->previous.line, parser->previous.column);
    }
    
    if (match(parser, TOKEN_FALSE)) {
        LiteralExpr lit;
        lit.type = TYPE_BOOL;
        lit.value.bool_value = false;
        return create_literal_expr(lit, parser->previous.line, parser->previous.column);
    }

    if (match(parser, TOKEN_THIS)) {
        int line = parser->previous.line;
        int col = parser->previous.column;
        return create_this_expr(NULL, line, col); // class_name will be filled by semantic analyzer
    }
    
    // Super keyword - parent class member access
    if (match(parser, TOKEN_SUPER)) {
        int line = parser->previous.line;
        int col = parser->previous.column;
        
        consume(parser, TOKEN_DOT, "Expected '.' after 'super'");
        consume(parser, TOKEN_IDENTIFIER, "Expected member name after 'super.'");
        Token member = parser->previous;
        
        Expr* expr = ALLOCATE(Expr, 1);
        expr->type = EXPR_SUPER;
        expr->line = line;
        expr->column = col;
        expr->data_type = TYPE_CLASS;
        expr->class_name = NULL;
        expr->type_info = NULL;
        
        // Copy member name
        expr->as.super_expr.member_name = ALLOCATE(char, member.length + 1);
        memcpy(expr->as.super_expr.member_name, member.start, member.length);
        expr->as.super_expr.member_name[member.length] = '\0';
        
        // Check if it's a method call
        if (match(parser, TOKEN_LPAREN)) {
            expr->as.super_expr.is_method_call = true;
            
            // Parse arguments
            int capacity = 4;
            int count = 0;
            Expr** args = ALLOCATE(Expr*, capacity);
            
            if (!check(parser, TOKEN_RPAREN)) {
                do {
                    if (count >= capacity) {
                        capacity = GROW_CAPACITY(capacity);
                        args = GROW_ARRAY(Expr*, args, count, capacity);
                    }
                    args[count++] = expression(parser);
                } while (match(parser, TOKEN_COMMA));
            }
            
            consume(parser, TOKEN_RPAREN, "Expected ')' after arguments");
            
            expr->as.super_expr.arguments = args;
            expr->as.super_expr.arg_count = count;
        } else {
            // Field access
            expr->as.super_expr.is_method_call = false;
            expr->as.super_expr.arguments = NULL;
            expr->as.super_expr.arg_count = 0;
        }
        
        return expr;
    }

    // move expression: move varName — transfers ownership, invalidates source
    if (match(parser, TOKEN_MOVE)) {
        int line = parser->previous.line;
        int col  = parser->previous.column;
        consume(parser, TOKEN_IDENTIFIER, "Expected variable name after 'move'");
        char* var_name = copy_identifier(&parser->previous);

        Expr* expr = ALLOCATE(Expr, 1);
        expr->type           = EXPR_VARIABLE;
        expr->line           = line;
        expr->column         = col;
        expr->data_type      = TYPE_VOID;  /* resolved by semantic pass */
        expr->class_name     = NULL;
        expr->type_info      = NULL;
        expr->as.variable.name    = var_name;
        expr->as.variable.is_move = true;  /* <-- marks this as a move */
        return expr;
    }

    // new expression: new ClassName(args)
    if (match(parser, TOKEN_NEW)) {
        int line = parser->previous.line;
        int col = parser->previous.column;

        consume(parser, TOKEN_IDENTIFIER, "Expected class name after 'new'");
        char* class_name = copy_identifier(&parser->previous);

        if (check(parser, TOKEN_LESS)) {
            advance(parser);
            do {
                char* generic_arg_class = NULL;
                parse_type_with_class(parser, &generic_arg_class);
                free(generic_arg_class);
            } while (match(parser, TOKEN_COMMA));
            consume_generic_close(parser, "Expected '>' after generic type arguments");
        }

        consume(parser, TOKEN_LPAREN, "Expected '(' after class name");

        // Parse constructor arguments
        int capacity = 8;
        int count = 0;
        Expr** args = ALLOCATE(Expr*, capacity);

        // Handle empty parameter list explicitly
        if (check(parser, TOKEN_RPAREN)) {
            // Empty parameter list
            consume(parser, TOKEN_RPAREN, "Expected ')' after arguments");
            return create_new_expr(class_name, args, 0, line, col);
        }

        // Parse argument list
        do {
            if (count >= capacity) {
                capacity = GROW_CAPACITY(capacity);
                args = GROW_ARRAY(Expr*, args, count, capacity);
            }
            args[count++] = expression(parser);
        } while (match(parser, TOKEN_COMMA));

        consume(parser, TOKEN_RPAREN, "Expected ')' after arguments");
        return create_new_expr(class_name, args, count, line, col);
    }

    if (match(parser, TOKEN_IDENTIFIER)) {
        Token name_token = parser->previous;
        char* name = copy_identifier(&name_token);

        if (strcmp(name, "dyn") == 0) {
            if (check(parser, TOKEN_LBRACKET)) {
                parse_dyn_trait_bounds(parser);
            }
        }

        // Check for generic type instantiation: List<Int>, Map<String, Int>
        // Disambiguate from comparison: if '<' is followed by a type keyword or
        // uppercase identifier (class/struct name), treat as generics.
        // Otherwise, treat '<' as the less-than comparison operator.
        if (check(parser, TOKEN_LESS)) {
            // Peek at what follows '<' to disambiguate
            // Heuristic: the token after '<' must be a type keyword or an
            // uppercase identifier (convention for class/type names)
            // We peek by checking the character after '<'
            bool is_generic = false;
            {
                // Look ahead: what's the next token after '<'?
                Lexer peek_lexer;
                init_lexer(&peek_lexer, parser->current.start);
                // Skip the '<' token
                Token lt = scan_token(&peek_lexer);
                (void)lt;
                Token next = scan_token(&peek_lexer);
                // If the next token is a type keyword, it's a generic instantiation
                if (is_simple_type_token(next.type) ||
                    is_parameterized_type_token(next.type) ||
                    next.type == TOKEN_IDENTIFIER) {
                    // For identifiers, check if it's capitalized (class/type convention)
                    if (next.type == TOKEN_IDENTIFIER && next.length > 0) {
                        is_generic = (next.start[0] >= 'A' && next.start[0] <= 'Z');
                    } else if (next.type != TOKEN_IDENTIFIER) {
                        is_generic = true;  // Type keyword like i32, f64, string, etc.
                    }
                }
            }

            if (is_generic) {
            advance(parser);  // consume '<'

            // Parse type arguments
            int type_arg_capacity = 4;
            int type_arg_count = 0;
            DataType* type_args = ALLOCATE(DataType, type_arg_capacity);
            char** type_arg_classes = ALLOCATE(char*, type_arg_capacity);

            do {
                if (type_arg_count >= type_arg_capacity) {
                    type_arg_capacity = GROW_CAPACITY(type_arg_capacity);
                    type_args = GROW_ARRAY(DataType, type_args, type_arg_count, type_arg_capacity);
                    type_arg_classes = GROW_ARRAY(char*, type_arg_classes, type_arg_count, type_arg_capacity);
                }

                char* arg_class_name = NULL;
                type_args[type_arg_count] = parse_type_with_class(parser, &arg_class_name);
                type_arg_classes[type_arg_count] = arg_class_name;
                type_arg_count++;
            } while (match(parser, TOKEN_COMMA));

            consume_generic_close(parser, "Expected '>' after generic type arguments");

            // Create generic instantiation expression
            Expr* expr = ALLOCATE(Expr, 1);
            expr->type = EXPR_GENERIC_INST;
            expr->line = name_token.line;
            expr->column = name_token.column;
            expr->data_type = TYPE_CLASS;  // Generic instantiation produces a class type
            expr->class_name = NULL;  // Will be set during semantic analysis
            expr->type_info = NULL;

            expr->as.generic_inst.base_name = name;
            expr->as.generic_inst.type_args = type_args;
            expr->as.generic_inst.type_arg_classes = type_arg_classes;
            expr->as.generic_inst.type_arg_count = type_arg_count;

            return expr;
            } // end if (is_generic)
        } // end if (check TOKEN_LESS)

        // Check for static member access: ClassName.member
        if (check(parser, TOKEN_DOT)) {
            // This could be either:
            // 1. Variable followed by member access (handled in postfix)
            // 2. ClassName.staticMember (static access)
            // We'll create a variable expr and let semantic analysis determine if it's a class
            // Actually, let's handle it here to avoid ambiguity

            // Peek ahead: if identifier starts with uppercase, might be class name
            // For now, create variable expr and let postfix/semantic handle it
            // We'll convert EXPR_MEMBER_ACCESS to EXPR_STATIC_ACCESS in semantic if needed
            return create_variable_expr(name, name_token.line, name_token.column);
        }

        return create_variable_expr(name, name_token.line, name_token.column);
    }
    
    if (match(parser, TOKEN_LPAREN)) {
        Expr* expr = expression(parser);
        while (match(parser, TOKEN_COMMA)) {
            expr = expression(parser);
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after expression");
        return expr;
    }

    // Lambda expression: |x: Int| => x + 1
    if (match(parser, TOKEN_PIPE)) {
        return lambda_expression(parser);
    }

    if (match(parser, TOKEN_AWAIT)) {
        Token op = parser->previous;
        Expr* expression = unary(parser);
        return create_await_expr(expression, op.line, op.column);
    }

    // Array literal: [1, 2, 3]
    if (match(parser, TOKEN_LBRACKET)) {
        int line = parser->previous.line;
        int col = parser->previous.column;
        int capacity = 8;
        int count = 0;
        Expr** elements = ALLOCATE(Expr*, capacity);

        if (!check(parser, TOKEN_RBRACKET)) {
            do {
                if (count >= capacity) {
                    capacity = GROW_CAPACITY(capacity);
                    elements = GROW_ARRAY(Expr*, elements, count, capacity);
                }
                elements[count++] = expression(parser);
            } while (match(parser, TOKEN_COMMA));
        }

        consume(parser, TOKEN_RBRACKET, "Expected ']' after array literal elements");
        return create_array_literal_expr(elements, count, line, col);
    }

    error_at_current(parser, "Expected expression");
    return error_expression(parser->current.line, parser->current.column);
}

static Expr* unary(Parser* parser) {
    if (match(parser, TOKEN_NOT) || match(parser, TOKEN_MINUS) ||
        match(parser, TOKEN_BITNOT)) {
        Token op = parser->previous;
        Expr* operand = unary(parser);
        return create_unary_expr(op.type, operand, op.line, op.column);
    }

    return postfix(parser);
}

// Postfix expressions (member access with dot operator and array indexing)
static Expr* postfix(Parser* parser) {
    Expr* expr = primary(parser);

    while (match(parser, TOKEN_DOT) || match(parser, TOKEN_LBRACKET) || match(parser, TOKEN_LPAREN)) {
        if (parser->previous.type == TOKEN_LPAREN) {
            int line = parser->previous.line;
            int col = parser->previous.column;
            int count = 0;
            Expr** args = parse_call_arguments(parser, &count, "Expected ')' after arguments");
            expr = create_call_expr(expr, args, count, line, col);
            continue;
        }

        if (parser->previous.type == TOKEN_LBRACKET) {
            // Array indexing: arr[index]
            int line = parser->previous.line;
            int col = parser->previous.column;

            Expr* index = expression(parser);
            consume(parser, TOKEN_RBRACKET, "Expected ']' after array index");

            expr = create_index_expr(expr, index, line, col);
            continue;
        }

        // Member access with dot
        int line = parser->previous.line;
        int col = parser->previous.column;

        char* member_name = NULL;
        if (match(parser, TOKEN_IDENTIFIER)) {
            member_name = copy_identifier(&parser->previous);
        } else if (match(parser, TOKEN_PRINT)) {
            member_name = strdup("print");
        } else if (match(parser, TOKEN_UNION_KW)) {
            member_name = strdup("union");
        } else {
            error_at_current(parser, "Expected property or method name after '.'");
            member_name = strdup("");
        }

        // Check if it's a method call (followed by parentheses)
        if (match(parser, TOKEN_LPAREN)) {
            // Method call
            int capacity = 8;
            int count = 0;
            Expr** args = ALLOCATE(Expr*, capacity);

            // Handle empty parameter list explicitly
            if (check(parser, TOKEN_RPAREN)) {
                // Empty parameter list - just consume the closing paren
                consume(parser, TOKEN_RPAREN, "Expected ')' after method arguments");
                expr = create_member_access_expr(expr, member_name, true, args, 0, line, col);
            } else {
                // Parse argument list
                do {
                    if (count >= capacity) {
                        capacity = GROW_CAPACITY(capacity);
                        args = GROW_ARRAY(Expr*, args, count, capacity);
                    }
                    args[count++] = expression(parser);
                } while (match(parser, TOKEN_COMMA));

                consume(parser, TOKEN_RPAREN, "Expected ')' after method arguments");
                expr = create_member_access_expr(expr, member_name, true, args, count, line, col);
            }
        } else {
            // Field access
            expr = create_member_access_expr(expr, member_name, false, NULL, 0, line, col);
        }
    }

    return expr;
}

// Helper for postfix parsing when we already have the base expression
static Expr* postfix_with_expr(Parser* parser, Expr* expr) {
    while (match(parser, TOKEN_DOT) || match(parser, TOKEN_LBRACKET) || match(parser, TOKEN_LPAREN)) {
        if (parser->previous.type == TOKEN_LPAREN) {
            int line = parser->previous.line;
            int col = parser->previous.column;
            int count = 0;
            Expr** args = parse_call_arguments(parser, &count, "Expected ')' after arguments");
            expr = create_call_expr(expr, args, count, line, col);
            continue;
        }

        if (parser->previous.type == TOKEN_LBRACKET) {
            // Array indexing: arr[index]
            int line = parser->previous.line;
            int col = parser->previous.column;

            Expr* index = expression(parser);
            consume(parser, TOKEN_RBRACKET, "Expected ']' after array index");

            expr = create_index_expr(expr, index, line, col);
            continue;
        }

        // Member access with dot
        int line = parser->previous.line;
        int col = parser->previous.column;

        char* member_name = NULL;
        if (match(parser, TOKEN_IDENTIFIER)) {
            member_name = copy_identifier(&parser->previous);
        } else if (match(parser, TOKEN_PRINT)) {
            member_name = strdup("print");
        } else if (match(parser, TOKEN_UNION_KW)) {
            member_name = strdup("union");
        } else {
            error_at_current(parser, "Expected property or method name after '.'");
            member_name = strdup("");
        }

        // Check if it's a method call (followed by parentheses)
        if (match(parser, TOKEN_LPAREN)) {
            // Method call
            int capacity = 8;
            int count = 0;
            Expr** args = ALLOCATE(Expr*, capacity);

            // Handle empty parameter list explicitly
            if (check(parser, TOKEN_RPAREN)) {
                // Empty parameter list - just consume the closing paren
                consume(parser, TOKEN_RPAREN, "Expected ')' after method arguments");
                expr = create_member_access_expr(expr, member_name, true, args, 0, line, col);
            } else {
                // Parse argument list
                do {
                    if (count >= capacity) {
                        capacity = GROW_CAPACITY(capacity);
                        args = GROW_ARRAY(Expr*, args, count, capacity);
                    }
                    args[count++] = expression(parser);
                } while (match(parser, TOKEN_COMMA));

                consume(parser, TOKEN_RPAREN, "Expected ')' after method arguments");
                expr = create_member_access_expr(expr, member_name, true, args, count, line, col);
            }
        } else {
            // Field access
            expr = create_member_access_expr(expr, member_name, false, NULL, 0, line, col);
        }
    }

    return expr;
}

static Expr* factor(Parser* parser) {
    Expr* expr = unary(parser);

    while (match(parser, TOKEN_STAR) || match(parser, TOKEN_SLASH) || match(parser, TOKEN_PERCENT)) {
        Token op = parser->previous;
        Expr* right = postfix(parser);
        expr = create_binary_expr(expr, op.type, right, op.line, op.column);
    }

    return expr;
}

static Expr* term(Parser* parser) {
    Expr* expr = factor(parser);
    
    while (match(parser, TOKEN_PLUS) || match(parser, TOKEN_MINUS)) {
        Token op = parser->previous;
        Expr* right = factor(parser);
        expr = create_binary_expr(expr, op.type, right, op.line, op.column);
    }
    
    return expr;
}

// Bitwise shift: << >>
static Expr* shift(Parser* parser) {
    Expr* expr = term(parser);
    while (match(parser, TOKEN_LSHIFT) || match(parser, TOKEN_RSHIFT)) {
        Token op = parser->previous;
        Expr* right = term(parser);
        expr = create_binary_expr(expr, op.type, right, op.line, op.column);
    }
    return expr;
}

static Expr* comparison(Parser* parser) {
    Expr* expr = shift(parser);
    
    while (match(parser, TOKEN_LESS) || match(parser, TOKEN_LESS_EQUAL) ||
           match(parser, TOKEN_GREATER) || match(parser, TOKEN_GREATER_EQUAL)) {
        Token op = parser->previous;
        Expr* right = shift(parser);
        expr = create_binary_expr(expr, op.type, right, op.line, op.column);
    }
    
    return expr;
}

static Expr* equality(Parser* parser) {
    Expr* expr = comparison(parser);
    
    while (match(parser, TOKEN_EQUAL) || match(parser, TOKEN_NOT_EQUAL)) {
        Token op = parser->previous;
        Expr* right = comparison(parser);
        expr = create_binary_expr(expr, op.type, right, op.line, op.column);
    }
    
    return expr;
}

// Bitwise AND: &
static Expr* bitwise_and(Parser* parser) {
    Expr* expr = equality(parser);
    while (match(parser, TOKEN_BITAND)) {
        Token op = parser->previous;
        Expr* right = equality(parser);
        expr = create_binary_expr(expr, op.type, right, op.line, op.column);
    }
    return expr;
}

// Bitwise XOR: ^
static Expr* bitwise_xor(Parser* parser) {
    Expr* expr = bitwise_and(parser);
    while (match(parser, TOKEN_BITXOR)) {
        Token op = parser->previous;
        Expr* right = bitwise_and(parser);
        expr = create_binary_expr(expr, op.type, right, op.line, op.column);
    }
    return expr;
}

// Bitwise OR: | (TOKEN_PIPE in non-lambda context)
static Expr* bitwise_or(Parser* parser) {
    Expr* expr = bitwise_xor(parser);
    // TOKEN_PIPE is used for bitwise OR when not in lambda context
    // Lambda uses TOKEN_PIPE as the opening delimiter only in primary()
    while (check(parser, TOKEN_PIPE)) {
        // Only treat as bitwise OR if not followed by an identifier and ':'
        // (which would indicate a lambda parameter)
        // We use a simple heuristic: peek ahead
        Lexer peek_lexer;
        init_lexer(&peek_lexer, parser->current.start);
        Token pipe_tok = scan_token(&peek_lexer);  // consume |
        Token after_pipe = scan_token(&peek_lexer);
        Token after_id   = scan_token(&peek_lexer);
        bool is_lambda_start = (after_pipe.type == TOKEN_IDENTIFIER &&
                                after_id.type == TOKEN_COLON) ||
                               (after_pipe.type == TOKEN_PIPE);  // || empty lambda
        if (is_lambda_start) break;  // let primary() handle it as lambda
        (void)pipe_tok;
        advance(parser);  // consume TOKEN_PIPE as bitwise OR
        Token op = parser->previous;
        Expr* right = bitwise_xor(parser);
        expr = create_binary_expr(expr, op.type, right, op.line, op.column);
    }
    return expr;
}

static Expr* logical_and(Parser* parser) {
    Expr* expr = bitwise_or(parser);
    
    while (match(parser, TOKEN_AND)) {
        Token op = parser->previous;
        Expr* right = bitwise_or(parser);
        expr = create_binary_expr(expr, op.type, right, op.line, op.column);
    }
    
    return expr;
}

static Expr* logical_or(Parser* parser) {
    Expr* expr = logical_and(parser);
    
    while (match(parser, TOKEN_OR)) {
        Token op = parser->previous;
        Expr* right = logical_and(parser);
        expr = create_binary_expr(expr, op.type, right, op.line, op.column);
    }
    
    return expr;
}

static Expr* expression(Parser* parser) {
    return logical_or(parser);
}

// Statement parsing
static Stmt* print_statement(Parser* parser) {
    int line = parser->previous.line;
    int col = parser->previous.column;
    
    consume(parser, TOKEN_LPAREN, "Expected '(' after 'print'");
    Expr* expr = expression(parser);
    consume(parser, TOKEN_RPAREN, "Expected ')' after expression");
    // Semicolons are now optional
    match(parser, TOKEN_SEMICOLON);
    
    return create_print_stmt(expr, line, col);
}

static Stmt* block_statement(Parser* parser) {
    int line = parser->previous.line;
    int col = parser->previous.column;
    
    int capacity = 8;
    int count = 0;
    Stmt** statements = ALLOCATE(Stmt*, capacity);
    
    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (count >= capacity) {
            capacity = GROW_CAPACITY(capacity);
            statements = GROW_ARRAY(Stmt*, statements, count, capacity);
        }
        statements[count++] = declaration(parser);

        if (parser->panic_mode) {
            synchronize_block(parser);
        }
    }
    
    consume(parser, TOKEN_RBRACE, "Expected '}' after block");
    return create_block_stmt(statements, count, line, col);
}

static Stmt* implicit_block_statement(Parser* parser, int line, int col) {
    int capacity = 8;
    int count = 0;
    Stmt** statements = ALLOCATE(Stmt*, capacity);

    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (count >= capacity) {
            capacity = GROW_CAPACITY(capacity);
            statements = GROW_ARRAY(Stmt*, statements, count, capacity);
        }
        statements[count++] = declaration(parser);

        if (parser->panic_mode) {
            synchronize_block(parser);
        }
    }

    consume(parser, TOKEN_RBRACE, "Expected '}' after implicit block");
    return create_block_stmt(statements, count, line, col);
}

static Stmt* if_statement(Parser* parser) {
    int line = parser->previous.line;
    int col = parser->previous.column;

    Expr* condition = expression(parser);
    consume(parser, TOKEN_LBRACE, "Expected '{' after if condition");
    Stmt* then_branch = block_statement(parser);

    Stmt* else_branch = NULL;

    // Handle Elif chains
    if (match(parser, TOKEN_ELIF)) {
        // Treat Elif as nested If statement (else { if { ... } })
        else_branch = if_statement(parser);
    }
    else if (match(parser, TOKEN_ELSE)) {
        consume(parser, TOKEN_LBRACE, "Expected '{' after else");
        else_branch = block_statement(parser);
    }

    return create_if_stmt(condition, then_branch, else_branch, line, col);
}

static Stmt* while_statement(Parser* parser) {
    int line = parser->previous.line;
    int col = parser->previous.column;

    // Parens around condition are optional: both `while (cond) {` and `while cond {` work
    bool has_parens = match(parser, TOKEN_LPAREN);
    Expr* condition = expression(parser);
    if (has_parens) {
        consume(parser, TOKEN_RPAREN, "Expected ')' after condition");
    }
    consume(parser, TOKEN_LBRACE, "Expected '{' after while condition");
    Stmt* body = block_statement(parser);

    return create_while_stmt(condition, body, line, col);
}

static Stmt* for_statement(Parser* parser) {
    int line = parser->previous.line;
    int col  = parser->previous.column;

    // Detect for-in style: `for ident in expr {`
    bool is_for_in = false;
    if (check(parser, TOKEN_IDENTIFIER)) {
        Lexer peek_lexer;
        init_lexer(&peek_lexer, parser->current.start);
        scan_token(&peek_lexer);  // consume identifier
        Token in_tok = scan_token(&peek_lexer);
        if (in_tok.type == TOKEN_IN) {
            is_for_in = true;
        }
    }

    if (is_for_in) {
        consume(parser, TOKEN_IDENTIFIER, "Expected variable name");
        char* var_name = copy_identifier(&parser->previous);
        consume(parser, TOKEN_IN, "Expected 'in' after loop variable");
        Expr* iterable = expression(parser);
        consume(parser, TOKEN_LBRACE, "Expected '{' before for-in body");
        Stmt* body = block_statement(parser);

        Stmt* stmt = ALLOCATE(Stmt, 1);
        stmt->type   = STMT_FOR_IN;
        stmt->line   = line;
        stmt->column = col;
        stmt->as.for_in_stmt.var_name  = var_name;
        stmt->as.for_in_stmt.var_type  = TYPE_ERROR; // inferred by semantic
        stmt->as.for_in_stmt.iterable  = iterable;
        stmt->as.for_in_stmt.body      = body;
        return stmt;
    }

    // C-style for(init; cond; incr) { body }
    consume(parser, TOKEN_LPAREN, "Expected '(' after 'for'");
    
    consume(parser, TOKEN_IDENTIFIER, "Expected variable name");
    char* var_name = copy_identifier(&parser->previous);
    
    consume(parser, TOKEN_COLON, "Expected ':' after variable name");
    DataType var_type = parse_type(parser);
    
    consume(parser, TOKEN_ASSIGN, "Expected '=' after type");
    Expr* initializer = expression(parser);
    
    consume(parser, TOKEN_SEMICOLON, "Expected ';' after initializer");
    
    Expr* condition = expression(parser);
    consume(parser, TOKEN_SEMICOLON, "Expected ';' after condition");
    
    consume(parser, TOKEN_IDENTIFIER, "Expected variable name");
    int incr_line = parser->previous.line;
    int incr_col  = parser->previous.column;
    char* incr_var_name = copy_identifier(&parser->previous);
    Expr* incr_var = create_variable_expr(incr_var_name, incr_line, incr_col);
    consume(parser, TOKEN_ASSIGN, "Expected '=' in increment");
    Expr* incr_value = expression(parser);
    Stmt* increment  = create_assignment_stmt(incr_var, incr_value, line, col);
    
    consume(parser, TOKEN_RPAREN, "Expected ')' after for clauses");
    consume(parser, TOKEN_LBRACE, "Expected '{' after for header");
    Stmt* body = block_statement(parser);
    
    return create_for_stmt(var_name, var_type, initializer, condition, increment, body, line, col);
}

static Stmt* return_statement(Parser* parser) {
    int line = parser->previous.line;
    int col = parser->previous.column;

    Expr* value = NULL;
    if (!check(parser, TOKEN_SEMICOLON) && !check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        value = expression(parser);
    }

    // Semicolons are optional
    match(parser, TOKEN_SEMICOLON);
    return create_return_stmt(value, line, col);
}

// ============================================================
// throw statement: throw <expr>;
// ============================================================
static Stmt* throw_statement(Parser* parser) {
    int line = parser->previous.line;
    int col  = parser->previous.column;
    Expr* val = expression(parser);
    match(parser, TOKEN_SEMICOLON);
    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type   = STMT_THROW;
    stmt->line   = line;
    stmt->column = col;
    stmt->as.throw_stmt.value = val;
    return stmt;
}

// ============================================================
// match statement: match <expr> { <pattern> => <stmt>, ... }
// ============================================================
static Stmt* match_statement(Parser* parser) {
    int line = parser->previous.line;
    int col  = parser->previous.column;

    // Parse subject expression (no parens required, both allowed)
    bool has_parens = match(parser, TOKEN_LPAREN);
    Expr* subject = expression(parser);
    if (has_parens) consume(parser, TOKEN_RPAREN, "Expected ')' after match subject");

    consume(parser, TOKEN_LBRACE, "Expected '{' before match arms");

    int capacity = 8;
    int count = 0;
    MatchArm* arms = ALLOCATE(MatchArm, capacity);

    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (count >= capacity) {
            capacity = GROW_CAPACITY(capacity);
            arms = GROW_ARRAY(MatchArm, arms, count, capacity);
        }
        MatchArm* arm = &arms[count++];
        memset(arm, 0, sizeof(MatchArm));

        // Wildcard: _ => body
        if (match(parser, TOKEN_IDENTIFIER) &&
            parser->previous.length == 1 && *parser->previous.start == '_') {
            arm->pattern = NULL;  // wildcard
        } else {
            // Step back and parse as expression pattern
            // (we consumed TOKEN_IDENTIFIER in the check above if it matched)
            // Re-parse the pattern as a full expression:
            arm->pattern = expression(parser);
        }

        consume(parser, TOKEN_FAT_ARROW, "Expected '=>' after match pattern");

        // Body: either { block } or a single statement
        if (check(parser, TOKEN_LBRACE)) {
            advance(parser);
            arm->body = block_statement(parser);
        } else {
            arm->body = statement(parser);
        }

        match(parser, TOKEN_COMMA);  // optional trailing comma
    }

    consume(parser, TOKEN_RBRACE, "Expected '}' after match arms");

    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type   = STMT_MATCH;
    stmt->line   = line;
    stmt->column = col;
    stmt->as.match_stmt.subject   = subject;
    stmt->as.match_stmt.arms      = arms;
    stmt->as.match_stmt.arm_count = count;
    return stmt;
}

// ============================================================
// try/catch/finally
// ============================================================
static Stmt* try_statement(Parser* parser) {
    int line = parser->previous.line;
    int col  = parser->previous.column;

    consume(parser, TOKEN_LBRACE, "Expected '{' after 'try'");
    Stmt* try_body = block_statement(parser);

    int cap = 4, cnt = 0;
    CatchClause* catches = ALLOCATE(CatchClause, cap);

    while (match(parser, TOKEN_CATCH)) {
        if (cnt >= cap) {
            cap = GROW_CAPACITY(cap);
            catches = GROW_ARRAY(CatchClause, catches, cnt, cap);
        }
        CatchClause* clause = &catches[cnt++];
        memset(clause, 0, sizeof(CatchClause));

        consume(parser, TOKEN_LPAREN, "Expected '(' after 'catch'");
        if (!check(parser, TOKEN_RPAREN)) {
            consume(parser, TOKEN_IDENTIFIER, "Expected exception variable name");
            clause->exception_var = copy_identifier(&parser->previous);

            if (match(parser, TOKEN_COLON)) {
                consume(parser, TOKEN_IDENTIFIER, "Expected exception type name");
                clause->exception_type = copy_identifier(&parser->previous);
            }
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after catch parameter");
        consume(parser, TOKEN_LBRACE, "Expected '{' after catch clause");
        clause->body = block_statement(parser);
    }

    Stmt* finally_body = NULL;
    if (match(parser, TOKEN_FINALLY)) {
        consume(parser, TOKEN_LBRACE, "Expected '{' after 'finally'");
        finally_body = block_statement(parser);
    }

    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type   = STMT_TRY;
    stmt->line   = line;
    stmt->column = col;
    stmt->as.try_stmt.try_body     = try_body;
    stmt->as.try_stmt.catches      = catches;
    stmt->as.try_stmt.catch_count  = cnt;
    stmt->as.try_stmt.finally_body = finally_body;
    return stmt;
}

static Stmt* expression_statement(Parser* parser) {
    Expr* expr = expression(parser);

    // Check for simple or compound assignment
    TokenType assign_op = parser->current.type;
    if (assign_op == TOKEN_ASSIGN ||
        assign_op == TOKEN_PLUS_ASSIGN  || assign_op == TOKEN_MINUS_ASSIGN ||
        assign_op == TOKEN_STAR_ASSIGN  || assign_op == TOKEN_SLASH_ASSIGN ||
        assign_op == TOKEN_PERCENT_ASSIGN) {

        advance(parser);  // consume the assignment token
        int line = parser->previous.line;
        int col  = parser->previous.column;

        if (expr->type != EXPR_VARIABLE && expr->type != EXPR_MEMBER_ACCESS &&
            expr->type != EXPR_INDEX) {
            error_at_current(parser, "Invalid assignment target");
        }

        Expr* value = expression(parser);

        // Desugar compound assignment: x += y  =>  x = x + y
        if (assign_op != TOKEN_ASSIGN) {
            TokenType bin_op;
            switch (assign_op) {
                case TOKEN_PLUS_ASSIGN:    bin_op = TOKEN_PLUS;    break;
                case TOKEN_MINUS_ASSIGN:   bin_op = TOKEN_MINUS;   break;
                case TOKEN_STAR_ASSIGN:    bin_op = TOKEN_STAR;    break;
                case TOKEN_SLASH_ASSIGN:   bin_op = TOKEN_SLASH;   break;
                case TOKEN_PERCENT_ASSIGN: bin_op = TOKEN_PERCENT; break;
                default: bin_op = TOKEN_PLUS; break;
            }
            // Clone the target expression for the right-hand side
            Expr* lhs_copy = ALLOCATE(Expr, 1);
            memcpy(lhs_copy, expr, sizeof(Expr));
            value = create_binary_expr(lhs_copy, bin_op, value, line, col);
        }

        match(parser, TOKEN_SEMICOLON);
        return create_assignment_stmt(expr, value, line, col);
    }

    // Semicolons are optional
    match(parser, TOKEN_SEMICOLON);
    return create_expr_stmt(expr, expr->line, expr->column);
}

static Stmt* statement(Parser* parser) {
    if (match(parser, TOKEN_PRINT)) return print_statement(parser);
    if (match(parser, TOKEN_IF)) return if_statement(parser);
    if (match(parser, TOKEN_WHILE)) return while_statement(parser);
    if (match(parser, TOKEN_FOR)) return for_statement(parser);
    if (match(parser, TOKEN_RETURN)) return return_statement(parser);
    if (match(parser, TOKEN_LBRACE)) return block_statement(parser);
    if (match(parser, TOKEN_MATCH)) return match_statement(parser);
    if (match(parser, TOKEN_TRY)) return try_statement(parser);
    if (match(parser, TOKEN_THROW)) return throw_statement(parser);
    
    // Break statement
    if (match(parser, TOKEN_BREAK)) {
        int line = parser->previous.line;
        int col = parser->previous.column;
        match(parser, TOKEN_SEMICOLON);
        Stmt* stmt = ALLOCATE(Stmt, 1);
        stmt->type = STMT_BREAK;
        stmt->line = line;
        stmt->column = col;
        return stmt;
    }

    // Continue statement
    if (match(parser, TOKEN_CONTINUE)) {
        int line = parser->previous.line;
        int col = parser->previous.column;
        match(parser, TOKEN_SEMICOLON);
        Stmt* stmt = ALLOCATE(Stmt, 1);
        stmt->type = STMT_CONTINUE;
        stmt->line = line;
        stmt->column = col;
        return stmt;
    }
    
    return expression_statement(parser);
}

static Stmt* var_declaration(Parser* parser) {
    Token name_token = parser->previous;
    char* name = copy_identifier(&name_token);

    consume(parser, TOKEN_COLON, "Expected ':' after variable name");
    char* class_name = NULL;
    DataType type = parse_type_with_class(parser, &class_name);

    Expr* initializer = NULL;
    if (match(parser, TOKEN_ASSIGN)) {
        initializer = expression(parser);
    }

    // Semicolons are now optional
    match(parser, TOKEN_SEMICOLON);
    Stmt* stmt = create_declaration_stmt(name, type, initializer, name_token.line, name_token.column);
    stmt->as.declaration.class_name = class_name;  // Set class name if applicable
    return stmt;
}

static Stmt* function_declaration(Parser* parser) {
    int line = parser->previous.line;
    int col = parser->previous.column;

    consume(parser, TOKEN_IDENTIFIER, "Expected function name");
    char* name = copy_identifier(&parser->previous);

    // Check for generic type parameters: Func swap<T>(a: T, b: T) -> Void
    TypeParam* type_params = NULL;
    int type_param_count = 0;
    if (match(parser, TOKEN_LESS)) {
        type_params = parse_type_params(parser, &type_param_count);
    }

    consume(parser, TOKEN_LPAREN, "Expected '(' after function name");

    // Parameters
    int param_capacity = 8;
    int param_count = 0;
    Parameter* params = ALLOCATE(Parameter, param_capacity);

    if (!check(parser, TOKEN_RPAREN)) {
        do {
            if (param_count >= param_capacity) {
                param_capacity = GROW_CAPACITY(param_capacity);
                params = GROW_ARRAY(Parameter, params, param_count, param_capacity);
            }

            consume(parser, TOKEN_IDENTIFIER, "Expected parameter name");
            params[param_count].name = copy_identifier(&parser->previous);

            consume(parser, TOKEN_COLON, "Expected ':' after parameter name");
            char* param_class_name = NULL;
            params[param_count].type = parse_type_with_class(parser, &param_class_name);
            params[param_count].class_name = param_class_name;
            /* Retain a `lambda(...) -> R` signature so calls through this
             * parameter can be arity-checked (see semantic.c). */
            params[param_count].type_info = take_pending_fn_type_info();
            params[param_count].ownership = OWNERSHIP_OWNED;

            param_count++;
        } while (match(parser, TOKEN_COMMA));
    }

    consume(parser, TOKEN_RPAREN, "Expected ')' after parameters");

    // Return type is optional: func foo() { ... } defaults to void
    DataType return_type = TYPE_VOID;
    TypeInfo* return_type_info = NULL;
    if (match(parser, TOKEN_ARROW)) {
        char* return_class_name = NULL;
        return_type = parse_type_with_class(parser, &return_class_name);
        free(return_class_name);
        /* Retain a `-> lambda(...) -> R` signature for arity checking of calls
         * made on this function's result. */
        return_type_info = take_pending_fn_type_info();
    }

    Stmt* body;
    if (match(parser, TOKEN_LBRACE)) {
        body = block_statement(parser);
    } else {
        body = implicit_block_statement(parser, line, col);
    }

    Stmt* stmt = create_function_stmt(name, params, param_count, return_type, body, line, col);
    stmt->as.function.return_type_info = return_type_info;
    // Add generic type parameters
    stmt->as.function.type_params = type_params;
    stmt->as.function.type_param_count = type_param_count;
    return stmt;
}

static Stmt* class_declaration(Parser* parser) {
    int line = parser->previous.line;
    int col = parser->previous.column;

    consume(parser, TOKEN_IDENTIFIER, "Expected class name");
    char* class_name = copy_identifier(&parser->previous);

    // Check for generic type parameters: Class List<T> or Class Map<K, V>
    TypeParam* type_params = NULL;
    int type_param_count = 0;
    if (match(parser, TOKEN_LESS)) {
        type_params = parse_type_params(parser, &type_param_count);
    }

    // Check for inheritance
    char* parent_name = NULL;
    if (match(parser, TOKEN_EXTENDS)) {
        consume(parser, TOKEN_IDENTIFIER, "Expected parent class name after 'extends'");
        parent_name = copy_identifier(&parser->previous);
    }

    // Optional trait list: class Point implements Printable, Comparable { ... }
    char** implements = NULL;
    int implements_count = 0;
    if (match(parser, TOKEN_IMPLEMENTS)) {
        implements = parse_implements_list(parser, &implements_count);
    }

    consume(parser, TOKEN_LBRACE, "Expected '{' before class body");

    // Parse fields and methods
    int field_capacity = 8;
    int field_count = 0;
    FieldDecl* fields = ALLOCATE(FieldDecl, field_capacity);

    int method_capacity = 8;
    int method_count = 0;
    MethodDecl* methods = ALLOCATE(MethodDecl, method_capacity);

    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        // Parse member modifiers in any order.
        AccessModifier access = ACCESS_PUBLIC;
        bool saw_access_modifier = false;
        bool is_static = false;
        bool is_abstract_method = false;
        ClassFieldBindingKind binding_kind = CLASS_FIELD_BINDING_LEGACY;
        bool has_binding_keyword = false;

        for (;;) {
            if (match(parser, TOKEN_PUBLIC)) {
                if (saw_access_modifier) {
                    error_at(parser, &parser->previous,
                             "Duplicate access modifier in class member declaration");
                }
                access = ACCESS_PUBLIC;
                saw_access_modifier = true;
                continue;
            }

            if (match(parser, TOKEN_PRIVATE)) {
                if (saw_access_modifier) {
                    error_at(parser, &parser->previous,
                             "Duplicate access modifier in class member declaration");
                }
                access = ACCESS_PRIVATE;
                saw_access_modifier = true;
                continue;
            }

            if (match(parser, TOKEN_PROTECTED)) {
                if (saw_access_modifier) {
                    error_at(parser, &parser->previous,
                             "Duplicate access modifier in class member declaration");
                }
                access = ACCESS_PROTECTED;
                saw_access_modifier = true;
                continue;
            }

            if (match(parser, TOKEN_STATIC)) {
                if (is_static) {
                    error_at(parser, &parser->previous,
                             "Duplicate 'static' in class member declaration");
                }
                is_static = true;
                continue;
            }

            if (match(parser, TOKEN_ABSTRACT)) {
                if (is_abstract_method) {
                    error_at(parser, &parser->previous,
                             "Duplicate 'abstract' in class member declaration");
                }
                is_abstract_method = true;
                continue;
            }

            if (match(parser, TOKEN_LET)) {
                if (has_binding_keyword) {
                    error_at(parser, &parser->previous,
                             "Duplicate field binding keyword in class member declaration");
                }
                binding_kind = CLASS_FIELD_BINDING_LET;
                has_binding_keyword = true;
                continue;
            }

            if (match(parser, TOKEN_MUT)) {
                if (has_binding_keyword) {
                    error_at(parser, &parser->previous,
                             "Duplicate field binding keyword in class member declaration");
                }
                binding_kind = CLASS_FIELD_BINDING_MUT;
                has_binding_keyword = true;
                continue;
            }

            if (match(parser, TOKEN_CONST)) {
                if (has_binding_keyword) {
                    error_at(parser, &parser->previous,
                             "Duplicate field binding keyword in class member declaration");
                }
                binding_kind = CLASS_FIELD_BINDING_CONST;
                has_binding_keyword = true;
                continue;
            }

            break;
        }

        if (check(parser, TOKEN_FUNC)) {
            if (has_binding_keyword) {
                error_at_current(parser,
                    "Use 'func' for methods and 'let', 'mut', or 'const' for fields");
            }
            advance(parser);

            // Method declaration
            // Allow TOKEN_IDENTIFIER or TOKEN_NEW for method names. TOKEN_PRINT
            // and TOKEN_UNION_KW are also accepted contextually: `print` is a
            // statement keyword and `union` is a top-level declaration keyword,
            // but both are unambiguous as a method name here (a class body after
            // `func` expects a name), and postfix member-access parsing already
            // treats `.print()` / `.union()` as ordinary calls.
            if (!match(parser, TOKEN_IDENTIFIER) &&
                !match(parser, TOKEN_NEW) &&
                !match(parser, TOKEN_PRINT) &&
                !match(parser, TOKEN_UNION_KW)) {
                error_at_current(parser, "Expected method name");
                continue;
            }

            if (method_count >= method_capacity) {
                method_capacity = GROW_CAPACITY(method_capacity);
                methods = GROW_ARRAY(MethodDecl, methods, method_count, method_capacity);
            }

            Token method_name_token = parser->previous;

            // For TOKEN_NEW, we need to manually create the name "new"
            if (method_name_token.type == TOKEN_NEW) {
                methods[method_count].name = strdup("new");
            } else {
                // copy_identifier lifts the lexeme text verbatim, so contextual
                // keywords (print, union) yield their own spelling as the name.
                methods[method_count].name = copy_identifier(&method_name_token);
            }

            methods[method_count].is_constructor =
                is_constructor_name(class_name, methods[method_count].name);

            consume(parser, TOKEN_LPAREN, "Expected '(' after method name");

            // Parse parameters
            int param_capacity = 8;
            int param_count = 0;
            Parameter* params = ALLOCATE(Parameter, param_capacity);

            if (!check(parser, TOKEN_RPAREN)) {
                do {
                    if (param_count >= param_capacity) {
                        param_capacity = GROW_CAPACITY(param_capacity);
                        params = GROW_ARRAY(Parameter, params, param_count, param_capacity);
                    }

                    consume(parser, TOKEN_IDENTIFIER, "Expected parameter name");
                    params[param_count].name = copy_identifier(&parser->previous);

                    consume(parser, TOKEN_COLON, "Expected ':' after parameter name");
                    char* param_class_name = NULL;
                    params[param_count].type = parse_type_with_class(parser, &param_class_name);
                    params[param_count].class_name = param_class_name;
                    params[param_count].type_info = take_pending_fn_type_info();
                    params[param_count].ownership = OWNERSHIP_OWNED;

                    param_count++;
                } while (match(parser, TOKEN_COMMA));
            }

            consume(parser, TOKEN_RPAREN, "Expected ')' after parameters");

            methods[method_count].return_type = TYPE_VOID;
            if (match(parser, TOKEN_ARROW)) {
                char* method_return_class = NULL;
                methods[method_count].return_type = parse_type_with_class(parser, &method_return_class);
                free(method_return_class);
            }
            methods[method_count].parameters = params;
            methods[method_count].param_count = param_count;
            methods[method_count].is_static = is_static;
            methods[method_count].access = access;
            methods[method_count].is_abstract = is_abstract_method;

            if (is_abstract_method) {
                /* Abstract methods have no body — just end with semicolon */
                methods[method_count].body = NULL;
                match(parser, TOKEN_SEMICOLON);
            } else {
                consume(parser, TOKEN_LBRACE, "Expected '{' before method body");
                methods[method_count].body = block_statement(parser);
            }

            method_count++;
        } else if (has_binding_keyword) {
            if (is_abstract_method) {
                error_at_current(parser, "'abstract' can only be used on methods");
            }

            consume(parser, TOKEN_IDENTIFIER, "Expected field name after field modifiers");
            Token name_token = parser->previous;
            consume(parser, TOKEN_COLON, "Expected ':' after field name");

            FieldDecl* field = append_class_field_slot(&fields, &field_count, &field_capacity);
            char* field_class_name = NULL;
            DataType field_type = parse_type_with_class(parser, &field_class_name);
            init_class_field_decl(field, &name_token, field_type, field_class_name,
                                  is_static, access, binding_kind);

            if (match(parser, TOKEN_ASSIGN)) {
                field->default_value = expression(parser);
            }

            match(parser, TOKEN_SEMICOLON);
        } else if (check(parser, TOKEN_IDENTIFIER) && token_text_equals(&parser->current, "field")) {
            if (is_abstract_method) {
                error_at_current(parser, "'abstract' can only be used on methods");
            }
            advance(parser);
            consume(parser, TOKEN_IDENTIFIER, "Expected field name after 'field'");
            Token name_token = parser->previous;

            consume(parser, TOKEN_COLON, "Expected ':' after field name");

            FieldDecl* field = append_class_field_slot(&fields, &field_count, &field_capacity);
            char* field_class_name = NULL;
            DataType field_type = parse_type_with_class(parser, &field_class_name);
            init_class_field_decl(field, &name_token, field_type, field_class_name,
                                  is_static, access, CLASS_FIELD_BINDING_LEGACY);

            if (match(parser, TOKEN_ASSIGN)) {
                field->default_value = expression(parser);
            }

            match(parser, TOKEN_SEMICOLON);
        } else if (match(parser, TOKEN_IDENTIFIER)) {
            if (is_abstract_method) {
                error_at_current(parser, "'abstract' can only be used on methods");
            }
            Token name_token = parser->previous;

            // Field declaration: name: Type;
            if (check(parser, TOKEN_COLON)) {
                consume(parser, TOKEN_COLON, "Expected ':' after field name");

                FieldDecl* field = append_class_field_slot(&fields, &field_count, &field_capacity);
                char* field_class_name = NULL;
                DataType field_type = parse_type_with_class(parser, &field_class_name);
                init_class_field_decl(field, &name_token, field_type, field_class_name,
                                      is_static, access, CLASS_FIELD_BINDING_LEGACY);

                // Optional default value
                if (match(parser, TOKEN_ASSIGN)) {
                    field->default_value = expression(parser);
                }

                // Trailing semicolon is optional, matching the 'field'-keyword
                // and modern 'let'/'mut'/'const' member paths above.
                match(parser, TOKEN_SEMICOLON);
            } else {
                error_at_current(parser, "Expected ':' after field name or 'func' for method");
            }
        } else {
            error_at_current(parser, "Expected field or method declaration in class body");
            advance(parser); // Skip the unexpected token
        }

        if (parser->panic_mode) {
            synchronize_block(parser);
        }
    }

    consume(parser, TOKEN_RBRACE, "Expected '}' after class body");

    Stmt* stmt = create_class_stmt(class_name, parent_name, fields, field_count,
                                   methods, method_count, line, col);
    // Add generic type parameters
    stmt->as.class_stmt.type_params = type_params;
    stmt->as.class_stmt.type_param_count = type_param_count;
    stmt->as.class_stmt.implements = implements;
    stmt->as.class_stmt.implements_count = implements_count;
    return stmt;
}

static Stmt* include_statement(Parser* parser) {
    int line = parser->previous.line;
    int col = parser->previous.column;
    bool is_import = parser->previous.type == TOKEN_IMPORT;
    
    // The current token should be the string
    if (parser->current.type != TOKEN_STRING) {
        if (parser->current.type == TOKEN_IDENTIFIER) {
            error_at_current(parser,
                "Import paths must be string literals, e.g. import \"module/path\";");
        } else {
            error_at_current(parser,
                "Expected module path string literal after import/include");
        }
        return NULL;
    }
    
    // Extract string - the lexer should have already extracted it correctly
    // Make a deep copy immediately before any token advancement
    char* module_name = NULL;
    
    // Primary method: Use lexer's string_value (already extracted without quotes)
    if (parser->current.literal.string_value) {
        const char* str_val = parser->current.literal.string_value;
        size_t len = strlen(str_val);
        // Only use if length is reasonable and first char is printable ASCII
        if (len > 0 && len < 512 && str_val[0] >= 32 && str_val[0] < 127) {
            module_name = ALLOCATE(char, len + 1);
            if (module_name) {
                strcpy(module_name, str_val);
            }
        }
    }
    
    // Fallback: Extract from token start pointer (points into source)
    if (!module_name && parser->current.start && parser->current.length >= 3) {
        const char* start_ptr = parser->current.start;
        int total_len = parser->current.length;
        
        // Verify it looks like a valid string token
        if (start_ptr && start_ptr[0] == '"' && start_ptr[total_len - 1] == '"') {
            int content_len = total_len - 2;
            if (content_len > 0 && content_len < 512) {
                module_name = ALLOCATE(char, content_len + 1);
                if (module_name) {
                    memcpy(module_name, start_ptr + 1, content_len);
                    module_name[content_len] = '\0';
                }
            }
        }
    }
    
    if (!module_name) {
        error_at_current(parser, "Failed to extract module path from string token");
        return NULL;
    }
    
    advance(parser); // Now consume the string token

    // Keep the full path for module resolution, just remove extension
    // "lib/skia/ui.cpx" -> "lib/skia/ui"
    // "lib/skia/ui.nd"  -> "lib/skia/ui" (backward compat)
    // "lib/skia/ui"     -> "lib/skia/ui" (unchanged)
    char* dot = strrchr(module_name, '.');
    if (dot && (strcmp(dot, ".cpx") == 0 || strcmp(dot, ".nd") == 0)) {
        *dot = '\0';
    }

    // Semicolons are optional
    match(parser, TOKEN_SEMICOLON);
    return create_include_stmt(module_name, is_import, line, col);
}

// Forward declarations for struct/enum/union parsing
static Stmt* struct_declaration(Parser* parser);
static Stmt* enum_declaration(Parser* parser);
static Stmt* union_declaration(Parser* parser);
static Stmt* let_declaration(Parser* parser);
static Stmt* mut_declaration(Parser* parser);
static Stmt* const_declaration(Parser* parser);
static Stmt* impl_declaration(Parser* parser);

static Stmt* declaration(Parser* parser) {
    /* Abstract keyword can precede Class */
    bool is_abstract_class = false;
    if (check(parser, TOKEN_ABSTRACT)) {
        advance(parser);
        is_abstract_class = true;
    }

    bool is_async = false;
    if (match(parser, TOKEN_ASYNC)) {
        is_async = true;
    }

    if (check(parser, TOKEN_IMPLEMENTS)) {
        Token token = parser->current;
        report_unsupported_surface(parser, token,
            "'implements' is only accepted in class declarations; use 'impl Trait for Type' or ':' constraints");
        return NULL;
    }

    if (match(parser, TOKEN_CLASS)) {
        Stmt* s = class_declaration(parser);
        if (s && is_abstract_class)
            s->as.class_stmt.is_abstract = true;
        return s;
    }

    if (is_abstract_class) {
        error_at_current(parser, "'Abstract' can only precede 'Class'");
    }

    if (match(parser, TOKEN_FUNC)) {
        Stmt* s = function_declaration(parser);
        if (s) s->as.function.is_async = is_async;
        return s;
    }

    if (match(parser, TOKEN_INCLUDE) || match(parser, TOKEN_IMPORT)) {
        return include_statement(parser);
    }

    if (match(parser, TOKEN_EXTERN)) {
        return extern_declaration(parser);
    }

    // Struct declaration: struct Name { ... }
    if (match(parser, TOKEN_STRUCT)) {
        return struct_declaration(parser);
    }

    // Enum declaration: enum Name { ... }
    if (match(parser, TOKEN_ENUM_KW)) {
        return enum_declaration(parser);
    }

    // Union declaration: union Name { ... }
    if (match(parser, TOKEN_UNION_KW)) {
        return union_declaration(parser);
    }

    // Trait declaration: trait Name { func sig1(); ... }
    if (match(parser, TOKEN_TRAIT)) {
        return trait_statement(parser);
    }

    // Let declaration: let name: type = value  or  let name = value
    if (match(parser, TOKEN_LET)) {
        return let_declaration(parser);
    }

    // Const declaration: const name: type = value  or  const name = value
    if (match(parser, TOKEN_CONST)) {
        return const_declaration(parser);
    }

    // Mutable declaration: mut name: type = value
    if (match(parser, TOKEN_MUT)) {
        return mut_declaration(parser);
    }

    // Impl block: impl TypeName { func ... }  or  impl Trait for TypeName { ... }
    if (match(parser, TOKEN_IMPL)) {
        return impl_declaration(parser);
    }

    if (check(parser, TOKEN_SPAWN)) {
        Token token = parser->current;
        report_unsupported_surface(parser, token,
            "'spawn' blocks are not implemented yet");
        return NULL;
    }

    // Explicit allocation region: alloc { ... }
    if (check(parser, TOKEN_IDENTIFIER) && token_text_equals(&parser->current, "alloc")) {
        Token next = peek_next_token(parser);
        if (next.type == TOKEN_LBRACE) {
            int line = parser->current.line;
            int col = parser->current.column;
            advance(parser); /* consume alloc */
            consume(parser, TOKEN_LBRACE, "Expected '{' after 'alloc'");
            {
                Stmt* stmt = block_statement(parser);
                if (stmt && stmt->type == STMT_BLOCK) {
                    stmt->line = line;
                    stmt->column = col;
                    stmt->as.block.is_alloc_scope = true;
                }
                return stmt;
            }
        }
    }

    // Check for variable declaration (name: Type ...)
    // We only try to consume if we're sure it's a declaration
    if (check(parser, TOKEN_IDENTIFIER)) {
        // We need to peek two tokens ahead to distinguish:
        // - "name: Type" (declaration)
        // - "name <- value" or "name.member" or "name()" (expression statements)

        // Since we can't easily lookahead 2 tokens without consuming,
        // let's just check if the pattern looks like a declaration
        // by advancing and checking for COLON

        Token id_token = parser->current;
        advance(parser); // consume identifier

        if (check(parser, TOKEN_COLON)) {
            // This is a variable declaration - var_declaration expects identifier in previous
            return var_declaration(parser);
        }

        // Check for := (type inference declaration): x := expr
        if (check(parser, TOKEN_INFER_ASSIGN)) {
            advance(parser); // consume :=
            char* name = copy_identifier(&id_token);
            Expr* initializer = expression(parser);
            // Semicolons are optional
            match(parser, TOKEN_SEMICOLON);
            // Create declaration with TYPE_ERROR to signal type inference needed
            Stmt* stmt = create_declaration_stmt(name, TYPE_ERROR, initializer,
                                                 id_token.line, id_token.column);
            stmt->as.declaration.class_name = NULL;
            return stmt;
        }

        // Not a declaration - we consumed the identifier, so we need to handle this
        // as an expression/assignment statement. Parse the rest as expression.
        // Put the identifier into an expression and continue from there.

        char* var_name = copy_identifier(&id_token);
        Expr* expr = create_variable_expr(var_name, id_token.line, id_token.column);

        // Now continue parsing the rest as postfix operations (., [])
        expr = postfix_with_expr(parser, expr);

        // Check if this is an assignment (simple or compound)
        TokenType assign_op = parser->current.type;
        if (assign_op == TOKEN_ASSIGN ||
            assign_op == TOKEN_PLUS_ASSIGN  || assign_op == TOKEN_MINUS_ASSIGN ||
            assign_op == TOKEN_STAR_ASSIGN  || assign_op == TOKEN_SLASH_ASSIGN ||
            assign_op == TOKEN_PERCENT_ASSIGN) {

            advance(parser);  // consume the assignment token
            int line = parser->previous.line;
            int col = parser->previous.column;

            if (expr->type != EXPR_VARIABLE && expr->type != EXPR_MEMBER_ACCESS &&
                expr->type != EXPR_INDEX) {
                error_at_current(parser, "Invalid assignment target");
            }

            Expr* value = expression(parser);

            // Desugar compound assignment: x += y  =>  x = x + y
            if (assign_op != TOKEN_ASSIGN) {
                TokenType bin_op;
                switch (assign_op) {
                    case TOKEN_PLUS_ASSIGN:    bin_op = TOKEN_PLUS;    break;
                    case TOKEN_MINUS_ASSIGN:   bin_op = TOKEN_MINUS;   break;
                    case TOKEN_STAR_ASSIGN:    bin_op = TOKEN_STAR;    break;
                    case TOKEN_SLASH_ASSIGN:   bin_op = TOKEN_SLASH;   break;
                    case TOKEN_PERCENT_ASSIGN: bin_op = TOKEN_PERCENT; break;
                    default: bin_op = TOKEN_PLUS; break;
                }
                // Clone the target expression for the right-hand side
                Expr* lhs_copy = ALLOCATE(Expr, 1);
                memcpy(lhs_copy, expr, sizeof(Expr));
                value = create_binary_expr(lhs_copy, bin_op, value, line, col);
            }

            // Semicolons are optional
            match(parser, TOKEN_SEMICOLON);
            return create_assignment_stmt(expr, value, line, col);
        }

        // Just an expression statement
        // Semicolons are optional
        match(parser, TOKEN_SEMICOLON);
        return create_expr_stmt(expr, expr->line, expr->column);
    }

    return statement(parser);
}

static Stmt* extern_declaration(Parser* parser) {
    int line = parser->previous.line;
    int col = parser->previous.column;

    consume(parser, TOKEN_FUNC, "Expected 'func' after 'extern'");
    consume(parser, TOKEN_IDENTIFIER, "Expected function name after 'extern func'");
    char* name = copy_identifier(&parser->previous);

    consume(parser, TOKEN_LPAREN, "Expected '(' after function name");

    // Parameters
    int param_capacity = 8;
    int param_count = 0;
    Parameter* params = ALLOCATE(Parameter, param_capacity);

    if (!check(parser, TOKEN_RPAREN)) {
        do {
            if (param_count >= param_capacity) {
                param_capacity = GROW_CAPACITY(param_capacity);
                params = GROW_ARRAY(Parameter, params, param_count, param_capacity);
            }

            consume(parser, TOKEN_IDENTIFIER, "Expected parameter name");
            params[param_count].name = copy_identifier(&parser->previous);

            consume(parser, TOKEN_COLON, "Expected ':' after parameter name");
            char* param_class_name = NULL;
            params[param_count].type = parse_type_with_class(parser, &param_class_name);
            params[param_count].class_name = param_class_name;
            /* Retain a `lambda(...) -> R` signature so calls through this
             * parameter can be arity-checked (see semantic.c). */
            params[param_count].type_info = take_pending_fn_type_info();
            params[param_count].ownership = OWNERSHIP_OWNED;

            param_count++;
        } while (match(parser, TOKEN_COMMA));
    }

    consume(parser, TOKEN_RPAREN, "Expected ')' after parameters");
    consume(parser, TOKEN_ARROW, "Expected '->' after parameters");

    char* ret_class_name = NULL;
    DataType return_type = parse_type_with_class(parser, &ret_class_name);

    // Semicolons are now optional
    match(parser, TOKEN_SEMICOLON);

    return create_extern_stmt(name, params, param_count, return_type, ret_class_name, line, col);
}

// ============================================================
// trait declaration: trait Name { func sig1() -> T; ... }
// ============================================================
static Stmt* trait_statement(Parser* parser) {
    int line = parser->previous.line;
    int col  = parser->previous.column;

    consume(parser, TOKEN_IDENTIFIER, "Expected trait name after 'trait'");
    char* name = copy_identifier(&parser->previous);

    /* Optional generic type params  <T, K: Comparable> */
    TypeParam* type_params = NULL;
    int type_param_count = 0;
    if (check(parser, TOKEN_LESS)) {
        advance(parser);
        type_params = parse_type_params(parser, &type_param_count);
    }

    consume(parser, TOKEN_LBRACE, "Expected '{' after trait name");

    /* Parse method signatures */
    int cap = 8, cnt = 0;
    TraitMethodDecl* methods = ALLOCATE(TraitMethodDecl, cap);

    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        /* Optional 'func' keyword */
        match(parser, TOKEN_FUNC);

        consume(parser, TOKEN_IDENTIFIER, "Expected method name in trait");
        char* mname = copy_identifier(&parser->previous);

        /* Parameters */
        consume(parser, TOKEN_LPAREN, "Expected '(' after method name");
        int pcap = 4, pcnt = 0;
        Parameter* params = ALLOCATE(Parameter, pcap);
        if (!check(parser, TOKEN_RPAREN)) {
            do {
                if (pcnt >= pcap) {
                    pcap = GROW_CAPACITY(pcap);
                    params = GROW_ARRAY(Parameter, params, pcnt, pcap);
                }
                consume(parser, TOKEN_IDENTIFIER, "Expected parameter name");
                params[pcnt].name = copy_identifier(&parser->previous);
                params[pcnt].type = TYPE_VOID;
                params[pcnt].class_name = NULL;
                params[pcnt].type_info = NULL;
                params[pcnt].ownership = OWNERSHIP_OWNED;
                if (match(parser, TOKEN_COLON)) {
                    params[pcnt].type = parse_type(parser);
                    params[pcnt].type_info = take_pending_fn_type_info();
                }
                pcnt++;
            } while (match(parser, TOKEN_COMMA));
        }
        consume(parser, TOKEN_RPAREN, "Expected ')' after parameters");

        /* Return type */
        DataType ret = TYPE_VOID;
        char* ret_class = NULL;
        if (match(parser, TOKEN_ARROW)) {
            ret = parse_type_with_class(parser, &ret_class);
        }

        /* Optional default body or just semicolon */
        Stmt* default_body = NULL;
        bool has_default = false;
        if (match(parser, TOKEN_SEMICOLON)) {
            /* abstract method — no body */
        } else if (match(parser, TOKEN_LBRACE)) {
            has_default = true;
            default_body = block_statement(parser);
        } else {
            match(parser, TOKEN_SEMICOLON);  /* tolerate missing semicolons */
        }

        if (cnt >= cap) {
            cap = GROW_CAPACITY(cap);
            methods = GROW_ARRAY(TraitMethodDecl, methods, cnt, cap);
        }
        methods[cnt].name           = mname;
        methods[cnt].parameters     = params;
        methods[cnt].param_count    = pcnt;
        methods[cnt].return_type    = ret;
        methods[cnt].return_class_name = ret_class;
        methods[cnt].has_default    = has_default;
        methods[cnt].default_body   = default_body;
        cnt++;

        if (parser->panic_mode) {
            synchronize_block(parser);
        }
    }

    consume(parser, TOKEN_RBRACE, "Expected '}' after trait body");

    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type   = STMT_TRAIT;
    stmt->line   = line;
    stmt->column = col;
    stmt->as.trait_stmt.name            = name;
    stmt->as.trait_stmt.methods         = methods;
    stmt->as.trait_stmt.method_count    = cnt;
    stmt->as.trait_stmt.type_params     = type_params;
    stmt->as.trait_stmt.type_param_count = type_param_count;
    stmt->as.trait_stmt.super_traits    = NULL;
    stmt->as.trait_stmt.super_count     = 0;
    return stmt;
}

// Parse struct declaration: struct Name { field: type, ... }
static Stmt* struct_declaration(Parser* parser) {
    int line = parser->previous.line;
    int col = parser->previous.column;

    consume(parser, TOKEN_IDENTIFIER, "Expected struct name after 'struct'");
    char* name = copy_identifier(&parser->previous);

    consume(parser, TOKEN_LBRACE, "Expected '{' after struct name");

    int field_capacity = 8;
    int field_count = 0;
    StructField* fields = ALLOCATE(StructField, field_capacity);

    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (field_count >= field_capacity) {
            field_capacity = GROW_CAPACITY(field_capacity);
            fields = GROW_ARRAY(StructField, fields, field_count, field_capacity);
        }

        consume(parser, TOKEN_IDENTIFIER, "Expected field name");
        fields[field_count].name = copy_identifier(&parser->previous);

        consume(parser, TOKEN_COLON, "Expected ':' after field name");
        char* field_class = NULL;
        fields[field_count].type = parse_type_with_class(parser, &field_class);
        fields[field_count].class_name = field_class;
        fields[field_count].type_info = NULL;

        field_count++;

        // Allow comma or newline between fields
        match(parser, TOKEN_COMMA);
        match(parser, TOKEN_SEMICOLON);

        if (parser->panic_mode) {
            synchronize_block(parser);
        }
    }

    consume(parser, TOKEN_RBRACE, "Expected '}' after struct body");

    return create_struct_stmt(name, fields, field_count, line, col);
}

// Parse enum declaration: enum Name { Variant1, Variant2(type), ... }
static Stmt* enum_declaration(Parser* parser) {
    int line = parser->previous.line;
    int col = parser->previous.column;

    consume(parser, TOKEN_IDENTIFIER, "Expected enum name after 'enum'");
    char* name = copy_identifier(&parser->previous);

    consume(parser, TOKEN_LBRACE, "Expected '{' after enum name");

    int variant_capacity = 8;
    int variant_count = 0;
    EnumVariant* variants = ALLOCATE(EnumVariant, variant_capacity);

    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (variant_count >= variant_capacity) {
            variant_capacity = GROW_CAPACITY(variant_capacity);
            variants = GROW_ARRAY(EnumVariant, variants, variant_count, variant_capacity);
        }

        consume(parser, TOKEN_IDENTIFIER, "Expected variant name");
        variants[variant_count].name = copy_identifier(&parser->previous);
        variants[variant_count].payload_types = NULL;
        variants[variant_count].payload_names = NULL;
        variants[variant_count].payload_count = 0;
        variants[variant_count].tag_value = -1;  // auto-assign

        // Check for payload: Variant(type1, type2, ...)
        if (match(parser, TOKEN_LPAREN)) {
            int payload_capacity = 4;
            int payload_count = 0;
            DataType* payload_types = ALLOCATE(DataType, payload_capacity);
            char** payload_names = ALLOCATE(char*, payload_capacity);

            do {
                if (payload_count >= payload_capacity) {
                    payload_capacity = GROW_CAPACITY(payload_capacity);
                    payload_types = GROW_ARRAY(DataType, payload_types, payload_count, payload_capacity);
                    payload_names = GROW_ARRAY(char*, payload_names, payload_count, payload_capacity);
                }

                char* pclass = NULL;
                payload_types[payload_count] = parse_type_with_class(parser, &pclass);
                payload_names[payload_count] = pclass;  // Use class name as field name or NULL
                payload_count++;
            } while (match(parser, TOKEN_COMMA));

            consume(parser, TOKEN_RPAREN, "Expected ')' after variant payload");

            variants[variant_count].payload_types = payload_types;
            variants[variant_count].payload_names = payload_names;
            variants[variant_count].payload_count = payload_count;
        }

        variant_count++;

        // Allow comma or newline between variants
        match(parser, TOKEN_COMMA);
        match(parser, TOKEN_SEMICOLON);

        if (parser->panic_mode) {
            synchronize_block(parser);
        }
    }

    consume(parser, TOKEN_RBRACE, "Expected '}' after enum body");

    return create_enum_stmt(name, variants, variant_count, line, col);
}

// Parse union declaration: union Name { field: type, ... }
static Stmt* union_declaration(Parser* parser) {
    int line = parser->previous.line;
    int col = parser->previous.column;

    consume(parser, TOKEN_IDENTIFIER, "Expected union name after 'union'");
    char* name = copy_identifier(&parser->previous);

    consume(parser, TOKEN_LBRACE, "Expected '{' after union name");

    int field_capacity = 8;
    int field_count = 0;
    UnionField* fields = ALLOCATE(UnionField, field_capacity);

    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (field_count >= field_capacity) {
            field_capacity = GROW_CAPACITY(field_capacity);
            fields = GROW_ARRAY(UnionField, fields, field_count, field_capacity);
        }

        consume(parser, TOKEN_IDENTIFIER, "Expected field name");
        fields[field_count].name = copy_identifier(&parser->previous);

        consume(parser, TOKEN_COLON, "Expected ':' after field name");
        fields[field_count].type = parse_type(parser);
        fields[field_count].type_info = NULL;

        field_count++;

        match(parser, TOKEN_COMMA);
        match(parser, TOKEN_SEMICOLON);

        if (parser->panic_mode) {
            synchronize_block(parser);
        }
    }

    consume(parser, TOKEN_RBRACE, "Expected '}' after union body");

    return create_union_stmt(name, fields, field_count, line, col);
}

// Parse let declaration: let name: type = value  or  let name = value (inferred)
static Stmt* const_declaration(Parser* parser) {
    int line = parser->previous.line;
    int col = parser->previous.column;

    consume(parser, TOKEN_IDENTIFIER, "Expected constant name after 'const'");
    Token id_token = parser->previous;
    char* name = copy_identifier(&id_token);

    DataType type = TYPE_ERROR;
    char* class_name = NULL;

    if (match(parser, TOKEN_COLON)) {
        // Explicit type: const NAME: Type = value
        type = parse_type_with_class(parser, &class_name);
    }

    // const declarations MUST have an initializer
    consume(parser, TOKEN_ASSIGN, "Expected '=' after const declaration (constants require an initializer)");
    Expr* initializer = expression(parser);

    match(parser, TOKEN_SEMICOLON);

    Stmt* stmt = create_declaration_stmt(name, type, initializer, line, col);
    stmt->type = STMT_CONST_DECL;
    stmt->as.declaration.class_name = class_name;
    stmt->as.declaration.is_const = true;
    stmt->as.declaration.is_mutable = false;
    return stmt;
}

static Stmt* impl_declaration(Parser* parser) {
    int line = parser->previous.line;
    int col = parser->previous.column;

    consume(parser, TOKEN_IDENTIFIER, "Expected type name after 'impl'");
    char* target_name = copy_identifier(&parser->previous);

    // Optional generic type params: impl<T> List<T> { ... }
    TypeParam* type_params = NULL;
    int type_param_count = 0;
    if (match(parser, TOKEN_LESS)) {
        type_params = parse_type_params(parser, &type_param_count);
    }

    // Optional trait implementation: impl Printable for MyType { ... }
    char* trait_name = NULL;
    if (match(parser, TOKEN_FOR)) {
        // "impl TraitName for TypeName" — swap the names
        trait_name = target_name;
        consume(parser, TOKEN_IDENTIFIER, "Expected type name after 'for'");
        target_name = copy_identifier(&parser->previous);
    }

    consume(parser, TOKEN_LBRACE, "Expected '{' after impl target");

    // Parse method definitions (full function bodies)
    int method_capacity = 8;
    int method_count = 0;
    Stmt** methods = ALLOCATE(Stmt*, method_capacity);

    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (method_count >= method_capacity) {
            method_capacity = GROW_CAPACITY(method_capacity);
            methods = GROW_ARRAY(Stmt*, methods, method_count, method_capacity);
        }

        // Each item inside impl must be a function
        consume(parser, TOKEN_FUNC, "Expected 'func' in impl block");
        methods[method_count] = function_declaration(parser);
        method_count++;

        if (parser->panic_mode) {
            synchronize_block(parser);
        }
    }

    consume(parser, TOKEN_RBRACE, "Expected '}' after impl body");

    Stmt* stmt = ALLOCATE(Stmt, 1);
    stmt->type = STMT_IMPL;
    stmt->line = line;
    stmt->column = col;
    stmt->as.impl_stmt.target_name = target_name;
    stmt->as.impl_stmt.trait_name = trait_name;
    stmt->as.impl_stmt.methods = methods;
    stmt->as.impl_stmt.method_count = method_count;
    stmt->as.impl_stmt.type_params = type_params;
    stmt->as.impl_stmt.type_param_count = type_param_count;
    return stmt;
}

static Stmt* let_declaration(Parser* parser) {
    int line = parser->previous.line;
    int col = parser->previous.column;

    consume(parser, TOKEN_IDENTIFIER, "Expected variable name after 'let'");
    Token id_token = parser->previous;
    char* name = copy_identifier(&id_token);

    DataType type = TYPE_ERROR;
    char* class_name = NULL;

    if (match(parser, TOKEN_COLON)) {
        // Explicit type: let name: type = value
        type = parse_type_with_class(parser, &class_name);
    }

    Expr* initializer = NULL;
    if (match(parser, TOKEN_ASSIGN)) {
        initializer = expression(parser);
    } else if (type == TYPE_ERROR) {
        error_at_current(parser, "Type inference requires an initializer: let name = value");
    }

    match(parser, TOKEN_SEMICOLON);

    Stmt* stmt = create_declaration_stmt(name, type, initializer, line, col);
    stmt->as.declaration.class_name = class_name;
    stmt->as.declaration.is_mutable = false;
    return stmt;
}

static Stmt* mut_declaration(Parser* parser) {
    int line = parser->previous.line;
    int col = parser->previous.column;

    consume(parser, TOKEN_IDENTIFIER, "Expected variable name after 'mut'");
    {
        Token id_token = parser->previous;
        char* name = copy_identifier(&id_token);
        char* class_name = NULL;
        Expr* initializer = NULL;
        DataType type;

        consume(parser, TOKEN_COLON, "Expected ':' after mutable variable name");
        type = parse_type_with_class(parser, &class_name);

        if (match(parser, TOKEN_ASSIGN)) {
            initializer = expression(parser);
        }

        match(parser, TOKEN_SEMICOLON);

        {
            Stmt* stmt = create_declaration_stmt(name, type, initializer, line, col);
            stmt->as.declaration.class_name = class_name;
            stmt->as.declaration.is_mutable = true;
            return stmt;
        }
    }
}

Stmt** parse(Parser* parser, int* stmt_count) {
    int capacity = 16;
    int count = 0;
    Stmt** statements = ALLOCATE(Stmt*, capacity);
    
    while (!match(parser, TOKEN_EOF)) {
        if (count >= capacity) {
            capacity = GROW_CAPACITY(capacity);
            statements = GROW_ARRAY(Stmt*, statements, count, capacity);
        }
        
        statements[count++] = declaration(parser);
        
        if (parser->panic_mode) {
            synchronize(parser);
        }
    }
    
    *stmt_count = count;
    return statements;
}
