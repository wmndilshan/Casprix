// Casperix Runtime - Automatic Reference Counting (ARC)
// Thread-safe reference counting with weak reference support
//
// Object layout: [ArcHeader][user data...]
// - ArcHeader is prepended to every ARC-managed allocation
// - Atomic ref count for thread safety without locks
// - Support for custom destructors and weak references
// - Integration point for cycle collector (suspects list)

#ifndef CASPERIX_ARC_H
#define CASPERIX_ARC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Platform-specific atomics ──────────────────────────────────────────────
 * These macros are defined here (not in arc.c) so that the cycle collector
 * and any other file that manipulates ARC ref-counts can use the same
 * atomic primitives without duplicating the platform detection logic.
 * ─────────────────────────────────────────────────────────────────────────── */
#ifdef _MSC_VER
#  include <intrin.h>
#  define ATOMIC_INC(x)       _InterlockedIncrement((volatile long*)(x))
#  define ATOMIC_DEC(x)       _InterlockedDecrement((volatile long*)(x))
#  define ATOMIC_LOAD(x)      (*(volatile long*)(x))
#  define ATOMIC_CAS(x,o,n)   _InterlockedCompareExchange((volatile long*)(x),(n),(o))
/* size_t (64-bit) counters for stats */
#  define ATOMIC_INC_SZ(x)    _InterlockedIncrement64((volatile __int64*)(x))
#  define ATOMIC_DEC_SZ(x)    _InterlockedDecrement64((volatile __int64*)(x))
#  define ATOMIC_ADD_SZ(x,v)  _InterlockedExchangeAdd64((volatile __int64*)(x),(__int64)(v))
#  define ATOMIC_SUB_SZ(x,v)  _InterlockedExchangeAdd64((volatile __int64*)(x),-(__int64)(v))
#elif defined(__GNUC__) || defined(__clang__)
#  define ATOMIC_INC(x)       __sync_add_and_fetch((x), 1)
#  define ATOMIC_DEC(x)       __sync_sub_and_fetch((x), 1)
#  define ATOMIC_LOAD(x)      __sync_add_and_fetch((x), 0)
#  define ATOMIC_CAS(x,o,n)   __sync_val_compare_and_swap((x),(o),(n))
#  define ATOMIC_INC_SZ(x)    __sync_fetch_and_add((x),(size_t)1)
#  define ATOMIC_DEC_SZ(x)    __sync_fetch_and_sub((x),(size_t)1)
#  define ATOMIC_ADD_SZ(x,v)  __sync_fetch_and_add((x),(v))
#  define ATOMIC_SUB_SZ(x,v)  __sync_fetch_and_sub((x),(v))
#else
/* Fallback: non-atomic (single-threaded only) */
#  define ATOMIC_INC(x)       (++(*(x)))
#  define ATOMIC_DEC(x)       (--(*(x)))
#  define ATOMIC_LOAD(x)      (*(x))
#  define ATOMIC_CAS(x,o,n)   (*(x)==(o)?(*(x)=(n),(o)):*(x))
#  define ATOMIC_INC_SZ(x)    (++(*(x)))
#  define ATOMIC_DEC_SZ(x)    (--(*(x)))
#  define ATOMIC_ADD_SZ(x,v)  ((*(x))+=(v))
#  define ATOMIC_SUB_SZ(x,v)  ((*(x))-=(v))
#endif

// ARC object flags
#define ARC_FLAG_NONE          0x00
#define ARC_FLAG_MOVED         0x01  // Value has been moved (ownership transfer)
#define ARC_FLAG_HAS_WEAK      0x02  // Weak references exist for this object
#define ARC_FLAG_HAS_DESTRUCTOR 0x04 // Custom destructor registered
#define ARC_FLAG_CYCLE_SUSPECT 0x08  // Suspected of being in a reference cycle
#define ARC_FLAG_BUFFERED      0x10  // Already in cycle collector's buffer
#define ARC_FLAG_ACYCLIC       0x20  // Known to never participate in cycles
#define ARC_FLAG_DEALLOCATING  0x40  // Destruction has started; upgrades must fail

// ARC color for cycle collection (Bacon-Rajan algorithm)
typedef enum {
    ARC_COLOR_BLACK = 0,  // In use or free
    ARC_COLOR_PURPLE,     // Possible root of garbage cycle
    ARC_COLOR_GRAY,       // Being scanned
    ARC_COLOR_WHITE,      // Garbage (to be freed)
    ARC_COLOR_GREEN       // Acyclic, skip during collection
} ArcColor;

// Forward declaration
struct CycleCollector;

// Destructor function type
typedef void (*arc_destructor_fn)(void* obj);

// Field scanner for cycle detection - calls arc_visit on each ref field
typedef void (*arc_scan_fn)(void* obj, void (*visitor)(void* field_ptr));

