// Casperix Runtime - Unified Memory Manager
// Coordinates ARC, arena allocators, cycle collector, and ownership tracking
//
// Memory strategy by object type:
//   - Primitives (i32, f64, bool, etc.): stack-allocated, no GC overhead
//   - Strings: ARC with copy-on-write for strbuf
//   - Structs: stack by default, ARC when heap-allocated (escaped scope)
//   - Classes: always ARC (heap-allocated with vtable)
//   - Enums/Unions: stack by default, ARC for heap variants
//   - Arrays/Slices: ARC for the buffer, elements inline
//   - Closures: ARC (captured environment on heap)
//   - Compiler temporaries: arena-allocated (freed at end of compilation)
//
// The memory manager provides:
//   1. A unified interface for all allocation strategies
//   2. A global cycle collector for ARC objects
//   3. Compiler arena for AST/IR nodes
//   4. Statistics and diagnostics across all subsystems

#ifndef CASPERIX_MEMORY_H
#define CASPERIX_MEMORY_H

#include "arc.h"
#include "ownership.h"
#include "cycle_gc.h"
#include "support/arena.h"

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Memory manager context
typedef struct {
    Arena* compiler_arena;            // Arena for compiler internals (AST, IR)
    CycleCollector* cycle_collector;  // Cycle collector for ARC objects

    // Statistics
    size_t total_arc_objects;         // Lifetime ARC object count
    size_t total_arena_bytes;         // Lifetime arena allocation bytes
    size_t total_stack_allocs;        // Lifetime stack allocation count (estimated)
} MemoryManager;

// --- Lifecycle ---

// Initialize the memory manager (creates arena and cycle collector)
MemoryManager* mem_init(void);

// Initialize with custom cycle collector config
MemoryManager* mem_init_with_config(CycleGCConfig cycle_config);

// Shut down memory manager: runs final cycle collection, destroys arena
// Prints leak report if any ARC objects still alive
void mem_shutdown(MemoryManager* mm);

// --- Compiler arena allocation ---
// Use these for AST nodes, type info, symbol tables - anything that lives
// for the duration of compilation and is freed all at once

// Allocate from compiler arena
void* mem_arena_alloc(MemoryManager* mm, size_t size);

// Allocate and zero from compiler arena
void* mem_arena_calloc(MemoryManager* mm, size_t count, size_t size);

// Duplicate string into compiler arena
char* mem_arena_strdup(MemoryManager* mm, const char* str);

// Reset compiler arena (free all, reuse memory)
void mem_arena_reset(MemoryManager* mm);

// --- ARC allocation (for runtime objects) ---
// Use these for user-created objects that need reference counting

// Allocate an ARC-managed object
void* mem_arc_alloc(MemoryManager* mm, size_t size);

// Allocate with destructor
void* mem_arc_alloc_with_destructor(MemoryManager* mm, size_t size,
                                    arc_destructor_fn destructor);

// Allocate with destructor and cycle scanner
void* mem_arc_alloc_full(MemoryManager* mm, size_t size,
                         arc_destructor_fn destructor,
                         arc_scan_fn scanner);

// Retain an ARC object (increment ref count)
void* mem_arc_retain(MemoryManager* mm, void* obj);

// Release an ARC object (decrement ref count, may free)
// If the object has a scanner and count > 0 after release,
// adds it to cycle collector suspects
void mem_arc_release(MemoryManager* mm, void* obj);

// --- String allocation ---

// Allocate an immutable string (ARC, acyclic)
void* mem_alloc_string(MemoryManager* mm, const char* data, size_t length);

// Allocate a mutable string buffer (ARC, acyclic)
void* mem_alloc_strbuf(MemoryManager* mm, size_t initial_capacity);

// --- Array allocation ---

// Allocate an ARC-managed array
// element_size: size of each element
// count: number of elements
// scanner: optional, if elements contain ARC references
void* mem_alloc_array(MemoryManager* mm, size_t element_size, size_t count,
                      arc_scan_fn scanner);

// --- Cycle collection ---

// Run cycle collection if threshold reached
void mem_collect_cycles(MemoryManager* mm);

// Force cycle collection
void mem_force_collect_cycles(MemoryManager* mm);

// --- Diagnostics ---

// Combined statistics from all subsystems
typedef struct {
    ArcStats arc;
    CycleGCStats cycle_gc;
    size_t arena_allocated;
    size_t arena_used;
    size_t arena_blocks;
} MemoryStats;

// Get combined statistics
MemoryStats mem_get_stats(MemoryManager* mm);

// Print combined statistics
void mem_print_stats(MemoryManager* mm);

// Print leak report (objects still alive)
void mem_print_leak_report(MemoryManager* mm);

#ifdef __cplusplus
}
#endif

#endif // CASPERIX_MEMORY_H
