#include "compiler/frontend/lexer.h"
#include "support/error.h"

void init_lexer(Lexer* lexer, const char* source) {
    // Skip UTF-8 BOM if present
    if ((unsigned char)source[0] == 0xEF &&
        (unsigned char)source[1] == 0xBB &&
        (unsigned char)source[2] == 0xBF) {
        source += 3;
    }
    lexer->start = source;
    lexer->current = source;
    lexer->line = 1;
    lexer->column = 1;
    lexer->start_column = 1;
}

static bool is_at_end(Lexer* lexer) {
    return *lexer->current == '\0';
}

static char advance(Lexer* lexer) {
    lexer->current++;
    lexer->column++;
    return lexer->current[-1];
}

static char peek(Lexer* lexer) {
    return *lexer->current;
}

static char peek_next(Lexer* lexer) {
    if (is_at_end(lexer)) return '\0';
    return lexer->current[1];
}

static bool match(Lexer* lexer, char expected) {
    if (is_at_end(lexer)) return false;
    if (*lexer->current != expected) return false;
    
    lexer->current++;
    lexer->column++;
    return true;
}

static Token make_token(Lexer* lexer, TokenType type) {
    Token token;
    token.type = type;
    token.start = lexer->start;
    token.length = (int)(lexer->current - lexer->start);
    token.line = lexer->line;
    token.column = lexer->start_column;
    return token;
}

static Token error_token(Lexer* lexer, const char* message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.start = message;
    token.length = (int)strlen(message);
    token.line = lexer->line;
    token.column = lexer->start_column;
    return token;
}

