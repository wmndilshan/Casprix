/*
 * Casprix Compiler — MIR Core Implementation
 *
 * Arena allocator, type system, module/function/block management,
 * and all the infrastructure that the builder and passes depend on.
 */

#include "mir.h"
#include "mir_borrow.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

/* ================================================================
 * Arena Allocator
 * ================================================================ */

static MirArenaBlock* mir_arena_block_create(size_t capacity) {
    MirArenaBlock* block = (MirArenaBlock*)malloc(sizeof(MirArenaBlock));
    block->memory = (uint8_t*)malloc(capacity);
    block->capacity = capacity;
    block->used = 0;
    block->next = NULL;
    return block;
}

MirArena* mir_arena_create(void) {
    MirArena* arena = (MirArena*)malloc(sizeof(MirArena));
    arena->first = mir_arena_block_create(MIR_ARENA_BLOCK_SIZE);
    arena->current = arena->first;
    arena->total_allocated = 0;
    return arena;
}

void mir_arena_destroy(MirArena* arena) {
    if (!arena) return;
    MirArenaBlock* block = arena->first;
    while (block) {
        MirArenaBlock* next = block->next;
        free(block->memory);
        free(block);
        block = next;
    }
    free(arena);
}

void* mir_arena_alloc(MirArena* arena, size_t size) {
    /* Align to 8 bytes */
    size = (size + 7) & ~(size_t)7;

    if (arena->current->used + size > arena->current->capacity) {
        size_t block_size = size > MIR_ARENA_BLOCK_SIZE ? size * 2 : MIR_ARENA_BLOCK_SIZE;
        MirArenaBlock* new_block = mir_arena_block_create(block_size);
        arena->current->next = new_block;
        arena->current = new_block;
    }

    void* ptr = arena->current->memory + arena->current->used;
    arena->current->used += size;
    arena->total_allocated += size;
    memset(ptr, 0, size);
    return ptr;
}

char* mir_arena_strdup(MirArena* arena, const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* copy = (char*)mir_arena_alloc(arena, len);
    memcpy(copy, s, len);
    return copy;
}

/* ================================================================
 * Type System — Interned Types
 * ================================================================ */

/* Primitive type singletons (allocated lazily, one per module) */
static MirType* mir_intern_primitive(MirModule* m, MirTypeKind kind) {
    /* Search cache for existing type */
    for (int i = 0; i < m->type_cache_count; i++) {
        if (m->type_cache[i]->kind == kind &&
            kind != MIR_TYPE_PTR && kind != MIR_TYPE_REF &&
            kind != MIR_TYPE_MUT_REF && kind != MIR_TYPE_FUNC &&
            kind != MIR_TYPE_STRUCT && kind != MIR_TYPE_ARRAY &&
            kind != MIR_TYPE_SLICE && kind != MIR_TYPE_AGGREGATE) {
            return m->type_cache[i];
        }
    }

    /* Allocate new type */
    MirType* t = (MirType*)mir_arena_alloc(m->arena, sizeof(MirType));
    t->kind = kind;

    /* Add to cache */
    if (m->type_cache_count >= m->type_cache_capacity) {
        m->type_cache_capacity = m->type_cache_capacity ? m->type_cache_capacity * 2 : 64;
        m->type_cache = (MirType**)realloc(m->type_cache,
                         m->type_cache_capacity * sizeof(MirType*));
    }
    m->type_cache[m->type_cache_count++] = t;
    return t;
}

MirType* mir_type_void(MirModule* m) { return mir_intern_primitive(m, MIR_TYPE_VOID); }
MirType* mir_type_bool(MirModule* m) { return mir_intern_primitive(m, MIR_TYPE_BOOL); }
MirType* mir_type_i8(MirModule* m)   { return mir_intern_primitive(m, MIR_TYPE_I8); }
MirType* mir_type_i16(MirModule* m)  { return mir_intern_primitive(m, MIR_TYPE_I16); }
MirType* mir_type_i32(MirModule* m)  { return mir_intern_primitive(m, MIR_TYPE_I32); }
MirType* mir_type_i64(MirModule* m)  { return mir_intern_primitive(m, MIR_TYPE_I64); }
MirType* mir_type_u8(MirModule* m)   { return mir_intern_primitive(m, MIR_TYPE_U8); }
MirType* mir_type_u16(MirModule* m)  { return mir_intern_primitive(m, MIR_TYPE_U16); }
MirType* mir_type_u32(MirModule* m)  { return mir_intern_primitive(m, MIR_TYPE_U32); }
MirType* mir_type_u64(MirModule* m)  { return mir_intern_primitive(m, MIR_TYPE_U64); }
MirType* mir_type_f32(MirModule* m)  { return mir_intern_primitive(m, MIR_TYPE_F32); }
MirType* mir_type_f64(MirModule* m)  { return mir_intern_primitive(m, MIR_TYPE_F64); }

