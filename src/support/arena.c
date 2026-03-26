/*
 * Casprix Compiler - Arena Memory Allocator Implementation
 */

#include "support/arena.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Helper: Align size to given alignment
static size_t align_size(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

// Create new arena block
static ArenaBlock* create_block(size_t min_size) {
    size_t block_size = min_size > ARENA_BLOCK_SIZE ? min_size : ARENA_BLOCK_SIZE;
    
    ArenaBlock* block = (ArenaBlock*)malloc(sizeof(ArenaBlock));
    if (!block) return NULL;
    
    block->memory = (char*)malloc(block_size);
    if (!block->memory) {
        free(block);
        return NULL;
    }
    
    block->size = block_size;
    block->used = 0;
    block->next = NULL;
    
    return block;
}

// Create arena
Arena* arena_create(void) {
    Arena* arena = (Arena*)malloc(sizeof(Arena));
    if (!arena) return NULL;
    
    arena->first_block = create_block(ARENA_BLOCK_SIZE);
    if (!arena->first_block) {
        free(arena);
        return NULL;
    }
    
    arena->current_block = arena->first_block;
    arena->total_allocated = ARENA_BLOCK_SIZE;
    arena->total_used = 0;
    arena->block_count = 1;
    
    return arena;
}

// Fast bump allocation
void* arena_alloc(Arena* arena, size_t size) {
    if (size == 0) return NULL;
    
    // Align size
    size = align_size(size, ARENA_ALIGNMENT);
    
    ArenaBlock* block = arena->current_block;
    
    // Check if current block has enough space
    if (block->used + size <= block->size) {
        void* ptr = block->memory + block->used;
        block->used += size;
        arena->total_used += size;
        return ptr;
    }
    
    // Need new block
    ArenaBlock* new_block = create_block(size);
    if (!new_block) return NULL;
    
    // Link new block
    block->next = new_block;
    arena->current_block = new_block;
    arena->total_allocated += new_block->size;
    arena->block_count++;
    
    // Allocate from new block
    void* ptr = new_block->memory;
    new_block->used = size;
    arena->total_used += size;
    
    return ptr;
}

// Allocate aligned memory
void* arena_alloc_aligned(Arena* arena, size_t size, size_t alignment) {
    assert(alignment > 0 && (alignment & (alignment - 1)) == 0); // Power of 2
    
    // Add padding for alignment
    size_t padding = alignment - 1;
    void* raw = arena_alloc(arena, size + padding);
    if (!raw) return NULL;
    
    // Align pointer
    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned = align_size(addr, alignment);
    
    return (void*)aligned;
}

// Allocate and zero
void* arena_calloc(Arena* arena, size_t count, size_t size) {
    size_t total = count * size;
    void* ptr = arena_alloc(arena, total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

// Duplicate string
char* arena_strdup(Arena* arena, const char* str) {
    if (!str) return NULL;
    
    size_t len = strlen(str) + 1;
    char* dup = (char*)arena_alloc(arena, len);
    if (dup) {
        memcpy(dup, str, len);
    }
    return dup;
}

// Reset arena (reuse memory)
void arena_reset(Arena* arena) {
    if (!arena) return;
    
    // Reset all blocks
    ArenaBlock* block = arena->first_block;
    while (block) {
        block->used = 0;
        block = block->next;
    }
    
    arena->current_block = arena->first_block;
    arena->total_used = 0;
}

// Destroy arena
void arena_destroy(Arena* arena) {
    if (!arena) return;
    
    // Free all blocks
    ArenaBlock* block = arena->first_block;
    while (block) {
        ArenaBlock* next = block->next;
        free(block->memory);
        free(block);
        block = next;
    }
    
    free(arena);
}

// Get statistics
void arena_stats(Arena* arena, size_t* allocated, size_t* used, size_t* blocks) {
    if (allocated) *allocated = arena->total_allocated;
    if (used) *used = arena->total_used;
    if (blocks) *blocks = arena->block_count;
}

// Scoped arena allocation
ArenaScope arena_scope_begin(Arena* arena) {
    ArenaScope scope;
    scope.arena = arena;
    scope.saved_block = arena->current_block;
    scope.saved_used = arena->current_block->used;
    return scope;
}

void arena_scope_end(ArenaScope scope) {
    // Restore arena state
    ArenaBlock* block = scope.saved_block;
    
    // Free blocks allocated after scope began
    if (block->next) {
        ArenaBlock* next = block->next;
        while (next) {
            ArenaBlock* to_free = next;
            next = next->next;
            
            scope.arena->total_allocated -= to_free->size;
            scope.arena->block_count--;
            
            free(to_free->memory);
            free(to_free);
        }
        block->next = NULL;
    }
    
    // Restore block usage
    size_t freed = block->used - scope.saved_used;
    block->used = scope.saved_used;
    scope.arena->total_used -= freed;
    scope.arena->current_block = block;
}
