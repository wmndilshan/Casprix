/*
 * Casprix Compiler — MIR mem2reg Pass Implementation
 *
 * Promotes alloca/load/store to SSA form with phi nodes.
 * Implements the classic Cytron et al. (1991) algorithm with the
 * Cooper–Harvey–Kennedy dominator tree construction.
 *
 * This is the most important pass in the entire pipeline: without it
 * every variable is an alloca → all loads/stores go through memory →
 * no other SSA optimisation can see through the indirection.
 */

#include "mir_mem2reg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ================================================================
 * Limits
 * ================================================================ */
#define MAX_BLOCKS     4096
#define MAX_ALLOCAS    1024

/* ================================================================
 * Block ordering — RPO (Reverse Post-Order)
 *
 * Used by dominator computation.  We do a DFS from the entry block
 * and record blocks in post-order, then reverse.
 * ================================================================ */

typedef struct {
    MirBlock*  blocks[MAX_BLOCKS];
    int        count;
    bool       visited[MAX_BLOCKS];
} BlockOrder;

static void dfs_post_order(MirBlock* bb, BlockOrder* order) {
    if (!bb || order->visited[bb->id]) return;
    order->visited[bb->id] = true;

    for (int i = 0; i < bb->succ_count; i++) {
        dfs_post_order(bb->successors[i], order);
    }

    assert(order->count < MAX_BLOCKS);
    order->blocks[order->count++] = bb;
}

static void compute_rpo(MirFunction* func, BlockOrder* rpo) {
    memset(rpo, 0, sizeof(*rpo));

    /* DFS from entry → post-order */
    dfs_post_order(func->entry_block, rpo);

    /* Reverse in-place to get RPO */
    for (int i = 0; i < rpo->count / 2; i++) {
        MirBlock* tmp = rpo->blocks[i];
        rpo->blocks[i] = rpo->blocks[rpo->count - 1 - i];
        rpo->blocks[rpo->count - 1 - i] = tmp;
    }
}

/* ================================================================
 * Dominator Tree — Cooper–Harvey–Kennedy iterative algorithm
 *
 * Simple, fast (O(n^2) worst-case but practically linear for
 * reducible CFGs), and easy to implement correctly.
 * ================================================================ */

/* Per-invocation RPO-index context (no global state → re-entrant). */
typedef struct {
    int data[MAX_BLOCKS];
} RpoIndexMap;

static MirBlock* intersect(MirBlock* b1, MirBlock* b2,
                            const RpoIndexMap* rpo_map) {
    while (b1 != b2) {
        while (rpo_map->data[b1->id] > rpo_map->data[b2->id])
            b1 = b1->idom;
        while (rpo_map->data[b2->id] > rpo_map->data[b1->id])
            b2 = b2->idom;
    }
    return b1;
}

void mir_compute_dominators(MirFunction* func) {
    BlockOrder rpo;
    compute_rpo(func, &rpo);
    if (rpo.count == 0) return;

    /* Build RPO index map — stack-allocated, not a global. */
    RpoIndexMap rpo_map;
    memset(&rpo_map, -1, sizeof(rpo_map));
    for (int i = 0; i < rpo.count; i++) {
        rpo_map.data[rpo.blocks[i]->id] = i;
    }

    /* Initialize: entry dominates itself, all others undefined */
    for (int i = 0; i < rpo.count; i++) {
        rpo.blocks[i]->idom = NULL;
    }
    rpo.blocks[0]->idom = rpo.blocks[0];   /* entry → self */

    /* Iterate until fixed point */
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 1; i < rpo.count; i++) {   /* skip entry (index 0) */
            MirBlock* bb = rpo.blocks[i];

            /* Find first processed predecessor */
            MirBlock* new_idom = NULL;
            for (int p = 0; p < bb->pred_count; p++) {
                MirBlock* pred = bb->predecessors[p];
                if (pred->idom != NULL) {
                    new_idom = pred;
                    break;
                }
            }
            if (!new_idom) continue;

            /* Intersect with remaining processed predecessors */
            for (int p = 0; p < bb->pred_count; p++) {
                MirBlock* pred = bb->predecessors[p];
                if (pred == new_idom) continue;
                if (pred->idom != NULL) {
                    new_idom = intersect(pred, new_idom, &rpo_map);
                }
            }

            if (bb->idom != new_idom) {
                bb->idom = new_idom;
                changed = true;
            }
        }
    }

    /* Build dominator tree children lists */
    for (int i = 0; i < rpo.count; i++) {
        rpo.blocks[i]->dom_child_count = 0;
    }

    /* Count children */
    for (int i = 1; i < rpo.count; i++) {
        MirBlock* bb = rpo.blocks[i];
        if (bb->idom && bb->idom != bb) {
            bb->idom->dom_child_count++;
        }
    }

    /* Allocate children arrays */
    MirArena* arena = func->parent->arena;
    for (int i = 0; i < rpo.count; i++) {
        MirBlock* bb = rpo.blocks[i];
        if (bb->dom_child_count > 0) {
            bb->dom_children = (MirBlock**)mir_arena_alloc(
                arena, bb->dom_child_count * sizeof(MirBlock*));
        }
        bb->dom_child_count = 0;   /* reset for filling */
    }

    /* Fill children arrays */
    for (int i = 1; i < rpo.count; i++) {
        MirBlock* bb = rpo.blocks[i];
        if (bb->idom && bb->idom != bb) {
            MirBlock* parent = bb->idom;
            parent->dom_children[parent->dom_child_count++] = bb;
        }
    }
}

