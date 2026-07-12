#ifndef ASMGEN_H
#define ASMGEN_H

#include "casprix/common.h"
#include "compiler/frontend/ast.h"
#include "support/log.h"
#include "compiler/sema/symtable.h"
#include "compiler/sema/drop_planner.h"

typedef struct {
    FILE* output;
    int label_count;
    int string_count;
    int var_count;
    int temp_count;
    char** string_literals;
    int string_capacity;
    int string_size;
    ClassSymbol* current_class;
    char loop_start_labels[32][32];
    char loop_end_labels[32][32];
    int loop_depth;

    /* Per-function stack frame for local variables/params */
    char** local_names;      /* Variable names in current scope */
    int*   local_offsets;    /* Positive offset from rbp base (subtracted at use) */
    int    local_count;
    int    local_capacity;
    int    frame_size;       /* Total bytes allocated for locals (rounded up to 16) */
    
    /* Codegen-time Drop Planner for emitting ARC releases */
    DropPlanner drop_ctx;
} AssemblyGenerator;

/* Logger parameter is deprecated - uses global nd_logger */
void init_asm_generator(AssemblyGenerator* gen, FILE* output, void* unused);
void generate_assembly(AssemblyGenerator* gen, Stmt** statements, int count, SymbolTable* symbols);
void free_asm_generator(AssemblyGenerator* gen);

#endif // ASMGEN_H

