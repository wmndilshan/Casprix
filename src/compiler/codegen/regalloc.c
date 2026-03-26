/*
 * Casprix Compiler - Register Allocation Implementation
 */

#include "compiler/codegen/regalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// x86-64 register names
static const char* gp_reg_names_64[] = {
    "rax", "rcx", "rdx", "rbx", "rsi", "rdi", "rbp", "rsp",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};

static const char* gp_reg_names_32[] = {
    "eax", "ecx", "edx", "ebx", "esi", "edi", "ebp", "esp",
    "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"
};

static const char* gp_reg_names_8[] = {
    "al", "cl", "dl", "bl", "sil", "dil", "bpl", "spl",
    "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"
};

void init_regalloc(RegAllocator* alloc) {
    alloc->intervals = malloc(64 * sizeof(LiveInterval));
    alloc->interval_count = 0;
    alloc->max_intervals = 64;
    alloc->spill_count = 0;
    alloc->spill_slot_size = 0;
    alloc->current_pos = 0;
    
    // Initialize available registers
    // rax, rcx, rdx, rsi, rdi, r8-r11 are caller-saved (volatile)
    // rbx, r12-r15 are callee-saved (non-volatile)
    // rbp, rsp are special (stack management)
    
    for (int i = 0; i < 16; i++) {
        alloc->gp_regs_available[i] = true;
        alloc->fp_regs_available[i] = true;
    }
    
    // Reserve special registers
    alloc->gp_regs_available[REG_RBP] = false;  // Frame pointer
    alloc->gp_regs_available[REG_RSP] = false;  // Stack pointer
}

void regalloc_add_interval(RegAllocator* alloc, const char* var_name,
                           int start, int end, RegClass class) {
    if (alloc->interval_count >= alloc->max_intervals) {
        alloc->max_intervals *= 2;
        alloc->intervals = realloc(alloc->intervals, 
                                   alloc->max_intervals * sizeof(LiveInterval));
    }
    
    LiveInterval* interval = &alloc->intervals[alloc->interval_count++];
    interval->var_name = strdup(var_name);
    interval->start_pos = start;
    interval->end_pos = end;
    interval->assigned_reg = REG_SPILL;
    interval->spill_slot = -1;
    interval->use_count = 0;
    interval->reg_class = class;
}

// Comparison function for sorting intervals by start position
static int compare_intervals(const void* a, const void* b) {
    const LiveInterval* ia = (const LiveInterval*)a;
    const LiveInterval* ib = (const LiveInterval*)b;
    return ia->start_pos - ib->start_pos;
}

// Linear scan register allocation
void regalloc_linear_scan(RegAllocator* alloc) {
    // Sort intervals by start position
    qsort(alloc->intervals, alloc->interval_count, 
          sizeof(LiveInterval), compare_intervals);
    
    // Active intervals (currently allocated)
    LiveInterval** active = malloc(16 * sizeof(LiveInterval*));
    int active_count = 0;
    
    for (int i = 0; i < alloc->interval_count; i++) {
        LiveInterval* current = &alloc->intervals[i];
        
        // Expire old intervals
        for (int j = 0; j < active_count; j++) {
            if (active[j]->end_pos <= current->start_pos) {
                // Free the register
                if (active[j]->assigned_reg != REG_SPILL) {
                    if (current->reg_class == REG_CLASS_GP) {
                        alloc->gp_regs_available[active[j]->assigned_reg] = true;
                    }
                }
                // Remove from active list
                active[j] = active[active_count - 1];
                active_count--;
                j--;
            }
        }
        
        // Try to allocate a register
        bool allocated = false;
        if (current->reg_class == REG_CLASS_GP) {
            // Prefer volatile registers first (no save/restore needed)
            int preferred[] = {REG_RAX, REG_RCX, REG_RDX, REG_RSI, REG_RDI,
                              REG_R8, REG_R9, REG_R10, REG_R11};
            
            for (int j = 0; j < 9; j++) {
                int reg = preferred[j];
                if (alloc->gp_regs_available[reg]) {
                    current->assigned_reg = reg;
                    alloc->gp_regs_available[reg] = false;
                    allocated = true;
                    break;
                }
            }
            
            // Try callee-saved if no volatile available
            if (!allocated) {
                int callee_saved[] = {REG_RBX, REG_R12, REG_R13, REG_R14, REG_R15};
                for (int j = 0; j < 5; j++) {
                    int reg = callee_saved[j];
                    if (alloc->gp_regs_available[reg]) {
                        current->assigned_reg = reg;
                        alloc->gp_regs_available[reg] = false;
                        allocated = true;
                        break;
                    }
                }
            }
        }
        
        // Spill if no register available
        if (!allocated) {
            current->assigned_reg = REG_SPILL;
            current->spill_slot = alloc->spill_count++;
            alloc->spill_slot_size += 8;
        } else {
            active[active_count++] = current;
        }
    }
    
    free(active);
}

Register regalloc_get_register(RegAllocator* alloc, const char* var_name) {
    for (int i = 0; i < alloc->interval_count; i++) {
        if (strcmp(alloc->intervals[i].var_name, var_name) == 0) {
            return alloc->intervals[i].assigned_reg;
        }
    }
    return REG_SPILL;
}

bool regalloc_is_spilled(RegAllocator* alloc, const char* var_name) {
    return regalloc_get_register(alloc, var_name) == REG_SPILL;
}

int regalloc_get_spill_offset(RegAllocator* alloc, const char* var_name) {
    for (int i = 0; i < alloc->interval_count; i++) {
        if (strcmp(alloc->intervals[i].var_name, var_name) == 0) {
            return -((alloc->intervals[i].spill_slot + 1) * 8);
        }
    }
    return 0;
}

void regalloc_free_register(RegAllocator* alloc, Register reg) {
    if (reg >= 0 && reg < 16) {
        alloc->gp_regs_available[reg] = true;
    }
}

const char* register_name(Register reg, int size) {
    if (reg == REG_SPILL || reg < 0 || reg >= 16) {
        return "SPILLED";
    }
    
    switch (size) {
        case 1: return gp_reg_names_8[reg];
        case 4: return gp_reg_names_32[reg];
        case 8: return gp_reg_names_64[reg];
        default: return gp_reg_names_64[reg];
    }
}

void free_regalloc(RegAllocator* alloc) {
    for (int i = 0; i < alloc->interval_count; i++) {
        free(alloc->intervals[i].var_name);
    }
    free(alloc->intervals);
}