MirType* mir_type_ptr(MirModule* m, MirType* pointee) {
    for (int i = 0; i < m->type_cache_count; i++) {
        if (m->type_cache[i]->kind == MIR_TYPE_PTR &&
            m->type_cache[i]->as.ptr.pointee == pointee) {
            return m->type_cache[i];
        }
    }
    MirType* t = (MirType*)mir_arena_alloc(m->arena, sizeof(MirType));
    t->kind = MIR_TYPE_PTR;
    t->as.ptr.pointee = pointee;

    if (m->type_cache_count >= m->type_cache_capacity) {
        m->type_cache_capacity = m->type_cache_capacity ? m->type_cache_capacity * 2 : 64;
        m->type_cache = (MirType**)realloc(m->type_cache,
                         m->type_cache_capacity * sizeof(MirType*));
    }
    m->type_cache[m->type_cache_count++] = t;
    return t;
}

MirType* mir_type_ref(MirModule* m, MirType* pointee, bool is_mut) {
    MirTypeKind kind = is_mut ? MIR_TYPE_MUT_REF : MIR_TYPE_REF;
    for (int i = 0; i < m->type_cache_count; i++) {
        if (m->type_cache[i]->kind == kind &&
            m->type_cache[i]->as.ref.pointee == pointee) {
            return m->type_cache[i];
        }
    }
    MirType* t = (MirType*)mir_arena_alloc(m->arena, sizeof(MirType));
    t->kind = kind;
    t->as.ref.pointee = pointee;
    t->as.ref.is_mut = is_mut;

    if (m->type_cache_count >= m->type_cache_capacity) {
        m->type_cache_capacity = m->type_cache_capacity ? m->type_cache_capacity * 2 : 64;
        m->type_cache = (MirType**)realloc(m->type_cache,
                         m->type_cache_capacity * sizeof(MirType*));
    }
    m->type_cache[m->type_cache_count++] = t;
    return t;
}

MirType* mir_type_struct(MirModule* m, const char* name,
                          MirType** fields, int n_fields) {
    /* Structs are NOT interned by content — each call creates a new named type.
     * But we do cache by name for deduplication. */
    for (int i = 0; i < m->type_cache_count; i++) {
        if (m->type_cache[i]->kind == MIR_TYPE_STRUCT &&
            m->type_cache[i]->as.strct.name &&
            name && strcmp(m->type_cache[i]->as.strct.name, name) == 0) {
            return m->type_cache[i];
        }
    }

    MirType* t = (MirType*)mir_arena_alloc(m->arena, sizeof(MirType));
    t->kind = MIR_TYPE_STRUCT;
    t->as.strct.name = mir_arena_strdup(m->arena, name);
    t->as.strct.n_fields = n_fields;
    if (n_fields > 0) {
        t->as.strct.fields = (MirType**)mir_arena_alloc(m->arena,
                              n_fields * sizeof(MirType*));
        memcpy(t->as.strct.fields, fields, n_fields * sizeof(MirType*));
    }

    /* Compute size and alignment */
    int size = 0, align = 1;
    for (int i = 0; i < n_fields; i++) {
        int fa = mir_type_align(fields[i]);
        if (fa > align) align = fa;
        size = (size + fa - 1) & ~(fa - 1);  /* align offset */
        size += mir_type_size(fields[i]);
    }
    size = (size + align - 1) & ~(align - 1); /* pad to alignment */
    t->as.strct.size = size;
    t->as.strct.align = align;

    if (m->type_cache_count >= m->type_cache_capacity) {
        m->type_cache_capacity = m->type_cache_capacity ? m->type_cache_capacity * 2 : 64;
        m->type_cache = (MirType**)realloc(m->type_cache,
                         m->type_cache_capacity * sizeof(MirType*));
    }
    m->type_cache[m->type_cache_count++] = t;
    return t;
}

MirType* mir_type_array(MirModule* m, MirType* elem, int count) {
    for (int i = 0; i < m->type_cache_count; i++) {
        if (m->type_cache[i]->kind == MIR_TYPE_ARRAY &&
            m->type_cache[i]->as.array.elem == elem &&
            m->type_cache[i]->as.array.count == count) {
            return m->type_cache[i];
        }
    }

    MirType* t = (MirType*)mir_arena_alloc(m->arena, sizeof(MirType));
    t->kind = MIR_TYPE_ARRAY;
    t->as.array.elem = elem;
    t->as.array.count = count;

    if (m->type_cache_count >= m->type_cache_capacity) {
        m->type_cache_capacity = m->type_cache_capacity ? m->type_cache_capacity * 2 : 64;
        m->type_cache = (MirType**)realloc(m->type_cache,
                         m->type_cache_capacity * sizeof(MirType*));
    }
    m->type_cache[m->type_cache_count++] = t;
    return t;
}

MirType* mir_type_func(MirModule* m, MirType* ret,
                        MirType** params, int n_params) {
    MirType* t = (MirType*)mir_arena_alloc(m->arena, sizeof(MirType));
    t->kind = MIR_TYPE_FUNC;
    t->as.func.ret = ret;
    t->as.func.n_params = n_params;
    if (n_params > 0) {
        t->as.func.params = (MirType**)mir_arena_alloc(m->arena,
                              n_params * sizeof(MirType*));
        memcpy(t->as.func.params, params, n_params * sizeof(MirType*));
    }

    if (m->type_cache_count >= m->type_cache_capacity) {
        m->type_cache_capacity = m->type_cache_capacity ? m->type_cache_capacity * 2 : 64;
        m->type_cache = (MirType**)realloc(m->type_cache,
                         m->type_cache_capacity * sizeof(MirType*));
    }
    m->type_cache[m->type_cache_count++] = t;
    return t;
}

