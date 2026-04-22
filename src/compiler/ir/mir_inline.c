/*
 * Casprix Compiler — MIR Inlining Pass Implementation
 *
 * Function inlining on SSA-form MIR.  Clones the callee body into
 * the caller at each qualifying call site, remapping all SSA values.
 */

#include "mir_inline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ================================================================
 * Thresholds
 * ================================================================ */

MirInlineConfig mir_inline_config_for_level(int opt_level) {
    MirInlineConfig cfg;
    switch (opt_level) {
    case 0: /* -O0: no inlining */
        cfg.max_inst_count = 0;
        cfg.max_block_count = 0;
        cfg.max_call_depth = 0;
        cfg.inline_always = false;
        break;
    case 1: /* -O1: inline tiny functions only */
        cfg.max_inst_count = 8;
        cfg.max_block_count = 2;
        cfg.max_call_depth = 1;
        cfg.inline_always = false;
        break;
    case 2: /* -O2: moderate inlining */
        cfg.max_inst_count = 30;
        cfg.max_block_count = 6;
        cfg.max_call_depth = 2;
        cfg.inline_always = false;
        break;
    default: /* -O3+: aggressive */
        cfg.max_inst_count = 100;
        cfg.max_block_count = 20;
        cfg.max_call_depth = 3;
        cfg.inline_always = true;
        break;
    }
    return cfg;
}

/* ================================================================
 * Eligibility check
 * ================================================================ */

static bool is_recursive(MirFunction* func) {
    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            if (inst->opcode == MIR_CALL &&
                inst->as.call.func_name &&
                strcmp(inst->as.call.func_name, func->name) == 0) {
                return true;
            }
        }
    }
    return false;
}

static int count_insts(MirFunction* func) {
    int n = 0;
    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        n += bb->inst_count;
    }
    return n;
}

static bool is_inlinable(MirFunction* callee, MirInlineConfig config) {
    if (!callee || callee->is_extern) return false;
    if (!callee->entry_block) return false;  /* empty function */
    if (is_recursive(callee)) return false;

    int inst_count = count_insts(callee);
    if (inst_count > config.max_inst_count) return false;
    if (callee->block_count > config.max_block_count) return false;

    return true;
}

/* ================================================================
 * Value remapping table
 *
 * Maps callee SSA values → caller SSA values during cloning.
 * ================================================================ */

typedef struct {
    MirValueId*  map;       /* map[callee_val] = caller_val */
    int          capacity;
} ValueRemap;

static void remap_init(ValueRemap* r, int capacity) {
    r->capacity = capacity;
    r->map = (MirValueId*)calloc(capacity, sizeof(MirValueId));
    /* 0 = MIR_VALUE_NONE = "not yet mapped" */
}

static void remap_destroy(ValueRemap* r) {
    free(r->map);
}

static void remap_set(ValueRemap* r, MirValueId from, MirValueId to) {
    if ((int)from >= r->capacity) {
        int new_cap = r->capacity * 2;
        if ((int)from >= new_cap) new_cap = (int)from + 64;
        r->map = (MirValueId*)realloc(r->map, new_cap * sizeof(MirValueId));
        memset(r->map + r->capacity, 0,
               (new_cap - r->capacity) * sizeof(MirValueId));
        r->capacity = new_cap;
    }
    r->map[from] = to;
}

static MirValueId remap_get(ValueRemap* r, MirValueId val) {
    if (val == MIR_VALUE_NONE) return MIR_VALUE_NONE;
    if ((int)val < r->capacity && r->map[val] != MIR_VALUE_NONE) {
        return r->map[val];
    }
    return val;  /* unmapped — return as-is (shouldn't happen in correct IR) */
}

/* ================================================================
 * Block remapping table
 * ================================================================ */

typedef struct {
    MirBlock**   map;       /* map[callee_block_id] = caller_block */
    int          capacity;
} BlockRemap;

