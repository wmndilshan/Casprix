/*
 * Casprix Compiler - Register Allocation
 * Linear scan and graph coloring register allocation for x86-64
 */

#ifndef REGALLOC_H
#define REGALLOC_H

#include "compiler/frontend/ast.h"
#include <stdbool.h>
#include <stdint.h>

// x86-64 registers
typedef enum {
    REG_RAX = 0, REG_RCX, REG_RDX, REG_RBX,
    REG_RSI, REG_RDI, REG_RBP, REG_RSP,
    REG_R8,  REG_R9,  REG_R10, REG_R11,
    REG_R12, REG_R13, REG_R14, REG_R15,
    REG_SPILL = -1  // Spilled to stack
} Register;

// Register classes
typedef enum {
    REG_CLASS_GP,      // General purpose
    REG_CLASS_FLOAT,   // XMM registers
    REG_CLASS_ANY
} RegClass;

// Live interval for a variable
typedef struct {
    char* var_name;
    int start_pos;      // Start of live range
    int end_pos;        // End of live range
    Register assigned_reg;
    int spill_slot;     // Stack offset if spilled
    int use_count;      // Number of uses (for priority)
    RegClass reg_class;
} LiveInterval;

// Register allocator
typedef struct {
    LiveInterval* intervals;
    int interval_count;
    int max_intervals;

    // Available registers (bitmap)
    bool gp_regs_available[16];
    bool fp_regs_available[16];

    // Spill tracking
    int spill_count;
    int spill_slot_size;

    // Current position
    int current_pos;

    /* Bitmask of callee-saved GP registers actually assigned during allocation.
       Bit i is set when REG_RBX/R12/R13/R14/R15 (i = register index) is used.
       The prologue/epilogue generator reads this to emit the correct push/pop
       sequence without saving unused callee-saved registers. */
    uint32_t used_callee_saved_mask;
} RegAllocator;

// Initialize register allocator
void init_regalloc(RegAllocator* alloc);

// Add live interval
void regalloc_add_interval(RegAllocator* alloc, const char* var_name,
                           int start, int end, RegClass class);

// Perform register allocation (linear scan)
void regalloc_linear_scan(RegAllocator* alloc);

// Get assigned register for variable
Register regalloc_get_register(RegAllocator* alloc, const char* var_name);

// Check if variable is spilled
bool regalloc_is_spilled(RegAllocator* alloc, const char* var_name);

// Get sp ill slot offset
int regalloc_get_spill_offset(RegAllocator* alloc, const char* var_name);

// Free a register
void regalloc_free_register(RegAllocator* alloc, Register reg);

// Get register name
const char* register_name(Register reg, int size);

// Return the bitmask of callee-saved GP registers used (for prologue/epilogue)
uint32_t regalloc_used_callee_saved_mask(const RegAllocator* alloc);

// Cleanup
void free_regalloc(RegAllocator* alloc);

#endif // REGALLOC_H
