#include "tensor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <malloc.h>
#define cpx_aligned_alloc(a, s) _aligned_malloc((s), (a))
#define cpx_aligned_free(p) _aligned_free(p)
#else
static void* cpx_aligned_alloc(size_t alignment, size_t size) {
    size_t rounded = (size + alignment - 1) & ~(alignment - 1);
    return aligned_alloc(alignment, rounded);
}
#define cpx_aligned_free(p) free(p)
#endif

static void heap_delete(void* data, void* context) { (void)context; cpx_aligned_free(data); }

static TensorStorage* storage_new(void* data, size_t bytes) {
    TensorStorage* s = (TensorStorage*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->data = data; s->bytes = bytes; s->refs = 1; s->deleter = heap_delete;
    return s;
}

static void storage_retain(TensorStorage* s) { if (s) s->refs++; }
static void storage_release(TensorStorage* s) {
    if (!s || --s->refs) return;
    if (s->deleter) s->deleter(s->data, s->deleter_context);
    free(s);
}

bool tensor_shape_valid(i32 ndim, const i32* shape, size_t* count_out) {
    if (!shape || ndim <= 0 || ndim > MAX_TENSOR_DIM) return false;
    size_t count = 1;
    for (i32 i = 0; i < ndim; i++) {
        if (shape[i] <= 0 || count > SIZE_MAX / (size_t)shape[i]) return false;
        count *= (size_t)shape[i];
    }
    if (count_out) *count_out = count;
    return true;
}

static void init_layout(Tensor* t, i32 ndim, const i32* shape, size_t count) {
    memset(t->shape, 0, sizeof(t->shape));
    memset(t->stride, 0, sizeof(t->stride));
    t->ndim = ndim; t->size = count; t->dtype = TENSOR_F32; t->is_contiguous = true;
    i32 stride = 1;
    for (i32 i = ndim - 1; i >= 0; i--) {
        t->shape[i] = shape[i]; t->stride[i] = stride; stride *= shape[i];
    }
}

TensorArena* tensor_arena_create(size_t capacity) {
    if (!capacity) capacity = TENSOR_ALIGNMENT;
    TensorArena* a = (TensorArena*)calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->buffer = (u8*)cpx_aligned_alloc(TENSOR_ALIGNMENT, capacity);
    if (!a->buffer) { free(a); return NULL; }
    a->capacity = capacity;
    return a;
}

void* tensor_arena_alloc(TensorArena* a, size_t size, size_t alignment) {
    if (!a || !alignment || (alignment & (alignment - 1))) return NULL;
    size_t offset = (a->offset + alignment - 1) & ~(alignment - 1);
    if (offset > a->capacity || size > a->capacity - offset) {
        fprintf(stderr, "TensorArena out of memory (%llu byte request)\n", (unsigned long long)size);
        return NULL;
    }
    void* result = a->buffer + offset;
    a->offset = offset + size;
    if (a->offset > a->peak_usage) a->peak_usage = a->offset;
    return result;
}

Tensor* tensor_arena_alloc_tensor(TensorArena* a, i32 ndim, const i32* shape) {
    size_t count;
    if (!tensor_shape_valid(ndim, shape, &count)) return NULL;
    Tensor* t = (Tensor*)tensor_arena_alloc(a, sizeof(*t), 8);
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->data = (f32*)tensor_arena_alloc(a, count * sizeof(f32), TENSOR_ALIGNMENT);
    if (!t->data) return NULL;
    memset(t->data, 0, count * sizeof(f32));
    init_layout(t, ndim, shape, count);
    return t;
}

void tensor_arena_reset(TensorArena* a) { if (a) a->offset = 0; }
void tensor_arena_destroy(TensorArena* a) {
    if (!a) return;
    cpx_aligned_free(a->buffer);
    free(a);
}

Tensor* tensor_empty(i32 ndim, const i32* shape) {
    size_t count;
    if (!tensor_shape_valid(ndim, shape, &count)) return NULL;
    Tensor* t = (Tensor*)calloc(1, sizeof(*t));
    if (!t) return NULL;
    size_t bytes = count * sizeof(f32);
    void* data = cpx_aligned_alloc(TENSOR_ALIGNMENT, bytes);
    if (!data) { free(t); return NULL; }
    t->storage = storage_new(data, bytes);
    if (!t->storage) { cpx_aligned_free(data); free(t); return NULL; }
    t->data = (f32*)data; t->owns_memory = true;
    init_layout(t, ndim, shape, count);
    return t;
}

Tensor* tensor_create(i32 ndim, const i32* shape) {
    Tensor* tensor = tensor_empty(ndim, shape);
    if (tensor) memset(tensor->data, 0, tensor_nbytes(tensor));
    return tensor;
}

Tensor* tensor_create_from_data(i32 ndim, const i32* shape, const f32* data) {
    Tensor* t = tensor_create(ndim, shape);
    if (t && data) memcpy(t->data, data, tensor_nbytes(t));
    return t;
}

Tensor* tensor_view(Tensor* parent, i32 offset) {
    if (!parent || offset < 0 || (size_t)offset > parent->size) return NULL;
    Tensor* view = (Tensor*)malloc(sizeof(*view));
    if (!view) return NULL;
    *view = *parent;
    view->data += offset; view->storage_offset += (size_t)offset;
    view->size -= (size_t)offset; view->owns_memory = false;
    storage_retain(view->storage);
    return view;
}

Tensor* tensor_reshape_view(Tensor* parent, i32 ndim, const i32* shape) {
    size_t count;
    if (!parent || !tensor_is_contiguous(parent) ||
        !tensor_shape_valid(ndim, shape, &count) || count != parent->size) return NULL;
    Tensor* view = tensor_view(parent, 0);
    if (view) { init_layout(view, ndim, shape, count); view->owns_memory = false; }
    return view;
}

Tensor* tensor_slice_view(Tensor* parent, i32 dim, i32 start, i32 length) {
    if (!parent || dim < 0 || dim >= parent->ndim || start < 0 || length <= 0 ||
        start + length > parent->shape[dim]) return NULL;
    Tensor* view = tensor_view(parent, start * parent->stride[dim]);
    if (!view) return NULL;
    view->shape[dim] = length;
    view->size = parent->size / (size_t)parent->shape[dim] * (size_t)length;
    view->is_contiguous = dim == 0 || (start == 0 && length == parent->shape[dim]);
    return view;
}

Tensor* tensor_clone(const Tensor* source) {
    if (!source) return NULL;
    Tensor* clone = tensor_create(source->ndim, source->shape);
    if (clone) tensor_copy(source, clone);
    return clone;
}

void tensor_destroy(Tensor* t) {
    if (!t) return;
    if (t->storage) storage_release(t->storage);
    else if (t->owns_memory) cpx_aligned_free(t->data);
    free(t);
}

bool tensor_is_contiguous(const Tensor* t) {
    if (!t || !t->is_contiguous) return false;
    i32 expected = 1;
    for (i32 i = t->ndim - 1; i >= 0; i--) {
        if (t->stride[i] != expected) return false;
        expected *= t->shape[i];
    }
    return true;
}

size_t tensor_nbytes(const Tensor* t) { return t ? t->size * sizeof(f32) : 0; }

static size_t logical_offset(const Tensor* t, size_t linear) {
    size_t offset = 0;
    for (i32 dim = t->ndim - 1; dim >= 0; dim--) {
        size_t coordinate = linear % (size_t)t->shape[dim];
        linear /= (size_t)t->shape[dim];
        offset += coordinate * (size_t)t->stride[dim];
    }
    return offset;
}

void tensor_zeros(Tensor* t) {
    if (!t) return;
    if (tensor_is_contiguous(t)) memset(t->data, 0, tensor_nbytes(t));
    else for (size_t i = 0; i < t->size; i++) t->data[logical_offset(t, i)] = 0;
}
void tensor_fill(Tensor* t, f32 value) {
    if (t) for (size_t i = 0; i < t->size; i++) t->data[logical_offset(t, i)] = value;
}
void tensor_copy(const Tensor* src, Tensor* dst) {
    assert(src && dst && src->size == dst->size);
    if (tensor_is_contiguous(src) && tensor_is_contiguous(dst)) {
        memcpy(dst->data, src->data, tensor_nbytes(src));
    } else {
        for (size_t i = 0; i < src->size; i++)
            dst->data[logical_offset(dst, i)] = src->data[logical_offset(src, i)];
    }
}

static size_t element_offset(const Tensor* t, const i32* indices) {
    size_t offset = 0;
    for (i32 i = 0; i < t->ndim; i++) {
        assert(indices[i] >= 0 && indices[i] < t->shape[i]);
        offset += (size_t)indices[i] * (size_t)t->stride[i];
    }
    return offset;
}
f32 tensor_get(const Tensor* t, const i32* indices) { return t->data[element_offset(t, indices)]; }
void tensor_set(Tensor* t, const i32* indices, f32 value) { t->data[element_offset(t, indices)] = value; }

MemoryPools* mempool_create(i32 n, size_t act, size_t grad) {
    if (n < 0) return NULL;
    MemoryPools* p = (MemoryPools*)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->num_params = n;
    if (n) {
        p->params = (Tensor**)calloc((size_t)n, sizeof(Tensor*));
        p->param_grads = (Tensor**)calloc((size_t)n, sizeof(Tensor*));
        p->adam_m = (Tensor**)calloc((size_t)n, sizeof(Tensor*));
        p->adam_v = (Tensor**)calloc((size_t)n, sizeof(Tensor*));
    }
    p->activations = tensor_arena_create(act);
    p->grad_cache = tensor_arena_create(grad);
    if ((n && (!p->params || !p->param_grads || !p->adam_m || !p->adam_v)) ||
        !p->activations || !p->grad_cache) { mempool_destroy(p); return NULL; }
    return p;
}

void mempool_destroy(MemoryPools* p) {
    if (!p) return;
    for (i32 i = 0; i < p->num_params; i++) {
        tensor_destroy(p->params ? p->params[i] : NULL);
        tensor_destroy(p->param_grads ? p->param_grads[i] : NULL);
        tensor_destroy(p->adam_m ? p->adam_m[i] : NULL);
        tensor_destroy(p->adam_v ? p->adam_v[i] : NULL);
    }
    free(p->params); free(p->param_grads); free(p->adam_m); free(p->adam_v);
    tensor_arena_destroy(p->activations); tensor_arena_destroy(p->grad_cache); free(p);
}

void mempool_reset_step(MemoryPools* p) {
    if (p) { tensor_arena_reset(p->activations); tensor_arena_reset(p->grad_cache); }
}