static void block_remap_init(BlockRemap* r, int capacity) {
    r->capacity = capacity;
    r->map = (MirBlock**)calloc(capacity, sizeof(MirBlock*));
}

static void block_remap_destroy(BlockRemap* r) {
    free(r->map);
}

static void block_remap_set(BlockRemap* r, int from_id, MirBlock* to) {
    if (from_id >= r->capacity) {
        int new_cap = r->capacity * 2;
        if (from_id >= new_cap) new_cap = from_id + 32;
        r->map = (MirBlock**)realloc(r->map, new_cap * sizeof(MirBlock*));
        memset(r->map + r->capacity, 0,
               (new_cap - r->capacity) * sizeof(MirBlock*));
        r->capacity = new_cap;
    }
    r->map[from_id] = to;
}

static MirBlock* block_remap_get(BlockRemap* r, int id) {
    if (id < r->capacity && r->map[id]) return r->map[id];
    return NULL;
}

/* ================================================================
 * Instruction cloning
 * ================================================================ */

static MirInst* clone_inst(MirArena* arena, MirInst* src,
                            MirFunction* caller,
                            ValueRemap* vr, BlockRemap* br,
                            MirInlineStats* stats) {
    MirInst* dst = (MirInst*)mir_arena_alloc(arena, sizeof(MirInst));
    *dst = *src;  /* shallow copy */
    dst->next = NULL;
    dst->prev = NULL;

    /* Allocate new result value in caller if the instruction defines one */
    if (src->result != MIR_VALUE_NONE) {
        MirValueId new_val = mir_function_new_value(caller, src->type);
        remap_set(vr, src->result, new_val);
        dst->result = new_val;
    }

    /* Remap operands based on opcode */
    switch (dst->opcode) {
    case MIR_ADD: case MIR_SUB: case MIR_MUL: case MIR_DIV: case MIR_MOD:
    case MIR_FADD: case MIR_FSUB: case MIR_FMUL: case MIR_FDIV:
    case MIR_BAND: case MIR_BOR: case MIR_BXOR:
    case MIR_SHL: case MIR_SHR: case MIR_USHR:
    case MIR_CMP_EQ: case MIR_CMP_NE: case MIR_CMP_LT: case MIR_CMP_LE:
    case MIR_CMP_GT: case MIR_CMP_GE:
    case MIR_LOGIC_AND: case MIR_LOGIC_OR:
        dst->as.binary.lhs = remap_get(vr, src->as.binary.lhs);
        dst->as.binary.rhs = remap_get(vr, src->as.binary.rhs);
        break;

    case MIR_NEG: case MIR_FNEG: case MIR_BNOT: case MIR_LOGIC_NOT:
    case MIR_CAST: case MIR_BITCAST: case MIR_TRUNC:
    case MIR_ZEXT: case MIR_SEXT: case MIR_SITOFP: case MIR_FPTOSI:
        dst->as.unary.operand = remap_get(vr, src->as.unary.operand);
        break;

    case MIR_LOAD:
        dst->as.mem.ptr = remap_get(vr, src->as.mem.ptr);
        break;
    case MIR_STORE:
        dst->as.mem.ptr = remap_get(vr, src->as.mem.ptr);
        dst->as.mem.value = remap_get(vr, src->as.mem.value);
        break;

    case MIR_BR: {
        MirBlock* target = block_remap_get(br, src->as.br.target->id);
        dst->as.br.target = target ? target : src->as.br.target;
        break;
    }
    case MIR_CONDBR: {
        dst->as.condbr.cond = remap_get(vr, src->as.condbr.cond);
        MirBlock* t = block_remap_get(br, src->as.condbr.true_bb->id);
        MirBlock* f = block_remap_get(br, src->as.condbr.false_bb->id);
        if (t) dst->as.condbr.true_bb = t;
        if (f) dst->as.condbr.false_bb = f;
        break;
    }

    case MIR_RET:
        dst->as.ret.value = remap_get(vr, src->as.ret.value);
        /* RET will be replaced by the caller with br to merge block */
        break;
    case MIR_RET_VOID:
        /* Will be replaced */
        break;

    case MIR_CALL: {
        /* Deep-copy args array */
        if (src->as.call.n_args > 0) {
            MirValueId* new_args = (MirValueId*)mir_arena_alloc(
                arena, src->as.call.n_args * sizeof(MirValueId));
            for (int i = 0; i < src->as.call.n_args; i++) {
                new_args[i] = remap_get(vr, src->as.call.args[i]);
            }
            dst->as.call.args = new_args;
        }
        break;
    }
    case MIR_CALL_INDIRECT: {
        dst->as.call.callee = remap_get(vr, src->as.call.callee);
        if (src->as.call.n_args > 0) {
            MirValueId* new_args = (MirValueId*)mir_arena_alloc(
                arena, src->as.call.n_args * sizeof(MirValueId));
            for (int i = 0; i < src->as.call.n_args; i++) {
                new_args[i] = remap_get(vr, src->as.call.args[i]);
            }
            dst->as.call.args = new_args;
        }
        break;
    }
    case MIR_CALL_VIRTUAL: {
        dst->as.vcall.self_obj = remap_get(vr, src->as.vcall.self_obj);
        if (src->as.vcall.n_args > 0) {
            MirValueId* new_args = (MirValueId*)mir_arena_alloc(
                arena, src->as.vcall.n_args * sizeof(MirValueId));
            for (int i = 0; i < src->as.vcall.n_args; i++) {
                new_args[i] = remap_get(vr, src->as.vcall.args[i]);
            }
            dst->as.vcall.args = new_args;
        }
        break;
    }

    case MIR_PHI: {
        if (src->as.phi.n_edges > 0) {
            MirPhiEdge* new_edges = (MirPhiEdge*)mir_arena_alloc(
                arena, src->as.phi.n_edges * sizeof(MirPhiEdge));
            for (int i = 0; i < src->as.phi.n_edges; i++) {
                new_edges[i].value = remap_get(vr, src->as.phi.edges[i].value);
                MirBlock* b = block_remap_get(br, src->as.phi.edges[i].block->id);
                new_edges[i].block = b ? b : src->as.phi.edges[i].block;
            }
            dst->as.phi.edges = new_edges;
        }
        break;
    }

    case MIR_COPY: case MIR_MOVE:
    case MIR_BORROW: case MIR_BORROW_MUT:
        dst->as.transfer.source = remap_get(vr, src->as.transfer.source);
        break;

    case MIR_ARC_RETAIN: case MIR_ARC_RELEASE: case MIR_DROP:
        dst->as.refop.ptr = remap_get(vr, src->as.refop.ptr);
        break;

    case MIR_GET_FIELD_PTR:
        dst->as.gep.base = remap_get(vr, src->as.gep.base);
        break;
    case MIR_GET_ELEM_PTR:
        dst->as.gep.base = remap_get(vr, src->as.gep.base);
        dst->as.gep.index = remap_get(vr, src->as.gep.index);
        break;

    case MIR_STRUCT_INIT: {
        if (src->as.struct_init.n_fields > 0) {
            MirValueId* new_fields = (MirValueId*)mir_arena_alloc(
                arena, src->as.struct_init.n_fields * sizeof(MirValueId));
            for (int i = 0; i < src->as.struct_init.n_fields; i++) {
                new_fields[i] = remap_get(vr, src->as.struct_init.fields[i]);
            }
            dst->as.struct_init.fields = new_fields;
        }
        break;
    }
    case MIR_EXTRACT:
        dst->as.field_op.aggregate = remap_get(vr, src->as.field_op.aggregate);
        break;
    case MIR_INSERT:
        dst->as.field_op.aggregate = remap_get(vr, src->as.field_op.aggregate);
        dst->as.field_op.insert_val = remap_get(vr, src->as.field_op.insert_val);
        break;

    case MIR_SWITCH:
        dst->as.sw.discriminant = remap_get(vr, src->as.sw.discriminant);
        if (src->as.sw.n_cases > 0) {
            MirBlock** new_targets = (MirBlock**)mir_arena_alloc(
                arena, src->as.sw.n_cases * sizeof(MirBlock*));
            for (int i = 0; i < src->as.sw.n_cases; i++) {
                MirBlock* t = block_remap_get(br, src->as.sw.targets[i]->id);
                new_targets[i] = t ? t : src->as.sw.targets[i];
            }
            dst->as.sw.targets = new_targets;
        }
        if (src->as.sw.default_bb) {
            MirBlock* d = block_remap_get(br, src->as.sw.default_bb->id);
            if (d) dst->as.sw.default_bb = d;
        }
        break;

    default:
        /* Constants, NOP, ALLOCA, etc. — no value remapping needed
         * beyond the result which was already handled. */
        break;
    }

    if (stats) stats->instructions_copied++;
    return dst;
}

