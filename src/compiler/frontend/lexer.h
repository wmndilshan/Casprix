#ifndef LEXER_H
#define LEXER_H

#include "casprix/common.h"

typedef enum {
    // Literals
    TOKEN_INTEGER,
    TOKEN_FLOAT,
    TOKEN_STRING,
    TOKEN_TRUE,          // true
    TOKEN_FALSE,         // false

    // Identifiers
    TOKEN_IDENTIFIER,

    // Keywords - all lowercase
    TOKEN_INT_TYPE,      // int (alias for i32)
    TOKEN_FLOAT_TYPE,    // float (alias for f64)
    TOKEN_STRING_TYPE,   // string
    TOKEN_BOOL_TYPE,     // bool
    TOKEN_VOID_TYPE,     // void

    // Explicit-width integer types
    TOKEN_I8_TYPE,       // i8
    TOKEN_I16_TYPE,      // i16
    TOKEN_I32_TYPE,      // i32
    TOKEN_I64_TYPE,      // i64
    TOKEN_I128_TYPE,     // i128
    TOKEN_U8_TYPE,       // u8
    TOKEN_U16_TYPE,      // u16
    TOKEN_U32_TYPE,      // u32
    TOKEN_U64_TYPE,      // u64
    TOKEN_U128_TYPE,     // u128

    // Explicit-width float types
    TOKEN_F16_TYPE,      // f16
    TOKEN_F32_TYPE,      // f32
    TOKEN_F64_TYPE,      // f64
    TOKEN_BF16_TYPE,     // bf16

    // Character type
    TOKEN_CHAR_TYPE,     // char

    // Mutable string buffer
    TOKEN_STRBUF_TYPE,   // strbuf

    // Collection type keywords
    TOKEN_ARRAY_TYPE,    // array
    TOKEN_SLICE_TYPE,    // slice

    // Pointer/reference type keywords
    TOKEN_PTR_TYPE,      // ptr
    TOKEN_RAWPTR_TYPE,   // rawptr
    TOKEN_REF_TYPE,      // ref

    // SIMD/math type keywords
    TOKEN_TENSOR_TYPE,   // tensor

    // Declaration keywords
    TOKEN_LET,           // let
    TOKEN_CONST,         // const (compile-time constant)
    TOKEN_MUT,           // mut (mutable binding)
    TOKEN_STRUCT,        // struct
    TOKEN_ENUM_KW,       // enum
    TOKEN_UNION_KW,      // union

    // Control flow & language keywords
    TOKEN_PRINT,         // print
    TOKEN_IF,            // if
    TOKEN_ELIF,          // elif
    TOKEN_ELSE,          // else
    TOKEN_FOR,           // for
    TOKEN_WHILE,         // while
    TOKEN_FUNC,          // func
    TOKEN_RETURN,        // return
    TOKEN_BREAK,         // break
    TOKEN_CONTINUE,      // continue
    TOKEN_INCLUDE,       // (legacy, maps to import)
    TOKEN_IMPORT,        // import
    TOKEN_CLASS,         // class
    TOKEN_EXTENDS,       // extends
    TOKEN_THIS,          // this
    TOKEN_NEW,           // new
    TOKEN_STATIC,        // static
    TOKEN_SUPER,         // super
    TOKEN_ABSTRACT,      // abstract
    TOKEN_PRIVATE,       // private
    TOKEN_PUBLIC,        // public
    TOKEN_PROTECTED,     // protected
    TOKEN_EXTERN,        // extern

    // Async/Concurrency keywords
    TOKEN_ASYNC,         // async
    TOKEN_AWAIT,         // await
    TOKEN_SPAWN,         // spawn
    TOKEN_MOVE,          // move
    TOKEN_UNSAFE,        // unsafe

    // Closure capture keywords
    TOKEN_COPY,          // copy (for closure capture mode)
    TOKEN_WHERE,         // where (for type constraints)

    // Pattern matching / control flow
    TOKEN_MATCH,         // match
    TOKEN_IN,            // in   (for-in loops)

    // Exception handling
    TOKEN_TRY,           // try
    TOKEN_CATCH,         // catch
    TOKEN_FINALLY,       // finally
    TOKEN_THROW,         // throw

    // Trait / interface
    TOKEN_TRAIT,         // trait
    TOKEN_IMPL,          // impl
    TOKEN_IMPLEMENTS,    // implements

    // Operators
    TOKEN_PLUS,          // +
    TOKEN_MINUS,         // -
    TOKEN_STAR,          // *
    TOKEN_SLASH,         // /
    TOKEN_PERCENT,       // %
    TOKEN_ASSIGN,        // =
    TOKEN_INFER_ASSIGN,  // :=  (type inference assignment)
    TOKEN_EQUAL,         // ==
    TOKEN_NOT_EQUAL,     // !=
    TOKEN_LESS,          // <
    TOKEN_LESS_EQUAL,    // <=
    TOKEN_GREATER,       // >
    TOKEN_GREATER_EQUAL, // >=
    TOKEN_AND,           // &&
    TOKEN_OR,            // ||
    TOKEN_NOT,           // !

    // Compound assignment operators
    TOKEN_PLUS_ASSIGN,   // +=
    TOKEN_MINUS_ASSIGN,  // -=
    TOKEN_STAR_ASSIGN,   // *=
    TOKEN_SLASH_ASSIGN,  // /=
    TOKEN_PERCENT_ASSIGN,// %=

    // Bitwise operators
    TOKEN_BITAND,        // &   (bitwise AND)
    TOKEN_BITXOR,        // ^   (bitwise XOR)
    TOKEN_BITNOT,        // ~   (bitwise NOT / unary complement)
    TOKEN_LSHIFT,        // <<  (left shift)
    TOKEN_RSHIFT,        // >>  (right shift)
    // Note: TOKEN_PIPE (|) already doubles as bitwise OR in non-lambda context

    // Delimiters
    TOKEN_LPAREN,        // (
    TOKEN_RPAREN,        // )
    TOKEN_LBRACE,        // {
    TOKEN_RBRACE,        // }
    TOKEN_LBRACKET,      // [
    TOKEN_RBRACKET,      // ]
    TOKEN_SEMICOLON,     // ;
    TOKEN_COLON,         // :
    TOKEN_COMMA,         // ,
    TOKEN_ARROW,         // ->
    TOKEN_DOT,           // .
    TOKEN_PIPE,          // | (for lambda params)
    TOKEN_FAT_ARROW,     // => (for lambda shorthand)
    TOKEN_QUESTION,      // ? (for optional types)

    // Special
    TOKEN_ERROR,
    TOKEN_EOF
} CpxTokenType;

typedef struct {
    CpxTokenType type;
    const char* start;   // Points to start of lexeme in source
    int length;
    int line;
    int column;

    // Literal values
    union {
        int64_t int_value;
        double float_value;
        char* string_value;
        bool bool_value;
    } literal;
} Token;

typedef struct {
    const char* start;
    const char* current;
    int line;
    int column;
    int start_column;
} Lexer;

void init_lexer(Lexer* lexer, const char* source);
Token scan_token(Lexer* lexer);
void print_token(Token* token);

#endif // LEXER_H
