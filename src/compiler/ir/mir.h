/*
 * Casprix Compiler — Mid-Level Intermediate Representation (MIR)
 *
 * SSA-based IR sitting between the AST and backend code generators.
 * Designed for:
 *   - Efficient optimization (constant propagation, DCE, copy prop, LICM)
 *   - Static safety analysis (borrow checking, lifetime inference)
 *   - Const evaluation (compile-time expression evaluation)
 *   - Backend-agnostic lowering (x86-64, ARM, WASM, bytecode VM)
 *
 * Architecture:
 *   Module → Function* → BasicBlock* → Instruction*
 *   Every value is in SSA form (single definition, dominance-based use).
 *   Phi nodes at block entries for join points.
 *
 * Memory model:
 *   All MIR nodes arena-allocated from MirArena for fast bulk deallocation.
 */

#ifndef MIR_H
#define MIR_H

#include "compiler/frontend/ast.h"
#include <stdint.h>
#include <stdbool.h>

/* ────────────────────────────────────────────────────────────
 * Forward declarations
 * ──────────────────────────────────────────────────────────── */
typedef struct MirModule    MirModule;
typedef struct MirFunction  MirFunction;
typedef struct MirBlock     MirBlock;
typedef struct MirInst      MirInst;
typedef struct MirValue     MirValue;
typedef struct MirType      MirType;
typedef struct MirArena     MirArena;
typedef struct MirBuilder   MirBuilder;

/* ────────────────────────────────────────────────────────────
 * Arena allocator for MIR nodes
 * ──────────────────────────────────────────────────────────── */
#define MIR_ARENA_BLOCK_SIZE (64 * 1024)  /* 64 KB */

typedef struct MirArenaBlock {
    uint8_t*               memory;
    size_t                 capacity;
    size_t                 used;
    struct MirArenaBlock*  next;
} MirArenaBlock;

struct MirArena {
    MirArenaBlock*  first;
    MirArenaBlock*  current;
    size_t          total_allocated;
};

MirArena*  mir_arena_create(void);
void       mir_arena_destroy(MirArena* arena);
void*      mir_arena_alloc(MirArena* arena, size_t size);
char*      mir_arena_strdup(MirArena* arena, const char* s);

/* ────────────────────────────────────────────────────────────
 * MIR Value IDs — lightweight SSA value references
 * ──────────────────────────────────────────────────────────── */

/* Every SSA value gets a unique monotonic ID within a function.
 * Value 0 is reserved as "no value" / void. */
typedef uint32_t MirValueId;
#define MIR_VALUE_NONE  ((MirValueId)0)

/* ────────────────────────────────────────────────────────────
 * MIR Types — compact type representation for IR
 * ──────────────────────────────────────────────────────────── */
typedef enum {
    MIR_TYPE_VOID,
    MIR_TYPE_BOOL,
    MIR_TYPE_I8,  MIR_TYPE_I16, MIR_TYPE_I32, MIR_TYPE_I64,
    MIR_TYPE_U8,  MIR_TYPE_U16, MIR_TYPE_U32, MIR_TYPE_U64,
    MIR_TYPE_F32, MIR_TYPE_F64,
    MIR_TYPE_PTR,           /* raw pointer (typed via pointee_type) */
    MIR_TYPE_REF,           /* borrowed reference (&T) */
    MIR_TYPE_MUT_REF,       /* mutable borrow (&mut T) */
    MIR_TYPE_FUNC,          /* function pointer */
    MIR_TYPE_STRUCT,        /* named aggregate */
    MIR_TYPE_ARRAY,         /* [T; N] fixed-size */
    MIR_TYPE_SLICE,         /* [T] fat pointer (ptr + len) */
    MIR_TYPE_AGGREGATE,     /* anonymous tuple / compound */
} MirTypeKind;

struct MirType {
    MirTypeKind     kind;
    union {
        struct { MirType* pointee; }                            ptr;
        struct { MirType* pointee; bool is_mut; }               ref;
        struct { MirType* ret; MirType** params; int n_params; } func;
        struct { const char* name; MirType** fields;
                 int n_fields; int size; int align; }           strct;
        struct { MirType* elem; int count; }                    array;
        struct { MirType* elem; }                               slice;
    } as;
};