/* ================================================================
 * Inline a single call site
 *
 * Caller:  ... | call_bb: ... CALL @callee ... | continue_bb: ...
 *         becomes:
 *          ... | call_bb: ... (pre-call insts) | inline_entry: ...
 *              | ... callee body ... | merge_bb: phi(ret_val) | continue: ...
 * ================================================================ */

static bool inline_call_site(MirModule* module, MirFunction* caller,
                              MirBlock* call_bb, MirInst* call_inst,
                              MirFunction* callee,
                              ValueRemap* vr, BlockRemap* br,
                              MirInlineStats* stats) {
    MirArena* arena = module->arena;

    /* ── 1. Create merge block (after the inlined code) ── */
    MirBlock* merge_bb = mir_function_add_block(caller, "inline.merge");

    /* Move all instructions after the CALL to the merge block */
    MirInst* post_call = call_inst->next;
    if (post_call) {
        merge_bb->first = post_call;
        post_call->prev = NULL;
        merge_bb->last = call_bb->last;
        call_bb->last = call_inst;
        call_inst->next = NULL;

        /* Count moved instructions */
        int moved = 0;
        for (MirInst* m = merge_bb->first; m; m = m->next) moved++;
        merge_bb->inst_count = moved;
        call_bb->inst_count -= moved;
    }

    /* ── 2. Map callee parameters → call arguments ── */
    for (int i = 0; i < callee->param_count; i++) {
        MirValueId arg = (i < call_inst->as.call.n_args)
                         ? call_inst->as.call.args[i]
                         : MIR_VALUE_NONE;
        remap_set(vr, callee->params[i].value_id, arg);
    }

    /* ── 3. Clone callee blocks into caller ── */
    /* First pass: create blocks and build block remap */
    for (MirBlock* src_bb = callee->block_list; src_bb; src_bb = src_bb->next_block) {
        char label[128];
        snprintf(label, sizeof(label), "inline.%s.%s", callee->name, src_bb->label);
        MirBlock* clone_bb = mir_function_add_block(caller, label);
        block_remap_set(br, src_bb->id, clone_bb);
        if (stats) stats->blocks_copied++;
    }

    /* ── 4. Phi node for return value (if non-void) ── */
    MirInst* ret_phi = NULL;
    if (callee->return_type && callee->return_type->kind != MIR_TYPE_VOID
        && call_inst->result != MIR_VALUE_NONE) {
        /* Create a phi node in the merge block to collect return values */
        ret_phi = (MirInst*)mir_arena_alloc(arena, sizeof(MirInst));
        ret_phi->opcode = MIR_PHI;
        ret_phi->result = call_inst->result;  /* reuse the CALL's result */
        ret_phi->type = callee->return_type;
        ret_phi->as.phi.edges = NULL;
        ret_phi->as.phi.n_edges = 0;
        ret_phi->next = NULL;
        ret_phi->prev = NULL;
        ret_phi->src_line = 0;
        ret_phi->src_col = 0;

        /* Insert at beginning of merge block */
        if (merge_bb->first) {
            ret_phi->next = merge_bb->first;
            merge_bb->first->prev = ret_phi;
        } else {
            merge_bb->last = ret_phi;
        }
        merge_bb->first = ret_phi;
        merge_bb->inst_count++;
    }

    /* ── 5. Clone instructions into new blocks ── */
    for (MirBlock* src_bb = callee->block_list; src_bb; src_bb = src_bb->next_block) {
        MirBlock* dst_bb = block_remap_get(br, src_bb->id);
        if (!dst_bb) continue;

        for (MirInst* src_inst = src_bb->first; src_inst; src_inst = src_inst->next) {
            /* Handle returns specially: replace with branch to merge */
            if (src_inst->opcode == MIR_RET) {
                if (ret_phi) {
                    /* Add phi edge: this block → return value */
                    MirValueId ret_val = remap_get(vr, src_inst->as.ret.value);
                    mir_phi_add_edge(ret_phi, dst_bb, ret_val);
                }
                /* Branch to merge block */
                MirInst* br_inst = (MirInst*)mir_arena_alloc(arena, sizeof(MirInst));
                memset(br_inst, 0, sizeof(MirInst));
                br_inst->opcode = MIR_BR;
                br_inst->result = MIR_VALUE_NONE;
                br_inst->as.br.target = merge_bb;
                mir_block_append(dst_bb, br_inst);

                /* Add CFG edge */
                mir_block_add_successor(dst_bb, merge_bb);
                mir_block_add_predecessor(merge_bb, dst_bb);
                continue;
            }
            if (src_inst->opcode == MIR_RET_VOID) {
                MirInst* br_inst = (MirInst*)mir_arena_alloc(arena, sizeof(MirInst));
                memset(br_inst, 0, sizeof(MirInst));
                br_inst->opcode = MIR_BR;
                br_inst->result = MIR_VALUE_NONE;
                br_inst->as.br.target = merge_bb;
                mir_block_append(dst_bb, br_inst);

                mir_block_add_successor(dst_bb, merge_bb);
                mir_block_add_predecessor(merge_bb, dst_bb);
                continue;
            }

            MirInst* clone = clone_inst(arena, src_inst, caller, vr, br, stats);
            mir_block_append(dst_bb, clone);

            /* Update CFG for cloned branches */
            if (clone->opcode == MIR_BR) {
                mir_block_add_successor(dst_bb, clone->as.br.target);
                mir_block_add_predecessor(clone->as.br.target, dst_bb);
            } else if (clone->opcode == MIR_CONDBR) {
                mir_block_add_successor(dst_bb, clone->as.condbr.true_bb);
                mir_block_add_successor(dst_bb, clone->as.condbr.false_bb);
                mir_block_add_predecessor(clone->as.condbr.true_bb, dst_bb);
                mir_block_add_predecessor(clone->as.condbr.false_bb, dst_bb);
            }
        }
    }

    /* ── 6. Replace the CALL instruction with a branch to inline entry ── */
    MirBlock* inline_entry = block_remap_get(br, callee->entry_block->id);

    /* Remove CALL from call_bb */
    if (call_inst->prev) call_inst->prev->next = NULL;
    else call_bb->first = NULL;
    call_bb->last = call_inst->prev;
    call_bb->inst_count--;

    /* Add branch to inline entry */
    MirInst* br_to_inline = (MirInst*)mir_arena_alloc(arena, sizeof(MirInst));
    memset(br_to_inline, 0, sizeof(MirInst));
    br_to_inline->opcode = MIR_BR;
    br_to_inline->result = MIR_VALUE_NONE;
    br_to_inline->as.br.target = inline_entry;
    mir_block_append(call_bb, br_to_inline);

    /* Update CFG.
     *
     * Before inlining call_bb had the successors that belonged to whatever
     * terminator sat after the CALL (those instructions are now in merge_bb).
     * We must:
     *   1. Transfer those edges to merge_bb (so its successors' pred lists
     *      point at merge_bb, not at call_bb).
     *   2. Clear call_bb's successor list and point it at inline_entry only.
     */
    for (int i = 0; i < call_bb->succ_count; i++) {
        MirBlock* old_succ = call_bb->successors[i];
        /* Replace call_bb in old_succ's predecessor list with merge_bb. */
        for (int j = 0; j < old_succ->pred_count; j++) {
            if (old_succ->predecessors[j] == call_bb) {
                old_succ->predecessors[j] = merge_bb;
                break;
            }
        }
        /* Also redirect phi edges: any phi in old_succ that referenced
         * call_bb should now reference merge_bb. */
        for (MirInst* phi = old_succ->first;
             phi && phi->opcode == MIR_PHI;
             phi = phi->next) {
            for (int k = 0; k < phi->as.phi.n_edges; k++) {
                if (phi->as.phi.edges[k].block == call_bb)
                    phi->as.phi.edges[k].block = merge_bb;
            }
        }
        mir_block_add_successor(merge_bb, old_succ);
    }
    call_bb->succ_count = 0;

    mir_block_add_successor(call_bb, inline_entry);
    mir_block_add_predecessor(inline_entry, call_bb);

    /* If the return phi ended up with zero edges (callee is a noreturn
     * function with no RET on any path), lower the call result to
     * MIR_UNREACHABLE instead of leaving a broken 0-edge phi. */
    if (ret_phi && ret_phi->as.phi.n_edges == 0) {
        ret_phi->opcode = MIR_UNREACHABLE;
        ret_phi->result = MIR_VALUE_NONE;
    }

    if (stats) stats->calls_inlined++;
    return true;
}

