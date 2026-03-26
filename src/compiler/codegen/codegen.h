#ifndef CODEGEN_H
#define CODEGEN_H

#include "../include/common.h"
#include "compiler/frontend/ast.h"

typedef struct {
    FILE* output;
    int indent_level;
    int temp_count;
    int label_count;
} CodeGenerator;

void init_code_generator(CodeGenerator* gen, FILE* output);
void generate_code(CodeGenerator* gen, Stmt** statements, int count);

#endif // CODEGEN_H