/* ────────────────────────────────────────────────────────────
 * MIR Instructions — SSA operations
 * ──────────────────────────────────────────────────────────── */
typedef enum {
    /* Constants & declarations */
    MIR_CONST_INT,          /* result = imm_i64                  */
    MIR_CONST_FLOAT,        /* result = imm_f64                  */
    MIR_CONST_BOOL,         /* result = imm_bool                 */
    MIR_CONST_STRING,       /* result = &str_literal             */
    MIR_CONST_FUNC,         /* result = &function_symbol         */
    MIR_CONST_NULL,         /* result = null                     */
    MIR_GLOBAL_ADDR,        /* result = &global_symbol           */

    /* Arithmetic (integer) */
    MIR_ADD,                /* result = lhs + rhs                */
    MIR_SUB,                /* result = lhs - rhs                */
    MIR_MUL,                /* result = lhs * rhs                */
    MIR_DIV,                /* result = lhs / rhs                */
    MIR_MOD,                /* result = lhs % rhs                */
    MIR_NEG,                /* result = -operand                 */

    /* Arithmetic (float) */
    MIR_FADD,
    MIR_FSUB,
    MIR_FMUL,
    MIR_FDIV,
    MIR_FNEG,

    /* Bitwise */
    MIR_BAND,               /* result = lhs & rhs               */
    MIR_BOR,                /* result = lhs | rhs               */
    MIR_BXOR,               /* result = lhs ^ rhs               */
    MIR_BNOT,               /* result = ~operand                */
    MIR_SHL,                /* result = lhs << rhs              */
    MIR_SHR,                /* result = lhs >> rhs (arithmetic) */
    MIR_USHR,               /* result = lhs >>> rhs (logical)   */

    /* Comparison */
    MIR_CMP_EQ,             /* result = lhs == rhs              */
    MIR_CMP_NE,             /* result = lhs != rhs              */
    MIR_CMP_LT,             /* result = lhs < rhs               */
    MIR_CMP_LE,             /* result = lhs <= rhs              */
    MIR_CMP_GT,             /* result = lhs > rhs               */
    MIR_CMP_GE,             /* result = lhs >= rhs              */

    /* Logical */
    MIR_LOGIC_AND,          /* result = lhs && rhs (short-circuit) */
    MIR_LOGIC_OR,           /* result = lhs || rhs (short-circuit) */
    MIR_LOGIC_NOT,          /* result = !operand                   */

    /* Type conversion */
    MIR_CAST,               /* result = (target_type)operand       */
    MIR_BITCAST,            /* result = reinterpret(operand)       */
    MIR_TRUNC,              /* result = truncate(operand)          */
    MIR_ZEXT,               /* result = zero_extend(operand)       */
    MIR_SEXT,               /* result = sign_extend(operand)       */
    MIR_SITOFP,             /* result = int_to_float(operand)      */
    MIR_FPTOSI,             /* result = float_to_int(operand)      */

    /* Memory */
    MIR_ALLOCA,             /* result = stack_alloc(type, count)   */
    MIR_LOAD,               /* result = *ptr                       */
    MIR_STORE,              /* *ptr = value (no result)            */
    MIR_GET_FIELD_PTR,      /* result = &(base->field[index])      */
    MIR_GET_ELEM_PTR,       /* result = &(base[index])             */

    /* Control flow */
    MIR_BR,                 /* unconditional branch to target      */
    MIR_CONDBR,             /* if (cond) goto true_bb else false_bb */
    MIR_SWITCH,             /* multi-way branch (match lowering)   */
    MIR_RET,                /* return value (or void)              */
    MIR_RET_VOID,           /* return void                         */
    MIR_UNREACHABLE,        /* undefined behavior / noreturn       */

    /* Function calls */
    MIR_CALL,               /* result = func(args...)              */
    MIR_CALL_INDIRECT,      /* result = (*fptr)(args...)           */
    MIR_CALL_VIRTUAL,       /* result = vtable[idx](self, args...) */

    /* SSA control */
    MIR_PHI,                /* result = phi(block1:val1, block2:val2, ...) */
    MIR_COPY,               /* result = source (for coalescing)    */

    /* Object / reference counting */
    MIR_ARC_RETAIN,         /* arc_retain(ptr)                     */
    MIR_ARC_RELEASE,        /* arc_release(ptr)                    */
    MIR_OBJ_ALLOC,          /* result = obj_alloc(vtable, size)    */

    /* Ownership / safety */
    MIR_BORROW,             /* result = &source (immutable borrow) */
    MIR_BORROW_MUT,         /* result = &mut source (mutable borrow) */
    MIR_MOVE,               /* result = move(source); source invalidated */
    MIR_DROP,               /* drop(value) — invoke destructor     */

    /* Aggregate operations */
    MIR_STRUCT_INIT,        /* result = { field0, field1, ... }    */
    MIR_EXTRACT,            /* result = aggregate.field[idx]       */
    MIR_INSERT,             /* result = aggregate with field[idx]=val */

    /* Debug / metadata */
    MIR_NOP,                /* no operation (placeholder)          */
    MIR_DEBUGLOC,           /* source location annotation          */
} MirOpcode;

