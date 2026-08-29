/*
 * LLM Runtime - Core Tensor Library
 * 
 * Production-grade tensor operations for CPU-only LLM training/inference
 * Target: x86-64, AVX2+FMA, no GPU
 */

#ifndef LLM_TENSOR_H
#define LLM_TENSOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Scalar types
typedef float    f32;
typedef uint16_t f16; 
typedef int32_t  i32;
typedef int8_t   i8;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

// Maximum tensor dimensions (sufficient for transformers)
#define MAX_TENSOR_DIM 4

// Alignment for SIMD (64 bytes = cache line)
#define TENSOR_ALIGNMENT 64

// Tensor representation
typedef struct Tensor {
    f32*   data;              // Aligned data pointer
    i32    ndim;              // Number of dimensions (1-4)
    i32    shape[MAX_TENSOR_DIM];   // Dimension sizes
    i32    stride[MAX_TENSOR_DIM];  // Stride in elements (not bytes)
    size_t size;              // Total number of elements
    bool   owns_memory;       // If true, tensor owns data and will free it
} Tensor;

// TensorArena allocator for per-step memory
typedef struct TensorArena {
    u8*    buffer;            // Pre-allocated memory pool
    size_t capacity;          // Total size in bytes
    size_t offset;            // Current allocation offset
    size_t peak_usage;        // Peak memory usage (for profiling)
} TensorArena;

// Memory pools for training
typedef struct MemoryPools {
    // Long-lived (persist across training)
    Tensor** params;          // Model parameters
    Tensor** param_grads;     // Parameter gradients
    i32      num_params;
    
    // Per-step arenas (reset after each step)
    TensorArena*   activations;     // Forward pass activations
    TensorArena*   grad_cache;      // Backward pass gradients
    
    // Optimizer state
    Tensor** adam_m;          // First moment
    Tensor** adam_v;          // Second moment
    i32      adam_t;          // Timestep
} MemoryPools;

// ===== ARENA ALLOCATOR =====

// Create arena with specified capacity
TensorArena* tensor_arena_create(size_t capacity);

// Allocate aligned memory from arena
void* tensor_arena_alloc(TensorArena* arena, size_t size, size_t alignment);

// Allocate tensor from arena
Tensor* tensor_arena_alloc_tensor(TensorArena* arena, i32 ndim, const i32* shape);

// Reset arena (reuse memory)
void tensor_arena_reset(TensorArena* arena);

// Destroy arena
void tensor_arena_destroy(TensorArena* arena);

// ===== TENSOR OPERATIONS =====

// Create tensor with owned memory
Tensor* tensor_create(i32 ndim, const i32* shape);

// Create tensor view (does not own memory)
Tensor* tensor_view(Tensor* parent, i32 offset);

// Destroy tensor (frees memory if owned)
void tensor_destroy(Tensor* tensor);

// Fill tensor with zeros
void tensor_zeros(Tensor* tensor);

// Fill tensor with constant value
void tensor_fill(Tensor* tensor, f32 value);

// Copy tensor data
void tensor_copy(const Tensor* src, Tensor* dst);

// Get element at index (slow, for debugging)
f32 tensor_get(const Tensor* tensor, const i32* indices);

// Set element at index (slow, for debugging)
void tensor_set(Tensor* tensor, const i32* indices, f32 value);

// ===== MEMORY POOL MANAGEMENT =====

// Initialize memory pools for training
MemoryPools* mempool_create(i32 num_params, size_t activation_capacity, size_t grad_capacity);

// Destroy memory pools
void mempool_destroy(MemoryPools* pool);

// Reset per-step arenas
void mempool_reset_step(MemoryPools* pool);

#endif // LLM_TENSOR_H
