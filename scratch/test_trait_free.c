#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compiler/frontend/lexer.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/ast.h"
#include "support/diagnostic.h"
#include "driver/io.h"
#include "driver/cli.h"

int main() {
    diag_engine_init(&g_diag);
    
    const char* source_path = "test_trait_leak.cpx";
    char* source = driver_read_file(source_path);
    if (!source) {
        fprintf(stderr, "Failed to read %s\n", source_path);
        return 1;
    }
    
    Lexer lexer;
    init_lexer(&lexer, source);
    
    Parser parser;
    init_parser(&parser, &lexer);
    
    int stmt_count = 0;
    Stmt** statements = parse(&parser, &stmt_count);
    
    printf("Parsed %d statements.\n", stmt_count);
    
    for (int i = 0; i < stmt_count; i++) {
        if (statements[i]) {
            free_stmt(statements[i]);
        }
    }
    free(statements);
    free(source);
    
    diag_engine_destroy(&g_diag);
    printf("Successfully freed all statements.\n");
    return 0;
}
