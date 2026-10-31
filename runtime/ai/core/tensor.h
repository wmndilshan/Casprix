/* Shared tensor ABI for Casprix autograd, neural-network and LLM code. */
#ifndef CPX_AI_CORE_TENSOR_H
#define CPX_AI_CORE_TENSOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef float f32;
typedef uint16_t f16;
typedef int32_t i32;
typedef int8_t i8;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define MAX_TENSOR_DIM 8
#define TENSOR_ALIGNMENT 64

typedef enum TensorDType { TENSOR_F32 = 0 } TensorDType;
typedef void (*TensorStorageDeleter)(void* data, void* context);

typedef struct TensorStorage {
    void* data;
    size_t bytes;
    size_t refs;
    TensorStorageDeleter deleter;
    void* deleter_context;
} TensorStorage;

/* Original fields stay first for source and ABI compatibility. */
typedef struct Tensor {
    f32* data;
    i32 ndim;
    i32 shape[MAX_TENSOR_DIM];
    i32 stride[MAX_TENSOR_DIM];
    size_t size;
    bool owns_memory;
    TensorDType dtype;
    size_t storage_offset;
    TensorStorage* storage;
    bool is_contiguous;
} Tensor;

typedef struct TensorArena {
    u8* buffer;
    size_t capacity;
    size_t offset;
    size_t peak_usage;
} TensorArena;

typedef struct MemoryPools {
    Tensor** params;
    Tensor** param_grads;
    i32 num_params;
    TensorArena* activations;
    TensorArena* grad_cache;
    Tensor** adam_m;
    Tensor** adam_v;
    i32 adam_t;
} MemoryPools;

TensorArena* tensor_arena_create(size_t capacity);
void* tensor_arena_alloc(TensorArena* arena, size_t size, size_t alignment);
Tensor* tensor_arena_alloc_tensor(TensorArena* arena, i32 ndim, const i32* shape);
void tensor_arena_reset(TensorArena* arena);
void tensor_arena_destroy(TensorArena* arena);

Tensor* tensor_create(i32 ndim, const i32* shape);
/* Uninitialized allocation for kernels that overwrite every element. */
Tensor* tensor_empty(i32 ndim, const i32* shape);
Tensor* tensor_create_from_data(i32 ndim, const i32* shape, const f32* data);
Tensor* tensor_view(Tensor* parent, i32 offset);
Tensor* tensor_reshape_view(Tensor* parent, i32 ndim, const i32* shape);
Tensor* tensor_slice_view(Tensor* parent, i32 dim, i32 start, i32 length);
Tensor* tensor_clone(const Tensor* source);
void tensor_destroy(Tensor* tensor);
bool tensor_shape_valid(i32 ndim, const i32* shape, size_t* element_count);
bool tensor_is_contiguous(const Tensor* tensor);
size_t tensor_nbytes(const Tensor* tensor);
void tensor_zeros(Tensor* tensor);
void tensor_fill(Tensor* tensor, f32 value);
void tensor_copy(const Tensor* src, Tensor* dst);
f32 tensor_get(const Tensor* tensor, const i32* indices);
void tensor_set(Tensor* tensor, const i32* indices, f32 value);

MemoryPools* mempool_create(i32 num_params, size_t activation_capacity, size_t grad_capacity);
void mempool_destroy(MemoryPools* pool);
void mempool_reset_step(MemoryPools* pool);

#endif