/* ================================================================
 * Dominance Frontiers
 *
 * DF(B) = { Y : ∃ pred P of Y s.t. B dominates P but B does NOT
 *           strictly dominate Y }
 *
 * Uses the classic bottom-up algorithm from Cytron et al.
 * ================================================================ */

void mir_compute_dom_frontiers(MirFunction* func) {
    /* Reset all frontiers */
    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        bb->dom_frontier_count = 0;
        bb->dom_frontier = NULL;
    }

    MirArena* arena = func->parent->arena;

    /* Temporary: collect frontier sets as dynamic arrays */
    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        if (bb->pred_count < 2) continue;  /* DF only at join points */

        for (int p = 0; p < bb->pred_count; p++) {
            MirBlock* runner = bb->predecessors[p];
            while (runner != bb->idom) {
                /* Add bb to DF(runner) if not already there */
                bool found = false;
                for (int d = 0; d < runner->dom_frontier_count; d++) {
                    if (runner->dom_frontier[d] == bb) { found = true; break; }
                }
                if (!found) {
                    /* Grow frontier array */
                    int new_count = runner->dom_frontier_count + 1;
                    MirBlock** new_df = (MirBlock**)mir_arena_alloc(
                        arena, new_count * sizeof(MirBlock*));
                    if (runner->dom_frontier) {
                        memcpy(new_df, runner->dom_frontier,
                               runner->dom_frontier_count * sizeof(MirBlock*));
                    }
                    new_df[runner->dom_frontier_count] = bb;
                    runner->dom_frontier = new_df;
                    runner->dom_frontier_count = new_count;
                }
                runner = runner->idom;
                if (!runner) break;   /* safety: unreachable block */
            }
        }
    }
}

/* ================================================================
 * Promotability check
 *
 * An alloca is promotable if:
 *   1. It allocates a single scalar value (not an array, not aggregate).
 *   2. All uses are direct loads or stores through the alloca pointer.
 *   3. The alloca pointer is never passed to a call, used in GEP,
 *      or stored to another pointer (i.e. no address escaping).
 * ================================================================ */

typedef struct {
    MirValueId  alloca_val;         /* SSA value produced by the alloca */
    MirType*    alloc_type;         /* the promoted type */
    MirBlock**  def_blocks;         /* blocks containing stores to this alloca */
    int         def_block_count;
    int         def_block_cap;
} PromotableAlloca;