/* Phi node incoming edge */
typedef struct {
    MirBlock*    block;
    MirValueId   value;
} MirPhiEdge;

/* Instruction operand structure */
struct MirInst {
    MirOpcode       opcode;
    MirValueId      result;         /* SSA result value (0 if void) */
    MirType*        type;           /* Type of the result value */

    union {
        /* Constants */
        int64_t     imm_i64;
        double      imm_f64;
        bool        imm_bool;
        const char* imm_string;
        const char* global_name;

        /* Binary ops: add, sub, mul, div, cmp, bitwise, shift */
        struct { MirValueId lhs, rhs; } binary;

        /* Unary ops: neg, not, cast, convert */
        struct { MirValueId operand; MirType* target_type; } unary;

        /* Memory: alloca */
        struct { MirType* alloc_type; int count; } alloca;

        /* Memory: load/store */
        struct { MirValueId ptr; MirValueId value; /* value only for store */ } mem;

        /* Memory: get field/elem ptr */
        struct { MirValueId base; int field_index; MirValueId index; } gep;

        /* Control flow: branch */
        struct { MirBlock* target; } br;

        /* Control flow: conditional branch */
        struct { MirValueId cond; MirBlock* true_bb; MirBlock* false_bb; } condbr;

        /* Control flow: switch */
        struct { MirValueId discriminant; MirBlock** targets;
                 int64_t* case_values; int n_cases; MirBlock* default_bb; } sw;

        /* Control flow: return */
        struct { MirValueId value; } ret;

        /* Function call */
        struct { const char* func_name; MirValueId* args;
                 int n_args; MirValueId callee; /* for indirect */ } call;

        /* Virtual call */
        struct { MirValueId self_obj; int vtable_index;
                 MirValueId* args; int n_args; } vcall;

        /* Phi node */
        struct { MirPhiEdge* edges; int n_edges; } phi;

        /* Copy/move/borrow */
        struct { MirValueId source; } transfer;

        /* ARC retain/release, drop */
        struct { MirValueId ptr; } refop;

        /* Obj alloc */
        struct { const char* class_name; int size; } obj_alloc;

        /* Struct init */
        struct { MirValueId* fields; int n_fields; } struct_init;

        /* Extract/Insert */
        struct { MirValueId aggregate; int field_idx;
                 MirValueId insert_val; /* only for INSERT */ } field_op;

        /* Debug location */
        struct { int line; int column; const char* file; } debug;
    } as;

    /* Linked list within basic block */
    MirInst*    next;
    MirInst*    prev;

    /* Source location (for diagnostics) */
    int         src_line;
    int         src_col;
};

/* ────────────────────────────────────────────────────────────
 * Basic Blocks
 * ──────────────────────────────────────────────────────────── */
struct MirBlock {
    uint32_t        id;
    const char*     label;          /* e.g., "entry", "then", "else", "loop.body" */
    MirInst*        first;          /* first instruction (linked list head) */
    MirInst*        last;           /* last instruction (linked list tail) */
    int             inst_count;