/* ────────────── Type queries ────────────── */

int mir_type_size(MirType* t) {
    if (!t) return 0;
    switch (t->kind) {
        case MIR_TYPE_VOID:     return 0;
        case MIR_TYPE_BOOL:     return 1;
        case MIR_TYPE_I8:
        case MIR_TYPE_U8:       return 1;
        case MIR_TYPE_I16:
        case MIR_TYPE_U16:      return 2;
        case MIR_TYPE_I32:
        case MIR_TYPE_U32:      return 4;
        case MIR_TYPE_I64:
        case MIR_TYPE_U64:      return 8;
        case MIR_TYPE_F32:      return 4;
        case MIR_TYPE_F64:      return 8;
        case MIR_TYPE_PTR:
        case MIR_TYPE_REF:
        case MIR_TYPE_MUT_REF:
        case MIR_TYPE_FUNC:     return 8;  /* 64-bit pointers */
        case MIR_TYPE_STRUCT:   return t->as.strct.size;
        case MIR_TYPE_ARRAY:    return mir_type_size(t->as.array.elem) * t->as.array.count;
        case MIR_TYPE_SLICE:    return 16; /* ptr + len */
        case MIR_TYPE_AGGREGATE: return 0; /* depends on usage */
    }
    return 0;
}

int mir_type_align(MirType* t) {
    if (!t) return 1;
    switch (t->kind) {
        case MIR_TYPE_VOID:     return 1;
        case MIR_TYPE_BOOL:
        case MIR_TYPE_I8:
        case MIR_TYPE_U8:       return 1;
        case MIR_TYPE_I16:
        case MIR_TYPE_U16:      return 2;
        case MIR_TYPE_I32:
        case MIR_TYPE_U32:
        case MIR_TYPE_F32:      return 4;
        case MIR_TYPE_I64:
        case MIR_TYPE_U64:
        case MIR_TYPE_F64:
        case MIR_TYPE_PTR:
        case MIR_TYPE_REF:
        case MIR_TYPE_MUT_REF:
        case MIR_TYPE_FUNC:     return 8;
        case MIR_TYPE_STRUCT:   return t->as.strct.align;
        case MIR_TYPE_ARRAY:    return mir_type_align(t->as.array.elem);
        case MIR_TYPE_SLICE:    return 8;
        case MIR_TYPE_AGGREGATE: return 8;
    }
    return 1;
}

bool mir_type_is_integer(MirType* t) {
    return t && (t->kind >= MIR_TYPE_I8 && t->kind <= MIR_TYPE_U64);
}

bool mir_type_is_float(MirType* t) {
    return t && (t->kind == MIR_TYPE_F32 || t->kind == MIR_TYPE_F64);
}

bool mir_type_is_pointer(MirType* t) {
    return t && (t->kind == MIR_TYPE_PTR || t->kind == MIR_TYPE_REF ||
                 t->kind == MIR_TYPE_MUT_REF);
}

const char* mir_type_name(MirType* t) {
    if (!t) return "void";
    switch (t->kind) {
        case MIR_TYPE_VOID:     return "void";
        case MIR_TYPE_BOOL:     return "bool";
        case MIR_TYPE_I8:       return "i8";
        case MIR_TYPE_I16:      return "i16";
        case MIR_TYPE_I32:      return "i32";
        case MIR_TYPE_I64:      return "i64";
        case MIR_TYPE_U8:       return "u8";
        case MIR_TYPE_U16:      return "u16";
        case MIR_TYPE_U32:      return "u32";
        case MIR_TYPE_U64:      return "u64";
        case MIR_TYPE_F32:      return "f32";
        case MIR_TYPE_F64:      return "f64";
        case MIR_TYPE_PTR:      return "ptr";
        case MIR_TYPE_REF:      return "&ref";
        case MIR_TYPE_MUT_REF:  return "&mut ref";
        case MIR_TYPE_FUNC:     return "fn";
        case MIR_TYPE_STRUCT:   return t->as.strct.name ? t->as.strct.name : "struct";
        case MIR_TYPE_ARRAY:    return "array";
        case MIR_TYPE_SLICE:    return "slice";
        case MIR_TYPE_AGGREGATE: return "aggregate";
    }
    return "unknown";
}

/* ================================================================
 * Module
 * ================================================================ */

MirModule* mir_module_create(const char* name) {
    MirArena* arena = mir_arena_create();
    MirModule* m = (MirModule*)mir_arena_alloc(arena, sizeof(MirModule));
    m->arena = arena;
    m->name = mir_arena_strdup(arena, name);
    m->func_list = NULL;
    m->func_count = 0;
    m->globals = NULL;
    m->global_count = 0;
    m->global_capacity = 0;
    m->type_cache = NULL;
    m->type_cache_count = 0;
    m->type_cache_capacity = 0;
    m->string_literals = NULL;
    m->string_count = 0;
    m->string_capacity = 0;
    return m;
}

