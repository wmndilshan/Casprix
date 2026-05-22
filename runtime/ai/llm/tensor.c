/*
 * LLM Runtime - Core Tensor Implementation
 */

#include "tensor.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#ifdef _WIN32
#include <malloc.h>
#define aligned_alloc(alignment, size) _aligned_malloc(size, alignment)
#define aligned_free(ptr) _aligned_free(ptr)
#else
#define aligned_free(ptr) free(ptr)
#endif

// ===== ARENA ALLOCATOR =====

TensorArena* tensor_arena_create(size_t capacity) {
    TensorArena* arena = (TensorArena*)malloc(sizeof(TensorArena));
    if (!arena) return NULL;
    
    // Allocate aligned buffer
    arena->buffer = (u8*)aligned_alloc(TENSOR_ALIGNMENT, capacity);
    if (!arena->buffer) {
        free(arena);
        return NULL;
    }
    
    arena->capacity = capacity;
    arena->offset = 0;
    arena->peak_usage = 0;
    
    return arena;
}

void* tensor_arena_alloc(TensorArena* arena, size_t size, size_t alignment) {
    // Align offset
    size_t aligned_offset = (arena->offset + alignment - 1) & ~(alignment - 1);
    
    if (aligned_offset + size > arena->capacity) {
        fprintf(stderr, "TensorArena out of memory: requested %llu bytes, only %llu available\n",
                (unsigned long long)size, (unsigned long long)(arena->capacity - aligned_offset));
        return NULL;
    }
    
    void* ptr = arena->buffer + aligned_offset;
    arena->offset = aligned_offset + size;
    
    // Track peak usage
    if (arena->offset > arena->peak_usage) {
        arena->peak_usage = arena->offset;
    }
    
    return ptr;
}

Tensor* tensor_arena_alloc_tensor(TensorArena* arena, i32 ndim, const i32* shape) {
    assert(ndim > 0 && ndim <= MAX_TENSOR_DIM);
    
    // Allocate tensor struct from arena
    Tensor* tensor = (Tensor*)tensor_arena_alloc(arena, sizeof(Tensor), 8);
    if (!tensor) return NULL;
    
    // Calculate total size
    size_t total_elements = 1;
    for (i32 i = 0; i < ndim; i++) {
        total_elements *= shape[i];
    }
    
    // Allocate data from arena
    size_t byte_size = total_elements * sizeof(f32);
    tensor->data = (f32*)tensor_arena_alloc(arena, byte_size, TENSOR_ALIGNMENT);
    if (!tensor->data) {
        free(tensor);
        return NULL;
    }
    
    // Set metadata
    tensor->ndim = ndim;
    tensor->size = total_elements;
    tensor->owns_memory = false; // TensorArena owns it
    
    // Copy shape and compute stride (row-major)
    i32 stride = 1;
    for (i32 i = ndim - 1; i >= 0; i--) {
        tensor->shape[i] = shape[i];
        tensor->stride[i] = stride;
        stride *= shape[i];
    }
    
    return tensor;
}

void tensor_arena_reset(TensorArena* arena) {
    arena->offset = 0;
}

void tensor_arena_destroy(TensorArena* arena) {
    if (arena) {
        aligned_free(arena->buffer);
        free(arena);
    }
}

// ===== TENSOR OPERATIONS =====

Tensor* tensor_create(i32 ndim, const i32* shape) {
    assert(ndim > 0 && ndim <= MAX_TENSOR_DIM);
    
    Tensor* tensor = (Tensor*)malloc(sizeof(Tensor));
    if (!tensor) return NULL;
    
    // Calculate total size
    size_t total_elements = 1;
    for (i32 i = 0; i < ndim; i++) {
        total_elements *= shape[i];
    }
    
    // Allocate aligned data
    size_t byte_size = total_elements * sizeof(f32);
    tensor->data = (f32*)aligned_alloc(TENSOR_ALIGNMENT, byte_size);
    if (!tensor->data) {
        free(tensor);
        return NULL;
    }
    
    tensor->ndim = ndim;
    tensor->size = total_elements;
    tensor->owns_memory = true;
    
    // Copy shape and compute stride
    i32 stride = 1;
    for (i32 i = ndim - 1; i >= 0; i--) {
        tensor->shape[i] = shape[i];
        tensor->stride[i] = stride;
        stride *= shape[i];
    }
    
    return tensor;
}