    /* CFG edges */
    MirBlock**      predecessors;
    int             pred_count;
    int             pred_capacity;
    MirBlock**      successors;
    int             succ_count;
    int             succ_capacity;

    /* Dominator tree (computed by analysis passes) */
    MirBlock*       idom;           /* immediate dominator */
    MirBlock**      dom_children;
    int             dom_child_count;
    int             dom_frontier_count;
    MirBlock**      dom_frontier;

    /* Loop info (computed by loop analysis) */
    int             loop_depth;
    bool            is_loop_header;

    /* Linked list within function */
    MirFunction*    parent;
    MirBlock*       next_block;
};

/* ────────────────────────────────────────────────────────────
 * Functions
 * ──────────────────────────────────────────────────────────── */
typedef struct {
    const char*     name;
    MirType*        type;
    MirValueId      value_id;      /* SSA value for this parameter */
} MirParam;

struct MirFunction {
    const char*     name;
    MirType*        return_type;
    MirParam*       params;
    int             param_count;

    MirBlock*       entry_block;    /* first basic block */
    MirBlock*       block_list;     /* linked list of all blocks */
    int             block_count;

    MirValueId      next_value_id;  /* monotonic SSA value counter */

    /* Per-value type table (indexed by MirValueId) */
    MirType**       value_types;
    int             value_type_capacity;

    /* For const-eval: is this function constexpr? */
    bool            is_constexpr;
    bool            is_extern;

    MirModule*      parent;
    MirFunction*    next_func;      /* linked list within module */
};

/* ────────────────────────────────────────────────────────────
 * Module — top-level IR container
 * ──────────────────────────────────────────────────────────── */
struct MirModule {
    const char*     name;
    MirArena*       arena;          /* all MIR nodes allocated here */

    MirFunction*    func_list;      /* linked list of functions */
    int             func_count;

    /* Global variables / constants */
    struct {
        const char*     name;
        MirType*        type;
        MirValueId      initializer;   /* const init or MIR_VALUE_NONE */
        bool            is_const;
    }*              globals;
    int             global_count;
    int             global_capacity;

    /* Type interning table */
    MirType**       type_cache;
    int             type_cache_count;
    int             type_cache_capacity;

    /* String literal table */
    const char**    string_literals;
    int             string_count;
    int             string_capacity;
};

/* ────────────────────────────────────────────────────────────
 * Module API
 * ──────────────────────────────────────────────────────────── */
MirModule*    mir_module_create(const char* name);
void          mir_module_destroy(MirModule* module);
MirFunction*  mir_module_add_function(MirModule* module, const char* name,
                                       MirType* ret, MirParam* params, int n_params);
MirFunction*  mir_module_find_function(MirModule* module, const char* name);
int           mir_module_add_global(MirModule* module, const char* name,
                                    MirType* type, bool is_const);
int           mir_module_find_global(MirModule* module, const char* name);
int           mir_module_add_string(MirModule* module, const char* str);

/* Type constructors (interned) */
MirType*  mir_type_void(MirModule* m);
MirType*  mir_type_bool(MirModule* m);
MirType*  mir_type_i8(MirModule* m);
MirType*  mir_type_i16(MirModule* m);
MirType*  mir_type_i32(MirModule* m);
MirType*  mir_type_i64(MirModule* m);
MirType*  mir_type_u8(MirModule* m);
MirType*  mir_type_u16(MirModule* m);
MirType*  mir_type_u32(MirModule* m);
MirType*  mir_type_u64(MirModule* m);
MirType*  mir_type_f32(MirModule* m);
MirType*  mir_type_f64(MirModule* m);
MirType*  mir_type_ptr(MirModule* m, MirType* pointee);
MirType*  mir_type_ref(MirModule* m, MirType* pointee, bool is_mut);
MirType*  mir_type_struct(MirModule* m, const char* name,
                           MirType** fields, int n_fields);
MirType*  mir_type_array(MirModule* m, MirType* elem, int count);
MirType*  mir_type_func(MirModule* m, MirType* ret,
                         MirType** params, int n_params);

/* Type queries */
int       mir_type_size(MirType* t);
int       mir_type_align(MirType* t);
bool      mir_type_is_integer(MirType* t);
bool      mir_type_is_float(MirType* t);
bool      mir_type_is_pointer(MirType* t);
const char* mir_type_name(MirType* t);

