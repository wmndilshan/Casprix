/*
 * Casprix Compiler - Peephole Optimizer Implementation
 */

#include "compiler/opt/peephole.h"
#include <stdlib.h>
#include <string.h>

void init_peephole(PeepholeContext* ctx) {
    ctx->instr_capacity = 1024;
    ctx->instructions = malloc(ctx->instr_capacity * sizeof(AsmInstr));
    ctx->instr_count = 0;
    ctx->optimizations_applied = 0;
}

void peephole_add_instr(PeepholeContext* ctx, const char* opcode,
                        const char* op1, const char* op2, const char* comment) {
    if (ctx->instr_count >= ctx->instr_capacity) {
        ctx->instr_capacity *= 2;
        ctx->instructions = realloc(ctx->instructions, 
                                    ctx->instr_capacity * sizeof(AsmInstr));
    }
    
    AsmInstr* instr = &ctx->instructions[ctx->instr_count++];
    instr->opcode = opcode ? strdup(opcode) : NULL;
    instr->operand1 = op1 ? strdup(op1) : NULL;
    instr->operand2 = op2 ? strdup(op2) : NULL;
    instr->comment = comment ? strdup(comment) : NULL;
    instr->line_number = ctx->instr_count;
}

/* Return true if opcode reads EFLAGS (so xor-for-zero would break it). */
static bool is_flag_consumer(const char* op) {
    static const char* consumers[] = {
        "jz","jnz","je","jne","jl","jle","jg","jge",
        "jb","jbe","ja","jae","js","jns","jp","jnp","jo","jno","jc","jnc",
        "jrcxz","jecxz",
        "cmovz","cmovnz","cmove","cmovne","cmovl","cmovle","cmovg","cmovge",
        "cmovb","cmovbe","cmova","cmovae","cmovs","cmovns","cmovo","cmovno",
        "setz","setnz","sete","setne","setl","setle","setg","setge",
        "setb","setbe","seta","setae","sets","setns","seto","setno",
        NULL
    };
    if (!op) return false;
    for (int i = 0; consumers[i]; i++)
        if (strcmp(op, consumers[i]) == 0) return true;
    return false;
}

/* Return true if opcode unconditionally writes EFLAGS. */
static bool is_flag_setter(const char* op) {
    static const char* setters[] = {
        "add","sub","and","or","xor","cmp","test","inc","dec",
        "neg","imul","mul","div","idiv","shl","shr","sar","sal",
        "adc","sbb","rcl","rcr",
        NULL
    };
    if (!op) return false;
    for (int i = 0; setters[i]; i++)
        if (strcmp(op, setters[i]) == 0) return true;
    return false;
}

/* Return true if it is safe to replace `mov reg, 0` at position i with
   `xor reg, reg`.  Unsafe when a flag-consuming instruction (jcc etc.)
   appears before the next flag-setter, because xor clobbers EFLAGS while
   mov does not. */
static bool xor_zero_is_safe(PeepholeContext* ctx, int i) {
    for (int j = i + 1; j < ctx->instr_count; j++) {
        const char* op = ctx->instructions[j].opcode;
        if (!op || strcmp(op, "nop") == 0) continue;
        if (is_flag_consumer(op)) return false;  /* flags read before reset */
        if (is_flag_setter(op))   return true;   /* flags overwritten first */
        /* Calls and unconditional jumps terminate the search conservatively. */
        if (strcmp(op, "call") == 0 || strcmp(op, "jmp") == 0 ||
            strcmp(op, "ret") == 0)
            return false;
    }
    return true;  /* no flag consumer found before end of instruction stream */
}

/* Pattern: mov rax, 0 => xor rax, rax (smaller encoding, faster on modern CPUs).
   Only applied when it is safe — i.e. no flag-consuming instruction lies
   between this mov and the next flag-setter. */
bool peephole_use_xor_for_zero(PeepholeContext* ctx) {
    bool changed = false;

    for (int i = 0; i < ctx->instr_count; i++) {
        AsmInstr* instr = &ctx->instructions[i];

        if (instr->opcode && strcmp(instr->opcode, "mov") == 0 &&
            instr->operand2 && strcmp(instr->operand2, "0") == 0 &&
            xor_zero_is_safe(ctx, i)) {

            free(instr->opcode);
            instr->opcode = strdup("xor");
            free(instr->operand2);
            instr->operand2 = strdup(instr->operand1);

            changed = true;
            ctx->optimizations_applied++;
        }
    }

    return changed;
}

// Pattern: redundant moves
bool peephole_remove_redundant_moves(PeepholeContext* ctx) {
    bool changed = false;
    
    for (int i = 0; i < ctx->instr_count - 1; i++) {
        AsmInstr* i1 = &ctx->instructions[i];
        AsmInstr* i2 = &ctx->instructions[i + 1];
        
        // mov rax, rbx; mov rbx, rax => mov rax, rbx
        if (i1->opcode && strcmp(i1->opcode, "mov") == 0 &&
            i2->opcode && strcmp(i2->opcode, "mov") == 0 &&
            i1->operand1 && i2->operand2 &&
            i1->operand2 && i2->operand1 &&
            strcmp(i1->operand1, i2->operand2) == 0 &&
            strcmp(i1->operand2, i2->operand1) == 0) {
            
            // Remove second instruction
            free(i2->opcode);
            i2->opcode = strdup("nop");
            if (i2->operand1) free(i2->operand1);
            if (i2->operand2) free(i2->operand2);
            i2->operand1 = NULL;
            i2->operand2 = NULL;
            
            changed = true;
            ctx->optimizations_applied++;
        }
        
        // mov rax, rax => remove
        if (i1->opcode && strcmp(i1->opcode, "mov") == 0 &&
            i1->operand1 && i1->operand2 &&
            strcmp(i1->operand1, i1->operand2) == 0) {
            
            free(i1->opcode);
            i1->opcode = strdup("nop");
            free(i1->operand1);
            free(i1->operand2);
            i1->operand1 = NULL;
            i1->operand2 = NULL;
            
            changed = true;
            ctx->optimizations_applied++;
        }
    }
    
    return changed;
}