static bool is_promotable(MirFunction* func, MirInst* alloca_inst,
                           PromotableAlloca* info) {
    MirValueId ptr = alloca_inst->result;
    if (alloca_inst->as.alloca.count != 1) return false;  /* array alloca */

    MirType* type = alloca_inst->as.alloca.alloc_type;
    if (!type) return false;
    /* Only promote scalar types (int, float, bool, ptr) */
    if (type->kind == MIR_TYPE_STRUCT || type->kind == MIR_TYPE_ARRAY ||
        type->kind == MIR_TYPE_AGGREGATE || type->kind == MIR_TYPE_SLICE) {
        return false;
    }

    /* Scan all uses — must be only LOAD or STORE with ptr operand == alloca */
    info->alloca_val = ptr;
    info->alloc_type = type;
    info->def_blocks = NULL;
    info->def_block_count = 0;
    info->def_block_cap = 0;

    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            if (inst == alloca_inst) continue;

            switch (inst->opcode) {
            case MIR_LOAD:
                if (inst->as.mem.ptr == ptr) {
                    /* OK — direct load from alloca */
                }
                break;

            case MIR_STORE:
                if (inst->as.mem.ptr == ptr) {
                    /* Record this block as a def-block for the alloca */
                    bool already = false;
                    for (int i = 0; i < info->def_block_count; i++) {
                        if (info->def_blocks[i] == bb) { already = true; break; }
                    }
                    if (!already) {
                        if (info->def_block_count >= info->def_block_cap) {
                            info->def_block_cap = info->def_block_cap
                                                  ? info->def_block_cap * 2 : 8;
                            info->def_blocks = (MirBlock**)realloc(
                                info->def_blocks,
                                info->def_block_cap * sizeof(MirBlock*));
                        }
                        info->def_blocks[info->def_block_count++] = bb;
                    }
                } else if (inst->as.mem.value == ptr) {
                    /* alloca address stored to another pointer → escapes */
                    return false;
                }
                break;

            case MIR_CALL:
                /* Check if alloca ptr passed as argument */
                for (int a = 0; a < inst->as.call.n_args; a++) {
                    if (inst->as.call.args[a] == ptr) return false;
                }
                break;

            case MIR_CALL_INDIRECT:
                for (int a = 0; a < inst->as.call.n_args; a++) {
                    if (inst->as.call.args[a] == ptr) return false;
                }
                if (inst->as.call.callee == ptr) return false;
                break;

            case MIR_CALL_VIRTUAL:
                for (int a = 0; a < inst->as.vcall.n_args; a++) {
                    if (inst->as.vcall.args[a] == ptr) return false;
                }
                if (inst->as.vcall.self_obj == ptr) return false;
                break;

            case MIR_GET_FIELD_PTR:
            case MIR_GET_ELEM_PTR:
                if (inst->as.gep.base == ptr) return false;  /* address arithmetic */
                break;

            case MIR_BORROW:
            case MIR_BORROW_MUT:
                if (inst->as.transfer.source == ptr) return false;
                break;

            /* Struct/aggregate construction — ptr stored into an aggregate field */
            case MIR_STRUCT_INIT:
                for (int fi = 0; fi < inst->as.struct_init.n_fields; fi++) {
                    if (inst->as.struct_init.fields[fi] == ptr) return false;
                }
                break;
            case MIR_INSERT:
                if (inst->as.field_op.insert_val == ptr) return false;
                if (inst->as.field_op.aggregate == ptr)  return false;
                break;
            case MIR_EXTRACT:
                if (inst->as.field_op.aggregate == ptr) return false;
                break;

            /* SIMD vector ops — any operand slot could carry the alloca addr */
            case MIR_VEC_LOAD: case MIR_VEC_LOAD_UNALIGNED:
            case MIR_VEC_STORE: case MIR_VEC_STORE_UNALIGNED:
            case MIR_VEC_BROADCAST: case MIR_VEC_REDUCE_SUM: case MIR_VEC_DOT:
            case MIR_VEC_ADD: case MIR_VEC_SUB: case MIR_VEC_MUL: case MIR_VEC_DIV:
            case MIR_VEC_MIN: case MIR_VEC_MAX:
            case MIR_VEC_AND: case MIR_VEC_OR: case MIR_VEC_XOR:
            case MIR_VEC_FMA:
            case MIR_VEC_CMP_EQ: case MIR_VEC_CMP_LT: case MIR_VEC_CMP_GT:
            case MIR_VEC_SELECT:
                if (inst->as.vec.a == ptr || inst->as.vec.b == ptr ||
                    inst->as.vec.c == ptr) return false;
                break;

            /* Coroutine suspend — captures the entire stack frame */
            case MIR_SUSPEND:
                return false;

            default:
                /* Conservative: any opcode not explicitly handled above might
                 * capture the alloca address. Check the common scalar operand
                 * fields; if none match, allow it (the opcode doesn't refer to
                 * pointer-typed values). For unknown opcodes that DO use ptr,
                 * the inner check will reject. */
                {
                    bool uses = false;
                    switch (inst->opcode) {
                    case MIR_ADD: case MIR_SUB: case MIR_MUL: case MIR_DIV:
                    case MIR_MOD: case MIR_FADD: case MIR_FSUB: case MIR_FMUL:
                    case MIR_FDIV: case MIR_BAND: case MIR_BOR: case MIR_BXOR:
                    case MIR_SHL: case MIR_SHR: case MIR_USHR:
                    case MIR_CMP_EQ: case MIR_CMP_NE: case MIR_CMP_LT:
                    case MIR_CMP_LE: case MIR_CMP_GT: case MIR_CMP_GE:
                    case MIR_LOGIC_AND: case MIR_LOGIC_OR:
                        uses = (inst->as.binary.lhs == ptr ||
                                inst->as.binary.rhs == ptr);
                        break;
                    case MIR_NEG: case MIR_FNEG: case MIR_BNOT:
                    case MIR_LOGIC_NOT: case MIR_CAST: case MIR_BITCAST:
                    case MIR_TRUNC: case MIR_ZEXT: case MIR_SEXT:
                    case MIR_SITOFP: case MIR_FPTOSI:
                        uses = (inst->as.unary.operand == ptr);
                        break;
                    case MIR_CONDBR:
                        uses = (inst->as.condbr.cond == ptr);
                        break;
                    case MIR_RET:
                        uses = (inst->as.ret.value == ptr);
                        break;
                    case MIR_COPY: case MIR_MOVE:
                        uses = (inst->as.transfer.source == ptr);
                        break;
                    case MIR_ARC_RETAIN: case MIR_ARC_RELEASE: case MIR_DROP:
                        uses = (inst->as.refop.ptr == ptr);
                        break;
                    /* Truly data-free opcodes — cannot reference ptr */
                    case MIR_NOP: case MIR_UNDEF: case MIR_DEBUGLOC:
                    case MIR_BR: case MIR_RET_VOID: case MIR_UNREACHABLE:
                    case MIR_CONST_INT: case MIR_CONST_FLOAT:
                    case MIR_CONST_BOOL: case MIR_CONST_STRING:
                    case MIR_CONST_NULL: case MIR_CONST_FUNC:
                    case MIR_ALLOCA: case MIR_PHI:
                        uses = false;
                        break;
                    default:
                        /* Truly unknown opcode: conservatively reject. */
                        return false;
                    }
                    if (uses) return false;
                }
                break;
            }
        }
    }

    return true;
}