static void skip_whitespace(Lexer* lexer) {
    for (;;) {
        char c = peek(lexer);
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
                advance(lexer);
                break;
            case '\n':
                lexer->line++;
                lexer->column = 0;
                advance(lexer);
                break;
            case '/':
                if (peek_next(lexer) == '/') {
                    // Single-line comment
                    while (peek(lexer) != '\n' && !is_at_end(lexer)) {
                        advance(lexer);
                    }
                } else if (peek_next(lexer) == '*') {
                    // Multi-line comment
                    advance(lexer); // consume /
                    advance(lexer); // consume *
                    
                    while (!is_at_end(lexer)) {
                        if (peek(lexer) == '*' && peek_next(lexer) == '/') {
                            advance(lexer); // consume *
                            advance(lexer); // consume /
                            break;
                        }
                        if (peek(lexer) == '\n') {
                            lexer->line++;
                            lexer->column = 0;
                        }
                        advance(lexer);
                    }
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

static Token string(Lexer* lexer) {
    // lexer->start points to the opening quote (set by scan_token before calling this)
    // lexer->current points to the character after the opening quote (advance consumed it)
    
    // Scan until closing quote
    while (peek(lexer) != '"' && !is_at_end(lexer)) {
        if (peek(lexer) == '\\') {
            advance(lexer); // Skip backslash
            if (!is_at_end(lexer)) {
                advance(lexer); // Skip escaped character
            }
        } else {
            if (peek(lexer) == '\n') lexer->line++;
            advance(lexer);
        }
    }
    
    if (is_at_end(lexer)) {
        return error_token(lexer, "Unterminated string");
    }
    
    // Consume closing quote
    advance(lexer);
    
    // Create token - start points to opening quote, length includes both quotes
    Token token = make_token(lexer, TOKEN_STRING);
    
    // Extract and process string content (between quotes) with escape sequences
    int raw_len = token.length - 2; // Remove both quotes
    char* raw_str = ALLOCATE(char, raw_len + 1);
    memcpy(raw_str, token.start + 1, raw_len);
    raw_str[raw_len] = '\0';
    
    // Process escape sequences
    char* processed = ALLOCATE(char, raw_len + 1); // Max size, will be <= raw_len
    int write_idx = 0;
    
    for (int read_idx = 0; read_idx < raw_len; read_idx++) {
        if (raw_str[read_idx] == '\\' && read_idx + 1 < raw_len) {
            read_idx++; // Move to character after backslash
            switch (raw_str[read_idx]) {
                case 'n':  processed[write_idx++] = '\n'; break;
                case 't':  processed[write_idx++] = '\t'; break;
                case 'r':  processed[write_idx++] = '\r'; break;
                case '\\': processed[write_idx++] = '\\'; break;
                case '"':  processed[write_idx++] = '"';  break;
                case '0':  processed[write_idx++] = '\0'; break;
                default:
                    // Unknown escape sequence, keep as-is
                    processed[write_idx++] = '\\';
                    processed[write_idx++] = raw_str[read_idx];
                    break;
            }
        } else {
            processed[write_idx++] = raw_str[read_idx];
        }
    }
    processed[write_idx] = '\0';
    
    // Store processed string
    token.literal.string_value = ALLOCATE(char, write_idx + 1);
    if (token.literal.string_value) {
        memcpy(token.literal.string_value, processed, write_idx + 1);
    }
    
    free(raw_str);
    free(processed);
    
    return token;
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int is_bin_digit(char c) {
    return c == '0' || c == '1';
}

static Token number(Lexer* lexer) {
    // Check for hex literal: 0x... or 0X...
    // At this point, the first digit has already been consumed by advance() in scan_token
    // So lexer->start points to that first digit, and lexer->current is one past it
    char first_char = lexer->start[0];
    if (first_char == '0' && (peek(lexer) == 'x' || peek(lexer) == 'X')) {
        advance(lexer); // consume 'x'
        while (is_hex_digit(peek(lexer))) {
            advance(lexer);
        }

        Token token = make_token(lexer, TOKEN_INTEGER);
        char* str = ALLOCATE(char, token.length + 1);
        memcpy(str, token.start, token.length);
        str[token.length] = '\0';
        token.literal.int_value = (int64_t)strtoull(str, NULL, 16);
        free(str);
        return token;
    }

    // Check for binary literal: 0b... or 0B...
    // Mirrors the hex case above; strtoull does not recognize a "0b" prefix
    // so we parse from str + 2 with an explicit base of 2.
    if (first_char == '0' && (peek(lexer) == 'b' || peek(lexer) == 'B')) {
        advance(lexer); // consume 'b'
        while (is_bin_digit(peek(lexer))) {
            advance(lexer);
        }

        Token token = make_token(lexer, TOKEN_INTEGER);
        char* str = ALLOCATE(char, token.length + 1);
        memcpy(str, token.start, token.length);
        str[token.length] = '\0';
        token.literal.int_value = (int64_t)strtoull(str + 2, NULL, 2);
        free(str);
        return token;
    }

    while (is_digit(peek(lexer))) {
        advance(lexer);
    }

    // Look for decimal part
    if (peek(lexer) == '.' && is_digit(peek_next(lexer))) {
        advance(lexer); // consume .

        while (is_digit(peek(lexer))) {
            advance(lexer);
        }

        Token token = make_token(lexer, TOKEN_FLOAT);

        // Parse float value
        char* str = ALLOCATE(char, token.length + 1);
        memcpy(str, token.start, token.length);
        str[token.length] = '\0';
        token.literal.float_value = atof(str);
        free(str);

        return token;
    } else {
        Token token = make_token(lexer, TOKEN_INTEGER);

        // Parse integer value
        char* str = ALLOCATE(char, token.length + 1);
        memcpy(str, token.start, token.length);
        str[token.length] = '\0';
        token.literal.int_value = atoll(str);
        free(str);

        return token;
    }
}

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_';
}

static TokenType check_keyword(const char* start, int length,
                               const char* rest, TokenType type) {
    int rest_len = strlen(rest);
    if (length == rest_len && memcmp(start, rest, rest_len) == 0) {
        return type;
    }
    return TOKEN_IDENTIFIER;
}

static TokenType identifier_type(Lexer* lexer) {
    // Keyword recognition - all lowercase modern syntax
    // Includes full numeric type system (i8-i128, u8-u128, f16-f64, bf16)
    // and compound type keywords (struct, enum, union, array, slice, tensor, ptr, ref, etc.)
    int length = (int)(lexer->current - lexer->start);
    const char* start = lexer->start;

    switch (start[0]) {
        case 'a':
            if (length == 8) return check_keyword(start, length, "abstract", TOKEN_ABSTRACT);
            if (length == 5) {
                if (memcmp(start, "array", 5) == 0) return TOKEN_ARRAY_TYPE;
                if (memcmp(start, "async", 5) == 0) return TOKEN_ASYNC;
                if (memcmp(start, "await", 5) == 0) return TOKEN_AWAIT;
            }
            break;
        case 'b':
            if (length == 4) {
                if (memcmp(start, "bool", 4) == 0) return TOKEN_BOOL_TYPE;
                if (memcmp(start, "bf16", 4) == 0) return TOKEN_BF16_TYPE;
            }
            if (length == 5) return check_keyword(start, length, "break", TOKEN_BREAK);
            break;
        case 'c':
            if (length == 4) {
                if (memcmp(start, "char", 4) == 0) return TOKEN_CHAR_TYPE;
                if (memcmp(start, "copy", 4) == 0) return TOKEN_COPY;
            }
            if (length == 5) {
                if (memcmp(start, "class", 5) == 0) return TOKEN_CLASS;
                if (memcmp(start, "catch", 5) == 0) return TOKEN_CATCH;
                if (memcmp(start, "const", 5) == 0) return TOKEN_CONST;
            }
            if (length == 8) return check_keyword(start, length, "continue", TOKEN_CONTINUE);
            break;
        case 'e':
            if (length == 4) {
                if (memcmp(start, "else", 4) == 0) return TOKEN_ELSE;
                if (memcmp(start, "elif", 4) == 0) return TOKEN_ELIF;
                if (memcmp(start, "enum", 4) == 0) return TOKEN_ENUM_KW;
            }
            if (length == 7) return check_keyword(start, length, "extends", TOKEN_EXTENDS);
            if (length == 6) return check_keyword(start, length, "extern", TOKEN_EXTERN);
            break;
        case 'f':
            if (length == 2) {
                // f16, f32, f64 start with 'f' but are length > 2
                break;  // fall through to default
            }
            if (length == 3) {
                if (memcmp(start, "for", 3) == 0) return TOKEN_FOR;
                if (memcmp(start, "f16", 3) == 0) return TOKEN_F16_TYPE;
                if (memcmp(start, "f32", 3) == 0) return TOKEN_F32_TYPE;
                if (memcmp(start, "f64", 3) == 0) return TOKEN_F64_TYPE;
            }
            if (length == 4) return check_keyword(start, length, "func", TOKEN_FUNC);
            if (length == 5) {
                if (memcmp(start, "float", 5) == 0) return TOKEN_FLOAT_TYPE;
                if (memcmp(start, "false", 5) == 0) return TOKEN_FALSE;
            }
            if (length == 7) return check_keyword(start, length, "finally", TOKEN_FINALLY);
            break;
        case 'i':
            if (length == 2) {
                if (start[1] == 'f') return TOKEN_IF;
                if (start[1] == '8') return TOKEN_I8_TYPE;
                if (start[1] == 'n') return TOKEN_IN;
            }
            if (length == 3) {
                if (memcmp(start, "int", 3) == 0) return TOKEN_INT_TYPE;
                if (memcmp(start, "i16", 3) == 0) return TOKEN_I16_TYPE;
                if (memcmp(start, "i32", 3) == 0) return TOKEN_I32_TYPE;
                if (memcmp(start, "i64", 3) == 0) return TOKEN_I64_TYPE;
            }
            if (length == 4) {
                if (memcmp(start, "i128", 4) == 0) return TOKEN_I128_TYPE;
                if (memcmp(start, "impl", 4) == 0) return TOKEN_IMPL;
            }
            if (length == 6) return check_keyword(start, length, "import", TOKEN_IMPORT);
            if (length == 10) return check_keyword(start, length, "implements", TOKEN_IMPLEMENTS);
            break;
        case 'l':
            if (length == 3) return check_keyword(start, length, "let", TOKEN_LET);
            break;
        case 'm':
            if (length == 3) return check_keyword(start, length, "mut", TOKEN_MUT);
            if (length == 4) return check_keyword(start, length, "move", TOKEN_MOVE);
            if (length == 5) return check_keyword(start, length, "match", TOKEN_MATCH);
            break;
        case 'n':
            if (length == 3) return check_keyword(start, length, "new", TOKEN_NEW);
            break;
        case 'p':
            if (length == 3) return check_keyword(start, length, "ptr", TOKEN_PTR_TYPE);
            if (length == 5) return check_keyword(start, length, "print", TOKEN_PRINT);
            if (length == 6) return check_keyword(start, length, "public", TOKEN_PUBLIC);
            if (length == 7) return check_keyword(start, length, "private", TOKEN_PRIVATE);
            if (length == 9) return check_keyword(start, length, "protected", TOKEN_PROTECTED);
            break;
        case 'r':
            if (length == 3) return check_keyword(start, length, "ref", TOKEN_REF_TYPE);
            if (length == 6) {
                if (memcmp(start, "return", 6) == 0) return TOKEN_RETURN;
                if (memcmp(start, "rawptr", 6) == 0) return TOKEN_RAWPTR_TYPE;
            }
            break;
        case 's':
            if (length == 5) {
                if (memcmp(start, "super", 5) == 0) return TOKEN_SUPER;
                if (memcmp(start, "slice", 5) == 0) return TOKEN_SLICE_TYPE;
                if (memcmp(start, "spawn", 5) == 0) return TOKEN_SPAWN;
            }
            if (length == 6) {
                if (memcmp(start, "string", 6) == 0) return TOKEN_STRING_TYPE;
                if (memcmp(start, "static", 6) == 0) return TOKEN_STATIC;
                if (memcmp(start, "struct", 6) == 0) return TOKEN_STRUCT;
                if (memcmp(start, "strbuf", 6) == 0) return TOKEN_STRBUF_TYPE;
            }
            break;
        case 't':
            if (length == 3) return check_keyword(start, length, "try", TOKEN_TRY);
            if (length == 4) {
                if (memcmp(start, "true", 4) == 0) return TOKEN_TRUE;
                if (memcmp(start, "this", 4) == 0) return TOKEN_THIS;
            }
            if (length == 5) {
                if (memcmp(start, "throw", 5) == 0) return TOKEN_THROW;
                if (memcmp(start, "trait", 5) == 0) return TOKEN_TRAIT;
            }
            if (length == 6) return check_keyword(start, length, "tensor", TOKEN_TENSOR_TYPE);
            break;
        case 'u':
            if (length == 2) {
                if (start[1] == '8') return TOKEN_U8_TYPE;
            }
            if (length == 6) return check_keyword(start, length, "unsafe", TOKEN_UNSAFE);
            if (length == 3) {
                if (memcmp(start, "u16", 3) == 0) return TOKEN_U16_TYPE;
                if (memcmp(start, "u32", 3) == 0) return TOKEN_U32_TYPE;
                if (memcmp(start, "u64", 3) == 0) return TOKEN_U64_TYPE;
            }
            if (length == 4) return check_keyword(start, length, "u128", TOKEN_U128_TYPE);
            if (length == 5) return check_keyword(start, length, "union", TOKEN_UNION_KW);
            break;
        case 'v':
            if (length == 4) return check_keyword(start, length, "void", TOKEN_VOID_TYPE);
            break;
        case 'w':
            if (length == 5) {
                if (memcmp(start, "while", 5) == 0) return TOKEN_WHILE;
                if (memcmp(start, "where", 5) == 0) return TOKEN_WHERE;
            }
            break;
    }

    return TOKEN_IDENTIFIER;
}

static Token identifier(Lexer* lexer) {
    while (is_alpha(peek(lexer)) || is_digit(peek(lexer))) {
        advance(lexer);
    }
    
    TokenType type = identifier_type(lexer);
    Token token = make_token(lexer, type);
    
    // Store boolean values
    if (type == TOKEN_TRUE) {
        token.literal.bool_value = true;
    } else if (type == TOKEN_FALSE) {
        token.literal.bool_value = false;
    }
    
    return token;
}

Token scan_token(Lexer* lexer) {
    skip_whitespace(lexer);
    
    lexer->start = lexer->current;
    lexer->start_column = lexer->column;
    
    if (is_at_end(lexer)) {
        return make_token(lexer, TOKEN_EOF);
    }
    
    char c = advance(lexer);
    
    if (is_alpha(c)) return identifier(lexer);
    if (is_digit(c)) return number(lexer);
    
    switch (c) {
        case '(': return make_token(lexer, TOKEN_LPAREN);
        case ')': return make_token(lexer, TOKEN_RPAREN);
        case '{': return make_token(lexer, TOKEN_LBRACE);
        case '}': return make_token(lexer, TOKEN_RBRACE);
        case '[': return make_token(lexer, TOKEN_LBRACKET);
        case ']': return make_token(lexer, TOKEN_RBRACKET);
        case ';': return make_token(lexer, TOKEN_SEMICOLON);
        case ':':
            if (match(lexer, '=')) {
                return make_token(lexer, TOKEN_INFER_ASSIGN);  // := for type inference
            } else {
                return make_token(lexer, TOKEN_COLON);
            }
        case ',': return make_token(lexer, TOKEN_COMMA);
        case '.':
            // Only a dot if not followed by a digit (avoid confusion with floats)
            if (!is_digit(peek(lexer))) {
                return make_token(lexer, TOKEN_DOT);
            } else {
                return error_token(lexer, "Unexpected '.' before digit");
            }
        case '+':
            return make_token(lexer,
                match(lexer, '=') ? TOKEN_PLUS_ASSIGN : TOKEN_PLUS);
        case '*':
            return make_token(lexer,
                match(lexer, '=') ? TOKEN_STAR_ASSIGN : TOKEN_STAR);
        case '/':
            return make_token(lexer,
                match(lexer, '=') ? TOKEN_SLASH_ASSIGN : TOKEN_SLASH);
        case '%':
            return make_token(lexer,
                match(lexer, '=') ? TOKEN_PERCENT_ASSIGN : TOKEN_PERCENT);
        case '"': return string(lexer);
        case '^': return make_token(lexer, TOKEN_BITXOR);
        case '~': return make_token(lexer, TOKEN_BITNOT);
        
        case '-':
            if (match(lexer, '=')) return make_token(lexer, TOKEN_MINUS_ASSIGN);
            return make_token(lexer, 
                match(lexer, '>') ? TOKEN_ARROW : TOKEN_MINUS);
        
        case '<':
            if (match(lexer, '<')) return make_token(lexer, TOKEN_LSHIFT);
            if (match(lexer, '=')) return make_token(lexer, TOKEN_LESS_EQUAL);
            return make_token(lexer, TOKEN_LESS);
        
        case '>':
            if (match(lexer, '>')) return make_token(lexer, TOKEN_RSHIFT);
            return make_token(lexer,
                match(lexer, '=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
        
        case '=':
            if (match(lexer, '=')) {
                return make_token(lexer, TOKEN_EQUAL);
            } else if (match(lexer, '>')) {
                return make_token(lexer, TOKEN_FAT_ARROW);  // => for lambda shorthand
            } else {
                return make_token(lexer, TOKEN_ASSIGN);  // Single = is assignment
            }
        
        case '!':
            return make_token(lexer,
                match(lexer, '=') ? TOKEN_NOT_EQUAL : TOKEN_NOT);
        
        case '&':
            if (match(lexer, '&')) {
                return make_token(lexer, TOKEN_AND);
            } else {
                return make_token(lexer, TOKEN_BITAND);  // bitwise AND
            }
        
        case '|':
            if (match(lexer, '|')) {
                return make_token(lexer, TOKEN_OR);
            } else {
                return make_token(lexer, TOKEN_PIPE);  // bitwise OR / lambda pipe
            }

        case '?': return make_token(lexer, TOKEN_QUESTION);  // For optional types
        case '#':
            // Legacy # comment support (deprecated — use // instead)
            while (peek(lexer) != '\n' && !is_at_end(lexer)) {
                advance(lexer);
            }
            return scan_token(lexer);  // Continue scanning next token
    }

    return error_token(lexer, "Unexpected character");
}

void print_token(Token* token) {
    printf("Token(%d, \"", token->type);
    printf("%.*s", token->length, token->start);
    printf("\", line=%d, col=%d)\n", token->line, token->column);
}