// Pattern: combine arithmetic (add rax, 5; add rax, 3 => add rax, 8)
bool peephole_combine_arithmetic(PeepholeContext* ctx) {
    bool changed = false;
    
    for (int i = 0; i < ctx->instr_count - 1; i++) {
        AsmInstr* i1 = &ctx->instructions[i];
        AsmInstr* i2 = &ctx->instructions[i + 1];
        
        if (i1->opcode && i2->opcode &&
            strcmp(i1->opcode, "add") == 0 &&
            strcmp(i2->opcode, "add") == 0 &&
            i1->operand1 && i2->operand1 &&
            strcmp(i1->operand1, i2->operand1) == 0) {
            
            // Try to parse immediate values
            int val1 = 0, val2 = 0;
            if (i1->operand2 && i2->operand2 &&
                sscanf(i1->operand2, "%d", &val1) == 1 &&
                sscanf(i2->operand2, "%d", &val2) == 1) {
                
                // Combine into first instruction
                char combined[32];
                snprintf(combined, sizeof(combined), "%d", val1 + val2);
                free(i1->operand2);
                i1->operand2 = strdup(combined);
                
                // Remove second instruction
                free(i2->opcode);
                i2->opcode = strdup("nop");
                
                changed = true;
                ctx->optimizations_applied++;
            }
        }
    }
    
    return changed;
}

// Pattern: eliminate dead stores
bool peephole_eliminate_dead_stores(PeepholeContext* ctx) {
    bool changed = false;
    
    for (int i = 0; i < ctx->instr_count - 1; i++) {
        AsmInstr* i1 = &ctx->instructions[i];
        AsmInstr* i2 = &ctx->instructions[i + 1];
        
        // mov [rbp-8], rax; mov [rbp-8], rbx => mov [rbp-8], rbx
        if (i1->opcode && i2->opcode &&
            strcmp(i1->opcode, "mov") == 0 &&
            strcmp(i2->opcode, "mov") == 0 &&
            i1->operand1 && i2->operand1 &&
            strcmp(i1->operand1, i2->operand1) == 0 &&
            strstr(i1->operand1, "[") != NULL) {  // Memory location
            
            free(i1->opcode);
            i1->opcode = strdup("nop");
            
            changed = true;
            ctx->optimizations_applied++;
        }
    }
    
    return changed;
}

// Strength reduction (mul by power of 2 => shift)
bool peephole_strength_reduce(PeepholeContext* ctx) {
    bool changed = false;
    
    for (int i = 0; i < ctx->instr_count; i++) {
        AsmInstr* instr = &ctx->instructions[i];
        
        // imul rax, 8 => shl rax, 3
        if (instr->opcode && strcmp(instr->opcode, "imul") == 0 &&
            instr->operand2) {
            
            int val = 0;
            if (sscanf(instr->operand2, "%d", &val) == 1) {
                // Check if power of 2
                if (val > 0 && (val & (val - 1)) == 0) {
                    int shift = 0;
                    while ((1 << shift) != val) shift++;
                    
                    free(instr->opcode);
                    instr->opcode = strdup("shl");
                    free(instr->operand2);
                    
                    char shift_str[16];
                    snprintf(shift_str, sizeof(shift_str), "%d", shift);
                    instr->operand2 = strdup(shift_str);
                    
                    changed = true;
                    ctx->optimizations_applied++;
                }
            }
        }
    }
    
    return changed;
}

// Run all optimization passes
void peephole_optimize(PeepholeContext* ctx) {
    bool changed = true;
    int pass = 0;
    
    while (changed && pass < 10) {  // Max 10 passes
        changed = false;
        
        changed |= peephole_use_xor_for_zero(ctx);
        changed |= peephole_remove_redundant_moves(ctx);
        changed |= peephole_combine_arithmetic(ctx);
        changed |= peephole_eliminate_dead_stores(ctx);
        changed |= peephole_strength_reduce(ctx);
        
        pass++;
    }
}

// Emit optimized assembly
void peephole_emit(PeepholeContext* ctx, FILE* out) {
    for (int i = 0; i < ctx->instr_count; i++) {
        AsmInstr* instr = &ctx->instructions[i];
        
        // Skip nops
        if (instr->opcode && strcmp(instr->opcode, "nop") == 0) {
            continue;
        }
        
        fprintf(out, "    %s", instr->opcode ? instr->opcode : "");
        
        if (instr->operand1) {
            fprintf(out, " %s", instr->operand1);
            if (instr->operand2) {
                fprintf(out, ", %s", instr->operand2);
            }
        }
        
        if (instr->comment) {
            fprintf(out, "  ; %s", instr->comment);
        }
        
        fprintf(out, "\n");
    }
}

void free_peephole(PeepholeContext* ctx) {
    for (int i = 0; i < ctx->instr_count; i++) {
        AsmInstr* instr = &ctx->instructions[i];
        if (instr->opcode) free(instr->opcode);
        if (instr->operand1) free(instr->operand1);
        if (instr->operand2) free(instr->operand2);
        if (instr->comment) free(instr->comment);
    }
    free(ctx->instructions);
}