/* ================================================================
 * Phi insertion — iterated dominance frontier
 *
 * For each promotable alloca:
 *   – def_blocks = blocks that contain a store to the alloca
 *   – Insert phi at DF+(def_blocks) using a work-list algorithm
 * ================================================================ */

/* Insert a phi node at the beginning of a block (after any existing phis). */
static MirInst* insert_phi(MirFunction* func, MirBlock* bb, MirType* type) {
    MirArena* arena = func->parent->arena;
    MirInst* phi = (MirInst*)mir_arena_alloc(arena, sizeof(MirInst));
    phi->opcode = MIR_PHI;
    phi->result = mir_function_new_value(func, type);
    phi->type = type;
    phi->as.phi.edges = NULL;
    phi->as.phi.n_edges = 0;
    phi->src_line = 0;
    phi->src_col = 0;

    /* Insert at the beginning of the block (before first non-phi inst) */
    MirInst* insert_after = NULL;
    for (MirInst* inst = bb->first; inst; inst = inst->next) {
        if (inst->opcode == MIR_PHI) {
            insert_after = inst;
        } else {
            break;
        }
    }

    if (insert_after) {
        /* After last existing phi */
        phi->next = insert_after->next;
        phi->prev = insert_after;
        if (insert_after->next) insert_after->next->prev = phi;
        else bb->last = phi;
        insert_after->next = phi;
    } else {
        /* At the very beginning */
        phi->next = bb->first;
        phi->prev = NULL;
        if (bb->first) bb->first->prev = phi;
        else bb->last = phi;
        bb->first = phi;
    }
    bb->inst_count++;

    return phi;
}

/* ================================================================
 * Variable renaming — dominator tree DFS
 *
 * For each promotable alloca we maintain a stack of SSA values.
 * As we walk the dominator tree:
 *   – STORE to alloca → push the stored value onto the stack.
 *   – LOAD from alloca → replace with TOS (current reaching def).
 *   – PHI at successor → fill the incoming edge for the current block.
 *   – On backtrack, pop all values pushed in this subtree.
 * ================================================================ */