/* ────────────────────────────────────────────────────────────
 * Function API
 * ──────────────────────────────────────────────────────────── */
MirBlock*   mir_function_add_block(MirFunction* func, const char* label);
MirValueId  mir_function_new_value(MirFunction* func, MirType* type);
MirType*    mir_function_value_type(MirFunction* func, MirValueId id);

/* ────────────────────────────────────────────────────────────
 * Block API
 * ──────────────────────────────────────────────────────────── */
void      mir_block_append(MirBlock* block, MirInst* inst);
void      mir_block_add_predecessor(MirBlock* block, MirBlock* pred);
void      mir_block_add_successor(MirBlock* block, MirBlock* succ);
bool      mir_block_is_terminated(MirBlock* block);

/* ────────────────────────────────────────────────────────────
 * MIR Builder — high-level instruction construction
 *
 * Usage:
 *   MirBuilder b;
 *   mir_builder_init(&b, module, func);
 *   mir_builder_set_block(&b, entry);
 *   MirValueId v = mir_build_add(&b, a, c);
 *   mir_build_ret(&b, v);
 * ──────────────────────────────────────────────────────────── */
struct MirBuilder {
    MirModule*    module;
    MirFunction*  func;
    MirBlock*     current_block;
};

void mir_builder_init(MirBuilder* b, MirModule* module, MirFunction* func);
void mir_builder_set_block(MirBuilder* b, MirBlock* block);

/* Constants */
MirValueId mir_build_const_int(MirBuilder* b, int64_t val, MirType* type);
MirValueId mir_build_const_float(MirBuilder* b, double val, MirType* type);
MirValueId mir_build_const_bool(MirBuilder* b, bool val);
MirValueId mir_build_const_string(MirBuilder* b, const char* str);
MirValueId mir_build_const_func(MirBuilder* b, const char* func_name, MirType* type);
MirValueId mir_build_const_null(MirBuilder* b, MirType* ptr_type);
MirValueId mir_build_global_addr(MirBuilder* b, const char* global_name, MirType* value_type);

/* Arithmetic */
MirValueId mir_build_add(MirBuilder* b, MirValueId lhs, MirValueId rhs);
MirValueId mir_build_sub(MirBuilder* b, MirValueId lhs, MirValueId rhs);
MirValueId mir_build_mul(MirBuilder* b, MirValueId lhs, MirValueId rhs);
MirValueId mir_build_div(MirBuilder* b, MirValueId lhs, MirValueId rhs);
MirValueId mir_build_mod(MirBuilder* b, MirValueId lhs, MirValueId rhs);
MirValueId mir_build_neg(MirBuilder* b, MirValueId operand);

/* Float arithmetic */
MirValueId mir_build_fadd(MirBuilder* b, MirValueId lhs, MirValueId rhs);
MirValueId mir_build_fsub(MirBuilder* b, MirValueId lhs, MirValueId rhs);
MirValueId mir_build_fmul(MirBuilder* b, MirValueId lhs, MirValueId rhs);
MirValueId mir_build_fdiv(MirBuilder* b, MirValueId lhs, MirValueId rhs);
MirValueId mir_build_fneg(MirBuilder* b, MirValueId operand);

/* Comparison */
MirValueId mir_build_cmp_eq(MirBuilder* b, MirValueId lhs, MirValueId rhs);
MirValueId mir_build_cmp_ne(MirBuilder* b, MirValueId lhs, MirValueId rhs);
MirValueId mir_build_cmp_lt(MirBuilder* b, MirValueId lhs, MirValueId rhs);
MirValueId mir_build_cmp_le(MirBuilder* b, MirValueId lhs, MirValueId rhs);
MirValueId mir_build_cmp_gt(MirBuilder* b, MirValueId lhs, MirValueId rhs);
MirValueId mir_build_cmp_ge(MirBuilder* b, MirValueId lhs, MirValueId rhs);

