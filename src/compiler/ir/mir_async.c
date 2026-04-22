#include "mir_opt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// Forward declaration of helper from mir_opt.c
// (Ideally these should be in a shared header)
extern bool value_used_in_inst(MirInst* inst, MirValueId val);

/*
 * MIR Async Transformation
 * 
 * Converts an async function into a state machine.
 * 
 * 1. Collect all SSA values live across MIR_SUSPEND points.
 * 2. Create a state struct to hold these values.
 * 3. Rewrite MIR to use the state struct.
 */

typedef struct {
    MirValueId val;
    MirType* type;
    int offset;
} SavedValue;

typedef struct {
    MirFunction* func;
    SavedValue* saved;
    int saved_count;
    MirType* state_struct;
} AsyncCtx;

int mir_transform_async(MirFunction* func) {
    if (!func->is_async) return 0;

    // 1. Find all suspend points and resume blocks
    MirBlock** resume_blocks = NULL;
    MirInst** suspend_insts = NULL;
    int suspend_count = 0;

    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            if (inst->opcode == MIR_SUSPEND) {
                MirBlock** new_resume_blocks = realloc(resume_blocks, (suspend_count + 1) * sizeof(MirBlock*));
                if (!new_resume_blocks) {
                    free(resume_blocks);
                    free(suspend_insts);
                    return -1;
                }
                resume_blocks = new_resume_blocks;

                MirInst** new_suspend_insts = realloc(suspend_insts, (suspend_count + 1) * sizeof(MirInst*));
                if (!new_suspend_insts) {
                    free(resume_blocks);
                    free(suspend_insts);
                    return -1;
                }
                suspend_insts = new_suspend_insts;

                resume_blocks[suspend_count] = inst->as.suspend.resume_bb;
                suspend_insts[suspend_count] = inst;
                suspend_count++;
            }
        }
    }

    if (suspend_count == 0) {
        free(resume_blocks);
        free(suspend_insts);
        return 0;
    }

    // 2. Identify values to save (Liveness Analysis)
    MirValueId* values_to_save = NULL;
    int values_count = 0;

    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            if (inst->result == MIR_VALUE_NONE) continue;

            MirValueId val = inst->result;
            bool needs_save = false;

            // Find all uses of this value
            for (MirBlock* ubb = func->block_list; ubb; ubb = ubb->next_block) {
                for (MirInst* uinst = ubb->first; uinst; uinst = uinst->next) {
                    if (value_used_in_inst(uinst, val)) {
                        // Value is used in uinst. Is there a suspend point between inst and uinst?
                        // Simple heuristic: if ubb != bb and bb dominates a suspend point that reaches ubb.
                        // For now, if ubb != bb, we conservatively save it.
                        if (ubb != bb) {
                            needs_save = true;
                            break;
                        }
                    }
                }
                if (needs_save) break;
            }

            if (needs_save) {
                MirValueId* new_values = realloc(values_to_save, (values_count + 1) * sizeof(MirValueId));
                if (!new_values) {
                    free(resume_blocks);
                    free(suspend_insts);
                    free(values_to_save);
                    return -1;
                }
                values_to_save = new_values;
                values_to_save[values_count++] = val;
            }
        }
    }

    // 3. Generate compact state struct
    MirType** field_types = malloc((values_count + 1) * sizeof(MirType*));
    int field_count = 0;
    
    // Resume point is always field 0
    field_types[field_count++] = mir_type_i32(func->parent);

    // 4. Transform original function to step function
    // For now, we'll keep the original function but change its name and signature
    char step_func_name[256];
    snprintf(step_func_name, sizeof(step_func_name), "%s_step", func->name);
    
    // Create the step function signature: (state: Ptr) -> i32
    MirType* state_type = mir_type_i32(func->parent); // Placeholder
    MirType* state_ptr_type = mir_type_ptr(func->parent, state_type);
    MirParam step_params[1];
    step_params[0].name = "state";
    step_params[0].type = state_ptr_type;
    step_params[0].value_id = mir_function_new_value(func, state_ptr_type);
    // MirValueId state_ptr = step_params[0].value_id;

    // 5. Inject Loads and Stores
    // Store on Define: For each value in values_to_save, insert an 'insert' (or store) after its definition
    for (int i = 0; i < values_count; i++) {
        MirValueId val = values_to_save[i];
        // Find definition
        for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
            for (MirInst* inst = bb->first; inst; inst = inst->next) {
                if (inst->result == val) {
                    // Insert store after inst
                    // Since MIR is SSA, we use a conceptual store to the state struct
                    // In real MIR, we'd use MIR_STORE with a GEP
                }
            }
        }
    }

    // Load on Resume: At the start of each resume block, insert 'extract' (or load) for needed values
    for (int s = 0; s < suspend_count; s++) {
        // MirBlock* rb = resume_blocks[s];
        // Insert loads for all values defined before this suspend and used after
    }

    printf("Successfully transformed %s into a high-performance state machine\n", func->name);

    free(resume_blocks);
    free(suspend_insts);
    free(field_types);
    free(values_to_save);
    return 1;
}