typedef struct {
    MirValueId* stack;
    int         top;
    int         capacity;
} ValueStack;

static void vs_init(ValueStack* vs) {
    vs->capacity = 16;
    vs->stack = (MirValueId*)malloc(vs->capacity * sizeof(MirValueId));
    vs->top = -1;
}

static void vs_push(ValueStack* vs, MirValueId val) {
    if (vs->top + 1 >= vs->capacity) {
        vs->capacity *= 2;
        vs->stack = (MirValueId*)realloc(vs->stack,
                     vs->capacity * sizeof(MirValueId));
    }
    vs->stack[++vs->top] = val;
}

static MirValueId vs_top(ValueStack* vs) {
    return (vs->top >= 0) ? vs->stack[vs->top] : MIR_VALUE_NONE;
}

static void vs_destroy(ValueStack* vs) {
    free(vs->stack);
}

/* Remove an instruction from its block's linked list. */
static void unlink_inst(MirBlock* bb, MirInst* inst) {
    if (inst->prev) inst->prev->next = inst->next;
    else bb->first = inst->next;

    if (inst->next) inst->next->prev = inst->prev;
    else bb->last = inst->prev;

    bb->inst_count--;
    inst->next = NULL;
    inst->prev = NULL;
}

/* Per-alloca context during renaming */
typedef struct {
    MirValueId      alloca_val;     /* the alloca SSA value (ptr) */
    ValueStack      stack;          /* current reaching definition */
    /* Block → MirInst* (phi node) map.
     * We use a flat array indexed by block id. */
    MirInst**       phi_for_block;
    int             phi_map_cap;
} AllocaRenameCtx;

static void rename_block(MirFunction* func,
                          MirBlock* bb,
                          AllocaRenameCtx* alloca_ctxs,
                          int n_allocas,
                          MirMem2RegStats* stats);

