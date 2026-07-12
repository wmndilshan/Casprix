#include "mir.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/**
 * MIR Async Transformation
 * 
 * Lowers async functions into state machines.
 */

int mir_transform_async(MirFunction* func) {
    if (!func || !func->is_async) return 0;

    // 1. Identify suspend points
    int suspend_count = 0;
    MirInst** suspend_insts = NULL;
    MirBlock** resume_blocks = NULL;

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

    if (suspend_count == 0) return 0;

    // 2. Identify values that live across suspend points
    int values_count = 0;
    MirValueId* values_to_save = NULL;

    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            MirValueId val = inst->result;
            if (val == MIR_VALUE_NONE) continue;

            // Check if this value is used after any suspend point that it dominates
            bool needs_save = false;
            // TODO: Real liveness analysis. For now, assume all non-void values might need saving.
            needs_save = true;

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

    // 3. Create state struct
    // The state struct contains:
    // - current state (i32)
    // - saved values
    MirType** field_types = malloc((values_count + 1) * sizeof(MirType*));
    int field_count = 0;

    // Field 0: state index
    field_types[field_count++] = mir_type_i32(func->parent);

    // 4. Transform original function to step function
    // For now, we'll keep the original function but change its name and signature
    char step_func_name[256];
    snprintf(step_func_name, sizeof(step_func_name), "%s_step", func->name);
    
    // Create the step function signature: (state: Ptr) -> i32
    // MirType* state_type = mir_type_i32(func->parent); // Placeholder
    // MirType* state_ptr_type = mir_type_ptr(func->parent, state_type);
    
    /*
    MirParam step_params[1];
    step_params[0].name = "state";
    step_params[0].type = state_ptr_type;
    step_params[0].value_id = mir_function_new_value(func, state_ptr_type);
    // MirValueId state_ptr = step_params[0].value_id;
    */

    // 5. Inject Loads and Stores
    // Store on Define: For each value in values_to_save, insert an 'insert' (or store) after its definition
    for (int i = 0; i < values_count; i++) {
        // MirValueId val = values_to_save[i];
        // Find definition
    }

    // Load on Resume: At the start of each resume block, insert 'extract' (or load) for needed values
    for (int s = 0; s < suspend_count; s++) {
        // MirBlock* rb = resume_blocks[s];
        // Insert loads for all values defined before this suspend and used after
    }

    // Cleanup
    free(suspend_insts);
    free(resume_blocks);
    free(values_to_save);
    free(field_types);

    return 1;
}