Tensor* tensor_view(Tensor* parent, i32 offset) {
    Tensor* view = (Tensor*)malloc(sizeof(Tensor));
    if (!view) return NULL;
    
    // Copy metadata
    *view = *parent;
    
    // Offset data pointer
    view->data = parent->data + offset;
    view->owns_memory = false; // View doesn't own memory
    
    return view;
}

void tensor_destroy(Tensor* tensor) {
    if (tensor) {
        if (tensor->owns_memory) {
            aligned_free(tensor->data);
        }
        free(tensor);
    }
}

void tensor_zeros(Tensor* tensor) {
    memset(tensor->data, 0, tensor->size * sizeof(f32));
}

void tensor_fill(Tensor* tensor, f32 value) {
    for (size_t i = 0; i < tensor->size; i++) {
        tensor->data[i] = value;
    }
}

void tensor_copy(const Tensor* src, Tensor* dst) {
    assert(src->size == dst->size);
    memcpy(dst->data, src->data, src->size * sizeof(f32));
}

f32 tensor_get(const Tensor* tensor, const i32* indices) {
    size_t offset = 0;
    for (i32 i = 0; i < tensor->ndim; i++) {
        offset += indices[i] * tensor->stride[i];
    }
    return tensor->data[offset];
}

void tensor_set(Tensor* tensor, const i32* indices, f32 value) {
    size_t offset = 0;
    for (i32 i = 0; i < tensor->ndim; i++) {
        offset += indices[i] * tensor->stride[i];
    }
    tensor->data[offset] = value;
}

// ===== MEMORY POOL MANAGEMENT =====

MemoryPools* mempool_create(i32 num_params, size_t activation_capacity, size_t grad_capacity) {
    MemoryPools* pool = (MemoryPools*)malloc(sizeof(MemoryPools));
    if (!pool) return NULL;
    
    // Allocate param arrays only if needed
    if (num_params > 0) {
        pool->params = (Tensor**)calloc(num_params, sizeof(Tensor*));
        pool->param_grads = (Tensor**)calloc(num_params, sizeof(Tensor*));
        pool->adam_m = (Tensor**)calloc(num_params, sizeof(Tensor*));
        pool->adam_v = (Tensor**)calloc(num_params, sizeof(Tensor*));
    } else {
        pool->params = NULL;
        pool->param_grads = NULL;
        pool->adam_m = NULL;
        pool->adam_v = NULL;
    }
    pool->num_params = num_params;
    pool->adam_t = 0;
    
    // Create arenas
    pool->activations = tensor_arena_create(activation_capacity);
    pool->grad_cache = tensor_arena_create(grad_capacity);
    
    bool params_ok = (num_params == 0) || (pool->params && pool->param_grads && pool->adam_m && pool->adam_v);
    
    if (!params_ok || !pool->activations || !pool->grad_cache) {
        mempool_destroy(pool);
        return NULL;
    }
    
    return pool;
}

void mempool_destroy(MemoryPools* pool) {
    if (!pool) return;
    
    // Free parameter tensors
    for (i32 i = 0; i < pool->num_params; i++) {
        tensor_destroy(pool->params[i]);
        tensor_destroy(pool->param_grads[i]);
        tensor_destroy(pool->adam_m[i]);
        tensor_destroy(pool->adam_v[i]);
    }
    
    free(pool->params);
    free(pool->param_grads);
    free(pool->adam_m);
    free(pool->adam_v);
    
    tensor_arena_destroy(pool->activations);
    tensor_arena_destroy(pool->grad_cache);
    
    free(pool);
}

void mempool_reset_step(MemoryPools* pool) {
    tensor_arena_reset(pool->activations);
    tensor_arena_reset(pool->grad_cache);
}