// ARC object header - prepended to every managed object
typedef struct ArcHeader {
    void* allocation_base;         // Original allocation base for free()
    void* user_data;               // User-visible payload pointer
    size_t size;                   // Size of user data (excluding header)
    size_t alignment;              // Guaranteed payload alignment
    int32_t strong_count;          // Strong reference count (atomic on supported platforms)
    int32_t weak_count;            // Weak reference count (includes +1 from strong refs)
    uint32_t flags;                // ARC_FLAG_* bits
    ArcColor color;                // Color for cycle collection
    arc_destructor_fn destructor;  // Optional custom destructor
    arc_scan_fn scanner;           // Optional field scanner for cycle detection
} ArcHeader;

// Weak reference handle
typedef struct {
    ArcHeader* header;  // Points to the header (not user data)
} ArcWeak;

// --- Core ARC API ---

// Allocate a new ARC-managed object of given size
// Returns pointer to user data (after header)
// Initial strong_count = 1, weak_count = 1
void* arc_alloc(size_t size);

// Allocate with custom destructor
void* arc_alloc_with_destructor(size_t size, arc_destructor_fn destructor);

// Allocate with destructor and cycle scanner
void* arc_alloc_full(size_t size, arc_destructor_fn destructor, arc_scan_fn scanner);

// Allocate with an explicit payload alignment. alignment=0 uses the runtime default.
void* arc_alloc_aligned(size_t size, size_t alignment);
void* arc_alloc_with_destructor_aligned(size_t size, size_t alignment,
                                        arc_destructor_fn destructor);
void* arc_alloc_full_aligned(size_t size, size_t alignment,
                             arc_destructor_fn destructor, arc_scan_fn scanner);

// Increment strong reference count
// Returns the object pointer for convenience
void* arc_retain(void* obj);

// Decrement strong reference count
// Calls destructor and frees when count reaches 0
// If cycle collector is active and object has scanner, may buffer as suspect
void arc_release(void* obj);

// Release and report whether the object survived the decrement.
bool arc_release_survived(void* obj);

// Get current strong reference count
int32_t arc_strong_count(const void* obj);

// --- Weak References ---

// Create a weak reference to an ARC object
ArcWeak arc_weak_create(void* obj);

// Attempt to upgrade weak reference to strong reference
// Returns NULL if the object has been deallocated
void* arc_weak_upgrade(ArcWeak weak);

// Release weak reference
void arc_weak_release(ArcWeak* weak);

// Check if weak reference is still valid (object alive)
bool arc_weak_is_alive(ArcWeak weak);

// --- Object Info ---

// Get the ArcHeader from a user data pointer
ArcHeader* arc_get_header(void* obj);

// Get the ArcHeader from a user data pointer (const version)
const ArcHeader* arc_get_header_const(const void* obj);

// Report whether destruction has started for this object.
bool arc_is_deallocating(const void* obj);

// Check if object has been moved
bool arc_is_moved(const void* obj);

// Mark object as moved (for ownership transfer)
void arc_mark_moved(void* obj);

// Mark object as acyclic (optimization: skip during cycle collection)
void arc_mark_acyclic(void* obj);

// Expose the actual payload alignment for validation/tests.
size_t arc_allocation_alignment(const void* obj);

// Finalize an object whose strong count already reached zero via cycle collection.
// Returns true when this call performed the final destruction.
bool arc_cycle_collect(void* obj);

// --- Statistics ---

typedef struct {
    size_t total_allocations;    // Total objects allocated
    size_t total_frees;          // Total objects freed
    size_t current_objects;      // Currently live objects
    size_t current_bytes;        // Currently allocated bytes (user data only)
    size_t total_retains;        // Total retain calls
    size_t total_releases;       // Total release calls
    size_t weak_refs_created;    // Total weak refs created
    size_t weak_upgrades;        // Successful weak upgrades
    size_t weak_upgrade_fails;   // Failed weak upgrades (object dead)
} ArcStats;

// Get current ARC statistics
ArcStats arc_get_stats(void);

// Reset statistics counters
void arc_reset_stats(void);

// Print statistics to stdout
void arc_print_stats(void);

// Notify ARC that an object was freed externally (e.g. by the cycle collector)
// without going through arc_release. Keeps global stats accurate.
void arc_notify_freed(size_t size);

// --- Header access macros ---

#define ARC_HEADER_SIZE sizeof(ArcHeader)
#define ARC_HEADER_SLOT_SIZE sizeof(ArcHeader*)
#define ARC_OBJ_TO_HEADER(ptr) (*(((ArcHeader**)(ptr)) - 1))
#define ARC_HEADER_TO_OBJ(hdr) ((hdr)->user_data)

#ifdef __cplusplus
}
#endif

#endif // CASPERIX_ARC_H