void mir_module_destroy(MirModule* module) {
    if (!module) return;
    /* type_cache and globals arrays are realloc'd outside the arena */
    free(module->type_cache);
    free(module->globals);
    free(module->string_literals);
    /* Destroy arena (frees everything else) */
    mir_arena_destroy(module->arena);
}

MirFunction* mir_module_add_function(MirModule* module, const char* name,
                                      MirType* ret, MirParam* params, int n_params) {
    MirFunction* func = (MirFunction*)mir_arena_alloc(module->arena, sizeof(MirFunction));
    func->name = mir_arena_strdup(module->arena, name);
    func->return_type = ret;
    func->param_count = n_params;
    func->next_value_id = 1;  /* 0 is reserved for MIR_VALUE_NONE */
    func->parent = module;
    func->entry_block = NULL;
    func->block_list = NULL;
    func->block_count = 0;
    func->is_constexpr = false;
    func->is_extern = false;

    /* Value type table — initial capacity.  Must be initialised BEFORE
     * `mir_function_new_value` is ever called for this function (e.g.
     * for parameter value IDs below); otherwise those entries get
     * written into a zero-sized table and then silently clobbered when
     * the table is replaced here. */
    func->value_type_capacity = 64;
    func->value_types = (MirType**)mir_arena_alloc(module->arena,
                         func->value_type_capacity * sizeof(MirType*));

    /* Copy params */
    if (n_params > 0) {
        func->params = (MirParam*)mir_arena_alloc(module->arena,
                        n_params * sizeof(MirParam));
        for (int i = 0; i < n_params; i++) {
            func->params[i].name = mir_arena_strdup(module->arena, params[i].name);
            func->params[i].type = params[i].type;
            func->params[i].value_id = mir_function_new_value(func, params[i].type);
        }
    } else {
        func->params = NULL;
    }

    /* Link into module's function list */
    func->next_func = module->func_list;
    module->func_list = func;
    module->func_count++;

    return func;
}

MirFunction* mir_module_find_function(MirModule* module, const char* name) {
    for (MirFunction* f = module->func_list; f; f = f->next_func) {
        if (strcmp(f->name, name) == 0) return f;
    }
    return NULL;
}

int mir_module_find_global(MirModule* module, const char* name) {
    if (!module || !name) return -1;

    for (int i = 0; i < module->global_count; i++) {
        if (module->globals[i].name && strcmp(module->globals[i].name, name) == 0) {
            return i;
        }
    }

    return -1;
}

int mir_module_add_global(MirModule* module, const char* name,
                          MirType* type, bool is_const) {
    int existing;

    if (!module || !name) return -1;

    existing = mir_module_find_global(module, name);
    if (existing >= 0) {
        if (!module->globals[existing].type) {
            module->globals[existing].type = type;
        }
        module->globals[existing].is_const = module->globals[existing].is_const || is_const;
        return existing;
    }

    if (module->global_count >= module->global_capacity) {
        int new_capacity = module->global_capacity ? module->global_capacity * 2 : 16;
        void* new_globals = realloc(module->globals,
                                    new_capacity * sizeof(module->globals[0]));
        if (!new_globals) {
            return -1;
        }
        module->globals = new_globals;
        module->global_capacity = new_capacity;
    }

    existing = module->global_count++;
    module->globals[existing].name = mir_arena_strdup(module->arena, name);
    module->globals[existing].type = type;
    module->globals[existing].initializer = MIR_VALUE_NONE;
    module->globals[existing].is_const = is_const;
    return existing;
}

int mir_module_add_string(MirModule* module, const char* str) {
    /* Deduplicate */
    for (int i = 0; i < module->string_count; i++) {
        if (strcmp(module->string_literals[i], str) == 0) return i;
    }

    if (module->string_count >= module->string_capacity) {
        module->string_capacity = module->string_capacity ? module->string_capacity * 2 : 32;
        module->string_literals = (const char**)realloc(module->string_literals,
                                   module->string_capacity * sizeof(const char*));
    }
    module->string_literals[module->string_count] = mir_arena_strdup(module->arena, str);
    return module->string_count++;
}

/* ================================================================
 * Function
 * ================================================================ */

MirBlock* mir_function_add_block(MirFunction* func, const char* label) {
    MirBlock* block = (MirBlock*)mir_arena_alloc(func->parent->arena, sizeof(MirBlock));
    block->id = func->block_count++;
    block->label = mir_arena_strdup(func->parent->arena, label);
    block->first = NULL;
    block->last = NULL;
    block->inst_count = 0;
    block->parent = func;

    /* CFG edges */
    block->predecessors = NULL;
    block->pred_count = 0;
    block->pred_capacity = 0;
    block->successors = NULL;
    block->succ_count = 0;
    block->succ_capacity = 0;

    /* Dominator info */
    block->idom = NULL;
    block->dom_children = NULL;
    block->dom_child_count = 0;
    block->dom_frontier = NULL;
    block->dom_frontier_count = 0;

    /* Loop info */
    block->loop_depth = 0;
    block->is_loop_header = false;

    /* Link into function's block list (append to end) */
    block->next_block = NULL;
    if (!func->block_list) {
        func->block_list = block;
        func->entry_block = block;
    } else {
        MirBlock* last = func->block_list;
        while (last->next_block) last = last->next_block;
        last->next_block = block;
    }

    return block;
}

