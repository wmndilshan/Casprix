/*
 * Casprix Compiler - Debug and Visualization Module
 * Copyright (c) 2026 Casprix Project
 *
 * Academic features for viewing intermediate representations:
 * - Token stream from lexer
 * - Abstract Syntax Tree (AST)
 * - Symbol Table
 * - Compilation phases
 */

#ifndef ND_CORE_DEBUG_H
#define ND_CORE_DEBUG_H

#include <stdio.h>
#include <stdbool.h>
#include "compiler/frontend/lexer.h"
#include "compiler/frontend/ast.h"
#include "compiler/sema/symtable.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------------
 * Debug Configuration
 * --------------------------------------------------------------------------*/

typedef struct {
    bool dump_tokens;       /* Show lexer output */
    bool dump_ast;          /* Show abstract syntax tree */
    bool dump_symbols;      /* Show symbol table */
    bool dump_ir;           /* Show intermediate representation */
    bool step_by_step;      /* Pause between phases */
    bool verbose;           /* Extra details */
    FILE* output;           /* Output stream (default: stdout) */
    int indent_size;        /* Indentation for tree display */
} DebugConfig;

/* Global debug configuration */
extern DebugConfig g_debug_config;

/* Initialize debug configuration */
void debug_init(void);

/* ----------------------------------------------------------------------------
 * Token Visualization
 * --------------------------------------------------------------------------*/

/* Print a single token with details */
void debug_print_token(Token* token);

/* Print all tokens from source */
void debug_dump_tokens(const char* source);

/* Get token type name as string */
const char* debug_token_type_name(TokenType type);

/* ----------------------------------------------------------------------------
 * AST Visualization
 * --------------------------------------------------------------------------*/

/* Print AST for a program */
void debug_dump_ast(Stmt** statements, int count);

/* Print a single statement with indentation */
void debug_print_stmt(Stmt* stmt, int indent);

/* Print a single expression with indentation */
void debug_print_expr(Expr* expr, int indent);

/* Print data type */
const char* debug_data_type_name(DataType type);

/* ----------------------------------------------------------------------------
 * Symbol Table Visualization
 * --------------------------------------------------------------------------*/

/* Print entire symbol table */
void debug_dump_symbols(SymbolTable* table);

/* Print a single symbol */
void debug_print_symbol(Symbol* symbol, int indent);

/* Print class information */
void debug_print_class(ClassSymbol* cls, int indent);

/* ----------------------------------------------------------------------------
 * Compilation Phase Helpers
 * --------------------------------------------------------------------------*/

/* Print phase separator */
void debug_phase_start(const char* phase_name);

/* Print phase completion */
void debug_phase_end(const char* phase_name);

/* Wait for user input (step mode) */
void debug_step_wait(void);

/* ----------------------------------------------------------------------------
 * Utility Functions
 * --------------------------------------------------------------------------*/

/* Print indentation */
void debug_indent(int level);

/* Print a horizontal line */
void debug_line(char c, int width);

/* Print boxed header */
void debug_header(const char* title);

#ifdef __cplusplus
}
#endif

#endif /* ND_CORE_DEBUG_H */
