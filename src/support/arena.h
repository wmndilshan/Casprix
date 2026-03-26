/*
 * Casprix Compiler - Arena Memory Allocator
 * High-performance bump allocator for fast compilation
 */

#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>
#include <stdbool.h>

// Arena configuration
#define ARENA_BLOCK_SIZE (64 * 1024)  // 64KB blocks
#define ARENA_ALIGNMENT 16             // 16-byte alignment

// Memory block
typedef struct ArenaBlock {
    char* memory;
    size_t size;
    size_t used;
    struct ArenaBlock* next;
} ArenaBlock;

// Arena allocator
typedef struct {
    ArenaBlock* current_block;
    ArenaBlock* first_block;
    size_t total_allocated;
    size_t total_used;
    size_t block_count;
} Arena;

// Initialize arena
Arena* arena_create(void);

// Allocate from arena (fast bump allocation)
void* arena_alloc(Arena* arena, size_t size);

// Allocate aligned memory
void* arena_alloc_aligned(Arena* arena, size_t size, size_t alignment);

// Allocate and zero
void* arena_calloc(Arena* arena, size_t count, size_t size);

// Duplicate string in arena
char* arena_strdup(Arena* arena, const char* str);

// Reset arena (reuse all memory)
void arena_reset(Arena* arena);

// Destroy arena (free all memory)
void arena_destroy(Arena* arena);

// Get arena statistics
void arena_stats(Arena* arena, size_t* allocated, size_t* used, size_t* blocks);

// Temporary arena scope (for RAII-style cleanup)
typedef struct {
    Arena* arena;
    ArenaBlock* saved_block;
    size_t saved_used;
} ArenaScope;

ArenaScope arena_scope_begin(Arena* arena);
void arena_scope_end(ArenaScope scope);

#endif // ARENA_H