MirValueId mir_function_new_value(MirFunction* func, MirType* type) {
    MirValueId id = func->next_value_id++;

    /* Grow value type table if needed */
    if ((int)id >= func->value_type_capacity) {
        int new_cap = func->value_type_capacity * 2;
        MirType** new_types = (MirType**)mir_arena_alloc(func->parent->arena,
                               new_cap * sizeof(MirType*));
        memcpy(new_types, func->value_types,
               func->value_type_capacity * sizeof(MirType*));
        func->value_types = new_types;
        func->value_type_capacity = new_cap;
    }

    func->value_types[id] = type;
    return id;
}

MirType* mir_function_value_type(MirFunction* func, MirValueId id) {
    if (id == MIR_VALUE_NONE || (int)id >= func->value_type_capacity) return NULL;
    return func->value_types[id];
}

/* ================================================================
 * Block
 * ================================================================ */

void mir_block_append(MirBlock* block, MirInst* inst) {
    inst->next = NULL;
    inst->prev = block->last;
    if (block->last) {
        block->last->next = inst;
    } else {
        block->first = inst;
    }
    block->last = inst;
    block->inst_count++;
}

static void ensure_block_array(MirArena* arena, MirBlock*** arr, int* count, int* cap, MirBlock* val) {
    if (*count >= *cap) {
        int new_cap = *cap ? *cap * 2 : 4;
        MirBlock** new_arr = (MirBlock**)mir_arena_alloc(arena, new_cap * sizeof(MirBlock*));
        if (*arr) memcpy(new_arr, *arr, *count * sizeof(MirBlock*));
        *arr = new_arr;
        *cap = new_cap;
    }
    (*arr)[(*count)++] = val;
}

void mir_block_add_predecessor(MirBlock* block, MirBlock* pred) {
    /* Avoid duplicates */
    for (int i = 0; i < block->pred_count; i++) {
        if (block->predecessors[i] == pred) return;
    }
    ensure_block_array(block->parent->parent->arena,
                       &block->predecessors, &block->pred_count,
                       &block->pred_capacity, pred);
}

void mir_block_add_successor(MirBlock* block, MirBlock* succ) {
    for (int i = 0; i < block->succ_count; i++) {
        if (block->successors[i] == succ) return;
    }
    ensure_block_array(block->parent->parent->arena,
                       &block->successors, &block->succ_count,
                       &block->succ_capacity, succ);
}

bool mir_block_is_terminated(MirBlock* block) {
    if (!block->last) return false;
    switch (block->last->opcode) {
        case MIR_BR:
        case MIR_CONDBR:
        case MIR_SWITCH:
        case MIR_RET:
        case MIR_RET_VOID:
        case MIR_UNREACHABLE:
            return true;
        default:
            return false;
    }
}

/* ================================================================
 * MIR Verification — contract checks between lowering and codegen
 * ================================================================ */

static void mir_verror(FILE* diag, const char* fmt, ...) {
    if (!diag) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(diag, "MIR_VERIFY: ");
    vfprintf(diag, fmt, ap);
    fprintf(diag, "\n");
    va_end(ap);
}

static bool mir_vid_defined(MirFunction* func, MirValueId id) {
    if (id == MIR_VALUE_NONE) return true;
    return (int)id >= 0 && (int)id < (int)func->next_value_id;
}

static int mir_verify_value_ids_operand(FILE* diag, MirFunction* func,
                                         MirValueId id, const char* ctx) {
    if (id == MIR_VALUE_NONE) return 0;
    if (!mir_vid_defined(func, id)) {
        mir_verror(diag, "function @%s: undefined SSA value %%%u (%s)",
                   func->name ? func->name : "?", (unsigned)id, ctx);
        return 1;
    }
    if (!mir_function_value_type(func, id)) {
        mir_verror(diag, "function @%s: missing type for value %%%u (%s)",
                   func->name ? func->name : "?", (unsigned)id, ctx);
        return 1;
    }
    return 0;
}