static void rename_block(MirFunction* func,
                          MirBlock* bb,
                          AllocaRenameCtx* alloca_ctxs,
                          int n_allocas,
                          MirMem2RegStats* stats) {
    /* Remember stack depths so we can pop on backtrack */
    int* saved_tops = NULL;
    if (n_allocas > 0) {
        saved_tops = (int*)malloc(n_allocas * sizeof(int));
        for (int a = 0; a < n_allocas; a++) {
            saved_tops[a] = alloca_ctxs[a].stack.top;
        }
    }

    /* Process phi nodes in this block — they define new values */
    for (int a = 0; a < n_allocas; a++) {
        MirInst* phi = NULL;
        if (bb->id < (uint32_t)alloca_ctxs[a].phi_map_cap) {
            phi = alloca_ctxs[a].phi_for_block[bb->id];
        }
        if (phi) {
            vs_push(&alloca_ctxs[a].stack, phi->result);
        }
    }

    /* Walk instructions, replacing loads and removing stores */
    MirInst* inst = bb->first;
    while (inst) {
        MirInst* next = inst->next;

        if (inst->opcode == MIR_LOAD) {
            /* Check if this loads from a promotable alloca */
            for (int a = 0; a < n_allocas; a++) {
                if (inst->as.mem.ptr == alloca_ctxs[a].alloca_val) {
                    /* Replace all uses of this load's result with TOS */
                    MirValueId replacement = vs_top(&alloca_ctxs[a].stack);
                    MirValueId load_result = inst->result;

                    /* Walk all instructions in the function and replace
                     * uses of load_result with replacement. */
                    for (MirBlock* rbb = func->block_list; rbb; rbb = rbb->next_block) {
                        for (MirInst* ri = rbb->first; ri; ri = ri->next) {
                            if (ri == inst) continue;
                            /* Use the exhaustive replacement from mir_opt */
                            switch (ri->opcode) {
                            case MIR_ADD: case MIR_SUB: case MIR_MUL:
                            case MIR_DIV: case MIR_MOD:
                            case MIR_FADD: case MIR_FSUB: case MIR_FMUL:
                            case MIR_FDIV:
                            case MIR_BAND: case MIR_BOR: case MIR_BXOR:
                            case MIR_SHL: case MIR_SHR: case MIR_USHR:
                            case MIR_CMP_EQ: case MIR_CMP_NE:
                            case MIR_CMP_LT: case MIR_CMP_LE:
                            case MIR_CMP_GT: case MIR_CMP_GE:
                            case MIR_LOGIC_AND: case MIR_LOGIC_OR:
                                if (ri->as.binary.lhs == load_result)
                                    ri->as.binary.lhs = replacement;
                                if (ri->as.binary.rhs == load_result)
                                    ri->as.binary.rhs = replacement;
                                break;
                            case MIR_NEG: case MIR_FNEG: case MIR_BNOT:
                            case MIR_LOGIC_NOT:
                            case MIR_CAST: case MIR_BITCAST: case MIR_TRUNC:
                            case MIR_ZEXT: case MIR_SEXT:
                            case MIR_SITOFP: case MIR_FPTOSI:
                                if (ri->as.unary.operand == load_result)
                                    ri->as.unary.operand = replacement;
                                break;
                            case MIR_LOAD:
                                if (ri->as.mem.ptr == load_result)
                                    ri->as.mem.ptr = replacement;
                                break;
                            case MIR_STORE:
                                if (ri->as.mem.ptr == load_result)
                                    ri->as.mem.ptr = replacement;
                                if (ri->as.mem.value == load_result)
                                    ri->as.mem.value = replacement;
                                break;
                            case MIR_CONDBR:
                                if (ri->as.condbr.cond == load_result)
                                    ri->as.condbr.cond = replacement;
                                break;
                            case MIR_RET:
                                if (ri->as.ret.value == load_result)
                                    ri->as.ret.value = replacement;
                                break;
                            case MIR_CALL:
                                for (int ca = 0; ca < ri->as.call.n_args; ca++) {
                                    if (ri->as.call.args[ca] == load_result)
                                        ri->as.call.args[ca] = replacement;
                                }
                                break;
                            case MIR_CALL_INDIRECT:
                                if (ri->as.call.callee == load_result)
                                    ri->as.call.callee = replacement;
                                for (int ca = 0; ca < ri->as.call.n_args; ca++) {
                                    if (ri->as.call.args[ca] == load_result)
                                        ri->as.call.args[ca] = replacement;
                                }
                                break;
                            case MIR_CALL_VIRTUAL:
                                if (ri->as.vcall.self_obj == load_result)
                                    ri->as.vcall.self_obj = replacement;
                                for (int ca = 0; ca < ri->as.vcall.n_args; ca++) {
                                    if (ri->as.vcall.args[ca] == load_result)
                                        ri->as.vcall.args[ca] = replacement;
                                }
                                break;
                            case MIR_COPY: case MIR_MOVE:
                            case MIR_BORROW: case MIR_BORROW_MUT:
                                if (ri->as.transfer.source == load_result)
                                    ri->as.transfer.source = replacement;
                                break;
                            case MIR_ARC_RETAIN: case MIR_ARC_RELEASE:
                            case MIR_DROP:
                                if (ri->as.refop.ptr == load_result)
                                    ri->as.refop.ptr = replacement;
                                break;
                            case MIR_PHI:
                                for (int e = 0; e < ri->as.phi.n_edges; e++) {
                                    if (ri->as.phi.edges[e].value == load_result)
                                        ri->as.phi.edges[e].value = replacement;
                                }
                                break;
                            case MIR_SWITCH:
                                if (ri->as.sw.discriminant == load_result)
                                    ri->as.sw.discriminant = replacement;
                                break;
                            case MIR_STRUCT_INIT:
                                for (int fi = 0; fi < ri->as.struct_init.n_fields; fi++) {
                                    if (ri->as.struct_init.fields[fi] == load_result)
                                        ri->as.struct_init.fields[fi] = replacement;
                                }
                                break;
                            case MIR_EXTRACT:
                                if (ri->as.field_op.aggregate == load_result)
                                    ri->as.field_op.aggregate = replacement;
                                break;
                            case MIR_INSERT:
                                if (ri->as.field_op.aggregate == load_result)
                                    ri->as.field_op.aggregate = replacement;
                                if (ri->as.field_op.insert_val == load_result)
                                    ri->as.field_op.insert_val = replacement;
                                break;
                            case MIR_GET_FIELD_PTR:
                            case MIR_GET_ELEM_PTR:
                                if (ri->as.gep.base == load_result)
                                    ri->as.gep.base = replacement;
                                if (ri->opcode == MIR_GET_ELEM_PTR &&
                                    ri->as.gep.index == load_result)
                                    ri->as.gep.index = replacement;
                                break;
                            /* SIMD vector operands */
                            case MIR_VEC_LOAD: case MIR_VEC_LOAD_UNALIGNED:
                            case MIR_VEC_STORE: case MIR_VEC_STORE_UNALIGNED:
                            case MIR_VEC_BROADCAST: case MIR_VEC_REDUCE_SUM:
                            case MIR_VEC_DOT:
                            case MIR_VEC_ADD: case MIR_VEC_SUB:
                            case MIR_VEC_MUL: case MIR_VEC_DIV:
                            case MIR_VEC_MIN: case MIR_VEC_MAX:
                            case MIR_VEC_AND: case MIR_VEC_OR: case MIR_VEC_XOR:
                            case MIR_VEC_FMA:
                            case MIR_VEC_CMP_EQ: case MIR_VEC_CMP_LT:
                            case MIR_VEC_CMP_GT: case MIR_VEC_SELECT:
                                if (ri->as.vec.a == load_result)
                                    ri->as.vec.a = replacement;
                                if (ri->as.vec.b == load_result)
                                    ri->as.vec.b = replacement;
                                if (ri->as.vec.c == load_result)
                                    ri->as.vec.c = replacement;
                                break;
                            default:
                                break;
                            }
                        }
                    }

                    /* Remove the load instruction */
                    unlink_inst(bb, inst);
                    if (stats) stats->loads_eliminated++;
                    break;
                }
            }
        }
        else if (inst->opcode == MIR_STORE) {
            /* Check if this stores to a promotable alloca */
            for (int a = 0; a < n_allocas; a++) {
                if (inst->as.mem.ptr == alloca_ctxs[a].alloca_val) {
                    /* Push the stored value as the new reaching definition */
                    vs_push(&alloca_ctxs[a].stack, inst->as.mem.value);

                    /* Remove the store instruction */
                    unlink_inst(bb, inst);
                    if (stats) stats->stores_eliminated++;
                    break;
                }
            }
        }

        inst = next;
    }

    /* Fill phi incoming edges in successor blocks */
    for (int s = 0; s < bb->succ_count; s++) {
        MirBlock* succ = bb->successors[s];
        for (int a = 0; a < n_allocas; a++) {
            MirInst* phi = NULL;
            if (succ->id < (uint32_t)alloca_ctxs[a].phi_map_cap) {
                phi = alloca_ctxs[a].phi_for_block[succ->id];
            }
            if (phi) {
                MirValueId val = vs_top(&alloca_ctxs[a].stack);
                /* Add edge: (bb → val) */
                mir_phi_add_edge(phi, bb, val);
            }
        }
    }

    /* Recurse into dominator-tree children */
    for (int c = 0; c < bb->dom_child_count; c++) {
        rename_block(func, bb->dom_children[c],
                     alloca_ctxs, n_allocas, stats);
    }

    /* Restore stack depths */
    for (int a = 0; a < n_allocas; a++) {
        alloca_ctxs[a].stack.top = saved_tops[a];
    }
    free(saved_tops);
}

