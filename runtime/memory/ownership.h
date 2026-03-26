// Casperix Runtime - Ownership & Move Semantics
// Compile-time ownership annotations and runtime move helpers
//
// Ownership model:
//   - Every heap-allocated value has exactly one owner
//   - Ownership can be transferred (moved) to another variable
//   - After a move, the source is invalidated (set to NULL)
//   - Borrowing allows temporary read/write access without ownership transfer
//   - The semantic analyzer enforces ownership rules at compile time;
//     this header provides runtime support for move operations and
//     borrow checking debug assertions
//
// Integration:
//   - The compiler generates calls to ownership_move() for move expressions
//   - Debug builds insert borrow_check_* calls to catch violations at runtime
//   - Release builds compile out all borrow checks for zero overhead

#ifndef CASPERIX_OWNERSHIP_H
#define CASPERIX_OWNERSHIP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Ownership kind - used by the semantic analyzer for tracking
typedef enum {
    OWNER_OWNED,        // This variable owns the value
    OWNER_BORROWED,     // Immutable borrow (&T)
    OWNER_MUT_BORROWED, // Mutable borrow (&mut T)
    OWNER_MOVED         // Value has been moved away (use-after-move = error)
} OwnershipKind;

// --- Move operations ---

// Move a pointer value: nulls the source, returns the value
// The compiler generates this for move expressions like: y = move(x)
static inline void* ownership_move(void** source) {
    if (!source || !*source) return NULL;
    void* value = *source;
    *source = NULL;  // Invalidate source
    return value;
}

// Move with size info (for structs that are memcpy'd)
static inline void ownership_move_value(void* dest, void* source, size_t size) {
    if (!dest || !source || size == 0) return;
    memcpy(dest, source, size);
    memset(source, 0, size);  // Zero source after move
}

// --- Debug borrow checking (compiled out in release) ---

#ifdef CASPERIX_DEBUG_BORROWS

// Borrow tracking entry
typedef struct {
    const void* address;       // Address of borrowed value
    const char* borrower;      // Name of borrowing variable (debug info)
    const char* file;          // Source file
    int line;                  // Source line
    bool is_mutable;           // Mutable borrow?
} BorrowRecord;

// Maximum active borrows to track
#define MAX_BORROW_RECORDS 256

// Initialize borrow checker (call once at program start)
void borrow_checker_init(void);

// Shutdown borrow checker
void borrow_checker_shutdown(void);

// Record an immutable borrow
void borrow_check_borrow(const void* addr, const char* name,
                         const char* file, int line);

// Record a mutable borrow
void borrow_check_borrow_mut(const void* addr, const char* name,
                             const char* file, int line);

// Release a borrow (when borrow goes out of scope)
void borrow_check_release(const void* addr, const char* name);

// Assert no mutable borrows exist for addr (before creating immutable borrow)
void borrow_check_assert_no_mut(const void* addr, const char* file, int line);

// Assert no borrows at all exist for addr (before creating mutable borrow)
void borrow_check_assert_no_borrows(const void* addr, const char* file, int line);

// Assert value has not been moved (before any use)
void borrow_check_assert_not_moved(const void* addr, const char* name,
                                   const char* file, int line);

// Convenience macros for debug borrow checking
#define BORROW(addr, name) \
    borrow_check_borrow((addr), (name), __FILE__, __LINE__)
#define BORROW_MUT(addr, name) \
    borrow_check_borrow_mut((addr), (name), __FILE__, __LINE__)
#define BORROW_RELEASE(addr, name) \
    borrow_check_release((addr), (name))
#define ASSERT_NOT_MOVED(addr, name) \
    borrow_check_assert_not_moved((addr), (name), __FILE__, __LINE__)

#else
// Release mode: all borrow checks are no-ops
#define BORROW(addr, name)            ((void)0)
#define BORROW_MUT(addr, name)        ((void)0)
#define BORROW_RELEASE(addr, name)    ((void)0)
#define ASSERT_NOT_MOVED(addr, name)  ((void)0)

static inline void borrow_checker_init(void) {}
static inline void borrow_checker_shutdown(void) {}

#endif // CASPERIX_DEBUG_BORROWS

// --- Scope-based cleanup helpers ---

// Drop function type - called when value goes out of scope
typedef void (*drop_fn)(void* value);

// Scope guard - registered at scope entry, called at scope exit
typedef struct {
    void* value;
    drop_fn drop;
} ScopeGuard;

// Create a scope guard
static inline ScopeGuard scope_guard(void* value, drop_fn drop) {
    ScopeGuard g = { value, drop };
    return g;
}

// Execute scope guard (call drop function)
static inline void scope_guard_drop(ScopeGuard* guard) {
    if (guard && guard->value && guard->drop) {
        guard->drop(guard->value);
        guard->value = NULL;
    }
}

#ifdef __cplusplus
}
#endif

#endif // CASPERIX_OWNERSHIP_H