/* Bitwise */
MirValueId mir_build_and(MirBuilder* b, MirValueId lhs, MirValueId rhs);
MirValueId mir_build_or(MirBuilder* b, MirValueId lhs, MirValueId rhs);
MirValueId mir_build_xor(MirBuilder* b, MirValueId lhs, MirValueId rhs);
MirValueId mir_build_not(MirBuilder* b, MirValueId operand);
MirValueId mir_build_shl(MirBuilder* b, MirValueId lhs, MirValueId rhs);
MirValueId mir_build_shr(MirBuilder* b, MirValueId lhs, MirValueId rhs);

/* Memory */
MirValueId mir_build_alloca(MirBuilder* b, MirType* type);
MirValueId mir_build_load(MirBuilder* b, MirValueId ptr, MirType* type);
void       mir_build_store(MirBuilder* b, MirValueId ptr, MirValueId value);
MirValueId mir_build_get_field_ptr(MirBuilder* b, MirValueId base, int field_idx);
MirValueId mir_build_get_elem_ptr(MirBuilder* b, MirValueId base, MirValueId idx);

/* Control flow */
void       mir_build_br(MirBuilder* b, MirBlock* target);
void       mir_build_condbr(MirBuilder* b, MirValueId cond,
                             MirBlock* true_bb, MirBlock* false_bb);
void       mir_build_ret(MirBuilder* b, MirValueId value);
void       mir_build_ret_void(MirBuilder* b);

/* Calls */
MirValueId mir_build_call(MirBuilder* b, const char* func_name,
                           MirValueId* args, int n_args, MirType* ret_type);
MirValueId mir_build_call_indirect(MirBuilder* b, MirValueId callee,
                                    MirValueId* args, int n_args, MirType* ret_type);
MirValueId mir_build_call_virtual(MirBuilder* b, MirValueId self,
                                   int vtable_idx, MirValueId* args,
                                   int n_args, MirType* ret_type);

/* Phi */
MirValueId mir_build_phi(MirBuilder* b, MirType* type);
void       mir_phi_add_edge(MirInst* phi_inst, MirBlock* block, MirValueId value);

/* Type conversion */
MirValueId mir_build_cast(MirBuilder* b, MirValueId val, MirType* target);
MirValueId mir_build_zext(MirBuilder* b, MirValueId val, MirType* target);
MirValueId mir_build_sext(MirBuilder* b, MirValueId val, MirType* target);
MirValueId mir_build_trunc(MirBuilder* b, MirValueId val, MirType* target);
MirValueId mir_build_sitofp(MirBuilder* b, MirValueId val, MirType* target);
MirValueId mir_build_fptosi(MirBuilder* b, MirValueId val, MirType* target);

/* Ownership / safety */
MirValueId mir_build_borrow(MirBuilder* b, MirValueId source);
MirValueId mir_build_borrow_mut(MirBuilder* b, MirValueId source);
MirValueId mir_build_move(MirBuilder* b, MirValueId source);
void       mir_build_drop(MirBuilder* b, MirValueId value);

/* Reference counting */
void       mir_build_arc_retain(MirBuilder* b, MirValueId ptr);
void       mir_build_arc_release(MirBuilder* b, MirValueId ptr);
MirValueId mir_build_obj_alloc(MirBuilder* b, const char* class_name, int size);

/* Aggregates */
MirValueId mir_build_struct_init(MirBuilder* b, MirType* type,
                                  MirValueId* fields, int n_fields);
MirValueId mir_build_extract(MirBuilder* b, MirValueId agg, int field_idx);
MirValueId mir_build_insert(MirBuilder* b, MirValueId agg,
                             int field_idx, MirValueId val);

/* ────────────────────────────────────────────────────────────
 * MIR Printer — textual representation for debugging
 * ──────────────────────────────────────────────────────────── */
void mir_print_module(MirModule* module, FILE* out);
void mir_print_function(MirFunction* func, FILE* out);
void mir_print_block(MirBlock* block, FILE* out);
void mir_print_inst(MirInst* inst, FILE* out);
void mir_print_type(MirType* type, FILE* out);

/* ────────────────────────────────────────────────────────────
 * MIR Validation — structural integrity checks
 * ──────────────────────────────────────────────────────────── */
bool mir_validate_module(MirModule* module);
bool mir_validate_function(MirFunction* func);

#endif /* MIR_H */
