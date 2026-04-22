#ifndef CASPRIX_NET_CX_ARENA_H
#define CASPRIX_NET_CX_ARENA_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef struct CxArena {
    uint8_t*       base;
    size_t         capacity;
    _Atomic size_t offset;
} CxArena;

CxArena* cx_arena_create(size_t capacity);
void     cx_arena_destroy(CxArena* arena);
void*    cx_arena_alloc(CxArena* arena, size_t size);
void*    cx_arena_alloc_aligned(CxArena* arena, size_t size, size_t alignment);
void     cx_arena_reset(CxArena* arena);
size_t   cx_arena_used(const CxArena* arena);

#endif