/* ================================================================
 * Per-function inlining
 * ================================================================ */

int mir_inline_function(MirModule* module, MirFunction* caller,
                         MirInlineConfig config, MirInlineStats* stats) {
    if (!caller || caller->is_extern || !caller->entry_block) return 0;
    if (config.max_inst_count <= 0) return 0;

    MirInlineStats local_stats = {0};
    if (!stats) stats = &local_stats;

    int total_inlined = 0;
    bool changed = true;

    /* Iterate: inlining may expose more inlining opportunities */
    for (int pass = 0; pass < config.max_call_depth && changed; pass++) {
        changed = false;

        for (MirBlock* bb = caller->block_list; bb; bb = bb->next_block) {
            for (MirInst* inst = bb->first; inst; inst = inst->next) {
                if (inst->opcode != MIR_CALL) continue;
                if (!inst->as.call.func_name) continue;

                /* Don't inline self-recursion */
                if (strcmp(inst->as.call.func_name, caller->name) == 0) continue;

                /* Find callee in module */
                MirFunction* callee = mir_module_find_function(
                    module, inst->as.call.func_name);
                if (!callee) continue;
                if (!is_inlinable(callee, config)) continue;

                /* Inline this call site */
                ValueRemap vr;
                remap_init(&vr, callee->next_value_id + 32);
                BlockRemap br;
                block_remap_init(&br, callee->block_count + 8);

                bool ok = inline_call_site(module, caller, bb, inst,
                                           callee, &vr, &br, stats);

                remap_destroy(&vr);
                block_remap_destroy(&br);

                if (ok) {
                    total_inlined++;
                    changed = true;
                    goto restart;  /* restart iteration — block list changed */
                }
            }
        }
        restart:;
    }

    return total_inlined;
}

/* ================================================================
 * Module-level inlining
 * ================================================================ */

void mir_inline_module(MirModule* module, int opt_level, MirInlineStats* stats) {
    MirInlineStats total = {0};
    if (!stats) stats = &total;

    MirInlineConfig config = mir_inline_config_for_level(opt_level);
    if (config.max_inst_count <= 0) return;  /* inlining disabled */

    for (MirFunction* func = module->func_list; func; func = func->next_func) {
        if (func->is_extern) continue;
        mir_inline_function(module, func, config, stats);
    }
}