static int mir_verify_inst_value_ids(MirFunction* func, MirInst* inst, FILE* diag) {
    int e = 0;
    switch (inst->opcode) {
        case MIR_ADD: case MIR_SUB: case MIR_MUL: case MIR_DIV: case MIR_MOD:
        case MIR_FADD: case MIR_FSUB: case MIR_FMUL: case MIR_FDIV:
        case MIR_BAND: case MIR_BOR: case MIR_BXOR: case MIR_SHL: case MIR_SHR:
        case MIR_USHR:
        case MIR_CMP_EQ: case MIR_CMP_NE: case MIR_CMP_LT: case MIR_CMP_LE:
        case MIR_CMP_GT: case MIR_CMP_GE:
        case MIR_LOGIC_AND: case MIR_LOGIC_OR:
            e += mir_verify_value_ids_operand(diag, func, inst->as.binary.lhs, "binary lhs");
            e += mir_verify_value_ids_operand(diag, func, inst->as.binary.rhs, "binary rhs");
            break;
        case MIR_NEG: case MIR_FNEG: case MIR_BNOT: case MIR_LOGIC_NOT:
        case MIR_CAST: case MIR_BITCAST: case MIR_TRUNC:
        case MIR_ZEXT: case MIR_SEXT: case MIR_SITOFP: case MIR_FPTOSI:
            e += mir_verify_value_ids_operand(diag, func, inst->as.unary.operand, "unary");
            break;
        case MIR_LOAD:
            e += mir_verify_value_ids_operand(diag, func, inst->as.mem.ptr, "load ptr");
            break;
        case MIR_STORE:
            e += mir_verify_value_ids_operand(diag, func, inst->as.mem.ptr, "store ptr");
            e += mir_verify_value_ids_operand(diag, func, inst->as.mem.value, "store val");
            break;
        case MIR_GET_FIELD_PTR:
        case MIR_GET_ELEM_PTR:
            e += mir_verify_value_ids_operand(diag, func, inst->as.gep.base, "gep base");
            e += mir_verify_value_ids_operand(diag, func, inst->as.gep.index, "gep index");
            break;
        case MIR_CONDBR:
            e += mir_verify_value_ids_operand(diag, func, inst->as.condbr.cond, "cond");
            break;
        case MIR_RET:
            e += mir_verify_value_ids_operand(diag, func, inst->as.ret.value, "ret val");
            break;
        case MIR_SWITCH:
            e += mir_verify_value_ids_operand(diag, func, inst->as.sw.discriminant, "switch disc");
            break;
        case MIR_CALL:
        case MIR_CALL_INDIRECT:
            e += mir_verify_value_ids_operand(diag, func, inst->as.call.callee, "call callee");
            for (int i = 0; i < inst->as.call.n_args; i++) {
                e += mir_verify_value_ids_operand(diag, func, inst->as.call.args[i], "call arg");
            }
            break;
        case MIR_CALL_VIRTUAL:
            e += mir_verify_value_ids_operand(diag, func, inst->as.vcall.self_obj, "vcall self");
            for (int i = 0; i < inst->as.vcall.n_args; i++) {
                e += mir_verify_value_ids_operand(diag, func, inst->as.vcall.args[i], "vcall arg");
            }
            break;
        case MIR_PHI:
            for (int i = 0; i < inst->as.phi.n_edges; i++) {
                e += mir_verify_value_ids_operand(diag, func, inst->as.phi.edges[i].value, "phi edge");
            }
            break;
        case MIR_COPY: case MIR_BORROW: case MIR_BORROW_MUT: case MIR_MOVE:
            e += mir_verify_value_ids_operand(diag, func, inst->as.transfer.source, "transfer src");
            break;
        case MIR_ARC_RETAIN: case MIR_ARC_RELEASE: case MIR_DROP:
            e += mir_verify_value_ids_operand(diag, func, inst->as.refop.ptr, "refop ptr");
            break;
        case MIR_STRUCT_INIT:
            for (int i = 0; i < inst->as.struct_init.n_fields; i++) {
                e += mir_verify_value_ids_operand(diag, func, inst->as.struct_init.fields[i], "struct field");
            }
            break;
        case MIR_EXTRACT:
            e += mir_verify_value_ids_operand(diag, func, inst->as.field_op.aggregate, "extract agg");
            break;
        case MIR_INSERT:
            e += mir_verify_value_ids_operand(diag, func, inst->as.field_op.aggregate, "insert agg");
            e += mir_verify_value_ids_operand(diag, func, inst->as.field_op.insert_val, "insert val");
            break;
        case MIR_SUSPEND:
            e += mir_verify_value_ids_operand(diag, func, inst->as.suspend.future, "suspend future");
            break;
        case MIR_VEC_LOAD: case MIR_VEC_LOAD_UNALIGNED:
        case MIR_VEC_STORE: case MIR_VEC_STORE_UNALIGNED:
        case MIR_VEC_BROADCAST: case MIR_VEC_REDUCE_SUM:
        case MIR_VEC_ADD: case MIR_VEC_SUB: case MIR_VEC_MUL: case MIR_VEC_DIV:
        case MIR_VEC_MIN: case MIR_VEC_MAX:
        case MIR_VEC_AND: case MIR_VEC_OR:  case MIR_VEC_XOR:
        case MIR_VEC_FMA: case MIR_VEC_DOT:
        case MIR_VEC_CMP_EQ: case MIR_VEC_CMP_LT: case MIR_VEC_CMP_GT:
        case MIR_VEC_SELECT:
            e += mir_verify_value_ids_operand(diag, func, inst->as.vec.a, "vec a");
            e += mir_verify_value_ids_operand(diag, func, inst->as.vec.b, "vec b");
            e += mir_verify_value_ids_operand(diag, func, inst->as.vec.c, "vec c");
            break;
        default:
            break;
    }
    return e;
}

