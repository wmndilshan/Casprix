#ifndef SEMANTIC_H
#define SEMANTIC_H

#include "casprix/common.h"
#include "compiler/frontend/ast.h"
#include "compiler/sema/symtable.h"

typedef struct {
    SymbolTable* symbols;
    int scope_depth;
    DataType current_function_return_type;
    DataType* current_lambda_return_type;  // Pointer to active lambda's return_type for inference
    bool in_function;
    bool in_async_function;
    ClassSymbol* current_class;  // NULL when not in a class method
    MethodSymbol* current_method;  // NULL when not in a method
    int loop_depth;  // Track loop nesting for break/continue validation
    int alloc_scope_depth;  // Track nested alloc { ... } regions
} SemanticAnalyzer;

void init_semantic_analyzer(SemanticAnalyzer* analyzer);
void free_semantic_analyzer(SemanticAnalyzer* analyzer);

bool analyze_program(SemanticAnalyzer* analyzer, Stmt** statements, int count);

#endif // SEMANTIC_H
