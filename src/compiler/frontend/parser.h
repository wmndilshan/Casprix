#ifndef PARSER_H
#define PARSER_H

#include "casprix/common.h"
#include "compiler/frontend/lexer.h"
#include "compiler/frontend/ast.h"

typedef struct {
    Lexer* lexer;
    Token current;
    Token previous;
    bool had_error;
    bool panic_mode;
    const char* source; // Keep reference to source for manual extraction if needed
} Parser;

void init_parser(Parser* parser, Lexer* lexer);
Stmt** parse(Parser* parser, int* stmt_count);

#endif // PARSER_H