static int mir_verify_inst_types(MirFunction* func, MirInst* inst, FILE* diag) {
    int e = 0;
    switch (inst->opcode) {
        case MIR_ADD: case MIR_SUB: case MIR_MUL: case MIR_DIV: case MIR_MOD:
        case MIR_BAND: case MIR_BOR: case MIR_BXOR: case MIR_SHL: case MIR_SHR:
        case MIR_USHR: {
            MirType* a = mir_function_value_type(func, inst->as.binary.lhs);
            MirType* b = mir_function_value_type(func, inst->as.binary.rhs);
            if (a && b && (!mir_type_is_integer(a) || !mir_type_is_integer(b))) {
                mir_verror(diag, "@%s: integer opcode %d with non-integer operand type",
                           func->name ? func->name : "?", (int)inst->opcode);
                e++;
            }
            break;
        }
        case MIR_FADD: case MIR_FSUB: case MIR_FMUL: case MIR_FDIV: {
            MirType* a = mir_function_value_type(func, inst->as.binary.lhs);
            MirType* b = mir_function_value_type(func, inst->as.binary.rhs);
            if (a && b && (!mir_type_is_float(a) || !mir_type_is_float(b))) {
                mir_verror(diag, "@%s: float opcode %d with non-float operand type",
                           func->name ? func->name : "?", (int)inst->opcode);
                e++;
            }
            break;
        }
        /* LOAD / STORE pointee vs value typing is not always mirrored 1:1 in
         * lowered MIR (e.g. method lowering, wide stores).  Operand SSA ids are
         * still checked in mir_verify_inst_value_ids. */
        case MIR_LOAD:
        case MIR_STORE:
            break;
        case MIR_VEC_LOAD: case MIR_VEC_LOAD_UNALIGNED:
        case MIR_VEC_STORE: case MIR_VEC_STORE_UNALIGNED:
        case MIR_VEC_BROADCAST: case MIR_VEC_REDUCE_SUM:
        case MIR_VEC_ADD: case MIR_VEC_SUB: case MIR_VEC_MUL: case MIR_VEC_DIV:
        case MIR_VEC_MIN: case MIR_VEC_MAX:
        case MIR_VEC_AND: case MIR_VEC_OR:  case MIR_VEC_XOR:
        case MIR_VEC_FMA: case MIR_VEC_DOT:
        case MIR_VEC_CMP_EQ: case MIR_VEC_CMP_LT: case MIR_VEC_CMP_GT:
        case MIR_VEC_SELECT: {
            MirType* lt = inst->as.vec.lane_type;
            if (!lt || inst->as.vec.width < 0) {
                mir_verror(diag, "@%s: VEC_* missing lane_type or invalid width",
                           func->name ? func->name : "?");
                e++;
            } else if (!mir_type_is_integer(lt) && !mir_type_is_float(lt)) {
                mir_verror(diag, "@%s: VEC_* lane_type must be scalar primitive",
                           func->name ? func->name : "?");
                e++;
            }
            break;
        }
        default:
            break;
    }
    return e;
}

static void mir_collect_term_succs(MirInst* t, MirBlock** buf, int* n, int cap) {
    if (!t || *n >= cap) return;
    switch (t->opcode) {
        case MIR_BR:
            if (t->as.br.target) buf[(*n)++] = t->as.br.target;
            break;
        case MIR_CONDBR:
            if (t->as.condbr.true_bb)  buf[(*n)++] = t->as.condbr.true_bb;
            if (t->as.condbr.false_bb) buf[(*n)++] = t->as.condbr.false_bb;
            break;
        case MIR_SWITCH:
            for (int i = 0; i < t->as.sw.n_cases && *n < cap; i++) {
                if (t->as.sw.targets[i]) buf[(*n)++] = t->as.sw.targets[i];
            }
            if (*n < cap && t->as.sw.default_bb)
                buf[(*n)++] = t->as.sw.default_bb;
            break;
        default:
            break;
    }
}

static int mir_verify_cfg_reachable(MirFunction* func, FILE* diag) {
    int nbl = func->block_count;
    if (nbl <= 0) return 0;

    bool* seen = (bool*)calloc((size_t)nbl, sizeof(bool));
    if (!seen) return 0;

    MirBlock** q = (MirBlock**)malloc((size_t)nbl * sizeof(MirBlock*));
    if (!q) { free(seen); return 0; }

    int qh = 0, qt = 0;
    if (func->entry_block) {
        q[qt++] = func->entry_block;
        seen[func->entry_block->id] = true;
    }

    MirBlock* succ_buf[64];
    while (qh < qt) {
        MirBlock* cur = q[qh++];
        MirInst* term = cur->last;
        int ns = 0;
        mir_collect_term_succs(term, succ_buf, &ns, 64);
        for (int i = 0; i < ns; i++) {
            MirBlock* s = succ_buf[i];
            if (!s) continue;
            if (s->id < (uint32_t)nbl && !seen[s->id]) {
                seen[s->id] = true;
                q[qt++] = s;
            }
        }
    }
    free(q);

    int err = 0;
    for (MirBlock* b = func->block_list; b; b = b->next_block) {
        if (b->id < (uint32_t)nbl && !seen[b->id]) {
            mir_verror(diag, "@%s: orphaned basic block bb%u (%s) (not reachable from entry)",
                       func->name ? func->name : "?", b->id,
                       b->label ? b->label : "?");
            err++;
        }
    }
    free(seen);
    return err;
}