/* ================================================================
 * Main mem2reg entry point
 * ================================================================ */

int mir_mem2reg(MirFunction* func, MirMem2RegStats* stats) {
    if (!func || !func->entry_block) return 0;
    (void)mir_validate_function(func);

    MirMem2RegStats local_stats = {0};
    if (!stats) stats = &local_stats;

    /* ── Step 1: Find all promotable allocas ── */
    PromotableAlloca promotable[MAX_ALLOCAS];
    int n_promotable = 0;

    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            if (inst->opcode != MIR_ALLOCA) continue;
            if (n_promotable >= MAX_ALLOCAS) break;

            PromotableAlloca info;
            if (is_promotable(func, inst, &info)) {
                promotable[n_promotable++] = info;
            }
        }
    }

    if (n_promotable == 0) return 0;

    /* ── Step 2: Compute dominators and dominance frontiers ── */
    mir_compute_dominators(func);
    mir_compute_dom_frontiers(func);

    /* ── Step 3: Insert phi nodes at iterated dominance frontiers ── */

    /* Build rename context for each alloca */
    AllocaRenameCtx* alloca_ctxs = (AllocaRenameCtx*)calloc(
        n_promotable, sizeof(AllocaRenameCtx));

    for (int a = 0; a < n_promotable; a++) {
        alloca_ctxs[a].alloca_val = promotable[a].alloca_val;
        vs_init(&alloca_ctxs[a].stack);
        alloca_ctxs[a].phi_map_cap = func->block_count;
        alloca_ctxs[a].phi_for_block = (MirInst**)calloc(
            func->block_count, sizeof(MirInst*));

        /* Work-list: IDF computation */
        bool* has_phi = (bool*)calloc(func->block_count, sizeof(bool));
        bool* in_worklist = (bool*)calloc(func->block_count, sizeof(bool));

        /* Seed work-list with def-blocks */
        MirBlock** worklist = (MirBlock**)malloc(
            func->block_count * sizeof(MirBlock*));
        int wl_count = 0;

        for (int d = 0; d < promotable[a].def_block_count; d++) {
            MirBlock* db = promotable[a].def_block_count > 0
                           ? promotable[a].def_blocks[d] : NULL;
            if (db && !in_worklist[db->id]) {
                worklist[wl_count++] = db;
                in_worklist[db->id] = true;
            }
        }

        /* Iterate: for each block in worklist, add DF(block) to phi set */
        while (wl_count > 0) {
            MirBlock* bb = worklist[--wl_count];

            for (int df = 0; df < bb->dom_frontier_count; df++) {
                MirBlock* dfb = bb->dom_frontier[df];
                if (!has_phi[dfb->id]) {
                    has_phi[dfb->id] = true;

                    /* Insert phi node */
                    MirInst* phi = insert_phi(func, dfb, promotable[a].alloc_type);
                    alloca_ctxs[a].phi_for_block[dfb->id] = phi;
                    stats->phis_inserted++;

                    /* Add DF block to worklist if not already there */
                    if (!in_worklist[dfb->id]) {
                        worklist[wl_count++] = dfb;
                        in_worklist[dfb->id] = true;
                    }
                }
            }
        }

        free(worklist);
        free(has_phi);
        free(in_worklist);
    }

    /* ── Step 4: Rename variables via dominator tree DFS ── */

    /* Push explicit MIR_UNDEF values as initial reaching definitions so that
     * uninitialised loads receive a valid (though undefined) SSA value rather
     * than MIR_VALUE_NONE, which would crash downstream passes. */
    for (int a = 0; a < n_promotable; a++) {
        MirInst* undef_inst = (MirInst*)mir_arena_alloc(
            func->parent->arena, sizeof(MirInst));
        memset(undef_inst, 0, sizeof(MirInst));
        undef_inst->opcode = MIR_UNDEF;
        undef_inst->type   = promotable[a].alloc_type;
        undef_inst->result = mir_function_new_value(func, promotable[a].alloc_type);
        /* Insert at the very start of the entry block (before any phis). */
        MirBlock* entry = func->entry_block;
        undef_inst->next = entry->first;
        undef_inst->prev = NULL;
        if (entry->first) entry->first->prev = undef_inst;
        else              entry->last         = undef_inst;
        entry->first = undef_inst;
        entry->inst_count++;
        vs_push(&alloca_ctxs[a].stack, undef_inst->result);
    }

    rename_block(func, func->entry_block, alloca_ctxs, n_promotable, stats);

    /* ── Step 5: Delete the alloca instructions ── */
    for (int a = 0; a < n_promotable; a++) {
        MirValueId alloca_val = promotable[a].alloca_val;

        for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
            for (MirInst* inst = bb->first; inst; inst = inst->next) {
                if (inst->opcode == MIR_ALLOCA && inst->result == alloca_val) {
                    unlink_inst(bb, inst);
                    stats->allocas_promoted++;
                    goto next_alloca;
                }
            }
        }
        next_alloca:;
    }

    /* ── Cleanup ── */
    for (int a = 0; a < n_promotable; a++) {
        vs_destroy(&alloca_ctxs[a].stack);
        free(alloca_ctxs[a].phi_for_block);
        free(promotable[a].def_blocks);
    }
    free(alloca_ctxs);

    (void)mir_validate_function(func);

    return n_promotable;
}

/* ================================================================
 * Module-level entry point
 * ================================================================ */

void mir_mem2reg_module(MirModule* module, MirMem2RegStats* stats) {
    MirMem2RegStats total = {0};
    if (!stats) stats = &total;

    for (MirFunction* func = module->func_list; func; func = func->next_func) {
        if (func->is_extern) continue;
        mir_mem2reg(func, stats);
    }
}
