/*
 * Casprix Compiler — MIR Core Implementation
 *
 * Arena allocator, type system, module/function/block management,
 * and all the infrastructure that the builder and passes depend on.
 */

#include "mir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    /* Value type table — initialize before assigning parameter value IDs. */
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
        int new_cap = func->value_type_capacity ? func->value_type_capacity * 2 : 64;
        while ((int)id >= new_cap) {
            new_cap *= 2;
        }
        MirType** new_types = (MirType**)mir_arena_alloc(func->parent->arena,
                               new_cap * sizeof(MirType*));
        if (func->value_types && func->value_type_capacity > 0) {
            memcpy(new_types, func->value_types,
                   func->value_type_capacity * sizeof(MirType*));
        }
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