static int mir_verify_block_shape(MirFunction* func, MirBlock* block, FILE* diag) {
    int errors = 0;
    if (!mir_block_is_terminated(block)) {
        mir_verror(diag, "@%s: bb%u (%s) is not terminated",
                   func->name ? func->name : "?", block->id,
                   block->label ? block->label : "?");
        errors++;
    }
    bool past_phis = false;
    for (MirInst* inst = block->first; inst; inst = inst->next) {
        if (inst->opcode == MIR_PHI) {
            if (past_phis) {
                mir_verror(diag, "@%s: bb%u has PHI after non-PHI instruction",
                           func->name ? func->name : "?", block->id);
                errors++;
            }
        } else {
            past_phis = true;
        }
        bool is_term = (inst->opcode == MIR_BR || inst->opcode == MIR_CONDBR ||
                        inst->opcode == MIR_SWITCH || inst->opcode == MIR_RET ||
                        inst->opcode == MIR_RET_VOID || inst->opcode == MIR_UNREACHABLE);
        if (is_term && inst->next != NULL) {
            mir_verror(diag, "@%s: bb%u has instructions after terminator",
                       func->name ? func->name : "?", block->id);
            errors++;
        }
    }
    for (MirInst* inst = block->first; inst && inst->opcode == MIR_PHI; inst = inst->next) {
        if (inst->as.phi.n_edges != block->pred_count) {
            mir_verror(diag, "@%s: bb%u phi %%%u has %d edges but %d predecessors",
                       func->name ? func->name : "?", block->id,
                       inst->result, inst->as.phi.n_edges, block->pred_count);
            errors++;
        }
    }
    return errors;
}

int mir_verify_function(MirFunction* func, FILE* diag) {
    int errors = 0;
    if (!func || func->is_extern) return 0;

    if (!func->entry_block) {
        mir_verror(diag, "function @%s has no entry block", func->name ? func->name : "?");
        return 1;
    }
    if (func->entry_block->pred_count > 0) {
        mir_verror(diag, "function @%s entry block has predecessors", func->name ? func->name : "?");
        errors++;
    }

    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        errors += mir_verify_block_shape(func, bb, diag);
        for (MirInst* inst = bb->first; inst; inst = inst->next) {
            errors += mir_verify_inst_value_ids(func, inst, diag);
            errors += mir_verify_inst_types(func, inst, diag);
        }
    }
    errors += mir_verify_cfg_reachable(func, diag);
    return errors;
}

int mir_verify_module(MirModule* module, FILE* diag) {
    if (!module) return 0;
    int errors = 0;
    for (MirFunction* f = module->func_list; f; f = f->next_func) {
        errors += mir_verify_function(f, diag);
    }
    MirBorrowChecker bc;
    mir_borrow_init(&bc, module);
    int berr = mir_borrow_check_module(&bc, module);
    if (berr > 0) {
        if (diag) mir_borrow_print_errors(&bc, diag);
        errors += berr;
    }
    mir_borrow_destroy(&bc);
    return errors;
}

/* Statically detectable ownership-linearity violations: the same SSA value
 * is consumed (RC_DEC, MOVE, FREE, DROP) more than once on a straight-line
 * path within a single basic block. Cross-block flow requires the borrow
 * checker; this catches the obvious in-block double-consume that any
 * optimiser pass might introduce by accident. */
int mir_check_ownership_function(MirFunction* func, FILE* diag) {
    int errors = 0;
    if (!func || func->is_extern) return 0;

    for (MirBlock* bb = func->block_list; bb; bb = bb->next_block) {
        for (MirInst* a = bb->first; a; a = a->next) {
            MirValueId consumed = MIR_VALUE_NONE;
            switch (a->opcode) {
                case MIR_ARC_RELEASE: consumed = a->as.unary.operand; break;
                case MIR_MOVE:        consumed = a->as.unary.operand; break;
                case MIR_DROP:        consumed = a->as.unary.operand; break;
                default: break;
            }
            if (consumed == MIR_VALUE_NONE) continue;

            for (MirInst* b = a->next; b; b = b->next) {
                MirValueId other = MIR_VALUE_NONE;
                switch (b->opcode) {
                    case MIR_ARC_RELEASE: other = b->as.unary.operand; break;
                    case MIR_MOVE:        other = b->as.unary.operand; break;
                    case MIR_DROP:        other = b->as.unary.operand; break;
                    default: break;
                }
                if (other == consumed) {
                    mir_verror(diag,
                        "@%s: bb%u value %%%u consumed twice (linearity)",
                        func->name ? func->name : "?", bb->id, consumed);
                    errors++;
                    break;
                }
            }
        }
    }
    return errors;
}
