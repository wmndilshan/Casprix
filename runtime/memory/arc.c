// Casperix Runtime - ARC Implementation
// Thread-safe automatic reference counting with weak references

#include "arc.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Platform-specific atomic helpers
#ifdef _MSC_VER
    #include <intrin.h>

static inline int32_t arc_atomic_load_i32(const volatile int32_t* value) {
    return (int32_t)_InterlockedCompareExchange((volatile long*)value, 0, 0);
}

static inline int32_t arc_atomic_fetch_sub_i32(volatile int32_t* value, int32_t delta) {
    return (int32_t)_InterlockedExchangeAdd((volatile long*)value, -delta);
}

static inline bool arc_atomic_compare_exchange_i32(volatile int32_t* value,
                                                   int32_t* expected, int32_t desired) {
    long observed = _InterlockedCompareExchange((volatile long*)value,
                                                (long)desired, (long)*expected);
    if ((int32_t)observed == *expected) {
        return true;
    }
    *expected = (int32_t)observed;
    return false;
}

static inline uint32_t arc_atomic_load_u32(const volatile uint32_t* value) {
    return (uint32_t)_InterlockedCompareExchange((volatile long*)value, 0, 0);
}

static inline uint32_t arc_atomic_fetch_or_u32(volatile uint32_t* value, uint32_t mask) {
    return (uint32_t)_InterlockedOr((volatile long*)value, (long)mask);
}
#elif defined(__GNUC__) || defined(__clang__)
static inline int32_t arc_atomic_load_i32(const volatile int32_t* value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static inline int32_t arc_atomic_fetch_sub_i32(volatile int32_t* value, int32_t delta) {
    return __atomic_fetch_sub(value, delta, __ATOMIC_ACQ_REL);
}

static inline bool arc_atomic_compare_exchange_i32(volatile int32_t* value,
                                                   int32_t* expected, int32_t desired) {
    return __atomic_compare_exchange_n(value, expected, desired, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline uint32_t arc_atomic_load_u32(const volatile uint32_t* value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static inline uint32_t arc_atomic_fetch_or_u32(volatile uint32_t* value, uint32_t mask) {
    return __atomic_fetch_or(value, mask, __ATOMIC_ACQ_REL);
}
#else
static inline int32_t arc_atomic_load_i32(const volatile int32_t* value) {
    return *value;
}

static inline int32_t arc_atomic_fetch_sub_i32(volatile int32_t* value, int32_t delta) {
    int32_t old = *value;
    *value -= delta;
    return old;
}

static inline bool arc_atomic_compare_exchange_i32(volatile int32_t* value,
                                                   int32_t* expected, int32_t desired) {
    if (*value == *expected) {
        *value = desired;
        return true;
    }
    *expected = *value;
    return false;
}

static inline uint32_t arc_atomic_load_u32(const volatile uint32_t* value) {
    return *value;
}

static inline uint32_t arc_atomic_fetch_or_u32(volatile uint32_t* value, uint32_t mask) {
    uint32_t old = *value;
    *value |= mask;
    return old;
}
#endif

// Global ARC statistics
static ArcStats g_arc_stats = {0};

static void arc_abort_count_error(const char* message) {
    fprintf(stderr, "ARC ERROR: %s\n", message);
    abort();
}

static bool is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static size_t arc_default_alignment(void) {
    size_t align = _Alignof(max_align_t);
    if (align < _Alignof(ArcHeader*)) {
        align = _Alignof(ArcHeader*);
    }
    return align;
}

static bool add_sizes(size_t lhs, size_t rhs, size_t* out) {
    if (!out || lhs > SIZE_MAX - rhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

static inline ArcHeader* get_header(void* obj) {
    assert(obj != NULL);
    return ARC_OBJ_TO_HEADER(obj);
}

static inline const ArcHeader* get_header_const(const void* obj) {
    assert(obj != NULL);
    return ARC_OBJ_TO_HEADER((void*)obj);
}

static bool arc_header_is_deallocating(const ArcHeader* header) {
    return (arc_atomic_load_u32((const volatile uint32_t*)&header->flags) &
            ARC_FLAG_DEALLOCATING) != 0;
}

static bool arc_header_is_finalized(const ArcHeader* header) {
    int32_t strong_count;

    if (!header) {
        return true;
    }
    if (arc_header_is_deallocating(header)) {
        return true;
    }
    strong_count = arc_atomic_load_i32(&header->strong_count);
    return strong_count <= 0 &&
           (header->color == ARC_COLOR_WHITE ||
            header->color == ARC_COLOR_GRAY ||
            (arc_atomic_load_u32(&header->flags) & ARC_FLAG_BUFFERED) != 0);
}

static bool arc_try_inc_count(volatile int32_t* count, const char* overflow_message) {
    int32_t current = arc_atomic_load_i32(count);

    while (true) {
        if (current < 0) {
            arc_abort_count_error("reference count underflow detected");
        }
        if (current == INT32_MAX) {
            arc_abort_count_error(overflow_message);
        }
        if (arc_atomic_compare_exchange_i32(count, &current, current + 1)) {
            return true;
        }
    }
}

static void arc_zero_payload(const ArcHeader* header) {
    if (header && header->user_data && header->size > 0) {
        memset(header->user_data, 0, header->size);
    }
}

static void arc_free_allocation(ArcHeader* header) {
    void* allocation_base = header ? header->allocation_base : NULL;
    free(allocation_base);
}

static void arc_finalize_zero_strong(ArcHeader* header) {
    int32_t old_weak;

    if (!header) {
        return;
    }

    if ((header->flags & ARC_FLAG_HAS_DESTRUCTOR) && header->destructor) {
        header->destructor(header->user_data);
    }

    header->scanner = NULL;
    header->color = ARC_COLOR_BLACK;
    header->flags &= ~(ARC_FLAG_BUFFERED | ARC_FLAG_CYCLE_SUSPECT);

    g_arc_stats.total_frees++;
    if (g_arc_stats.current_objects > 0) {
        g_arc_stats.current_objects--;
    }
    if (g_arc_stats.current_bytes >= header->size) {
        g_arc_stats.current_bytes -= header->size;
    } else {
        g_arc_stats.current_bytes = 0;
    }

    old_weak = arc_atomic_fetch_sub_i32(&header->weak_count, 1);
    if (old_weak <= 0) {
        arc_abort_count_error("weak count underflow while finalizing object");
    }
    if (old_weak == 1) {
        arc_free_allocation(header);
        return;
    }

    arc_zero_payload(header);
}

static bool arc_start_deallocation(ArcHeader* header) {
    uint32_t old_flags;

    if (!header) {
        return false;
    }

    old_flags = arc_atomic_fetch_or_u32(&header->flags, ARC_FLAG_DEALLOCATING);
    if ((old_flags & ARC_FLAG_DEALLOCATING) != 0) {
        return false;
    }
    return true;
}

static bool arc_release_impl(void* obj) {
    ArcHeader* header;
    int32_t current;

    if (!obj) {
        return false;
    }

    header = get_header(obj);
    current = arc_atomic_load_i32(&header->strong_count);

    while (true) {
        if (current <= 0) {
            if (arc_header_is_finalized(header)) {
                return false;
            }
            arc_abort_count_error("over-release detected");
        }
        if (arc_atomic_compare_exchange_i32(&header->strong_count, &current, current - 1)) {
            break;
        }
    }

    if (current == 1) {
        if (arc_start_deallocation(header)) {
            arc_finalize_zero_strong(header);
        }
        return false;
    }

    return true;
}

// --- Core ARC API ---

void* arc_alloc(size_t size) {
    return arc_alloc_full_aligned(size, 0, NULL, NULL);
}

void* arc_alloc_aligned(size_t size, size_t alignment) {
    return arc_alloc_full_aligned(size, alignment, NULL, NULL);
}

void* arc_alloc_with_destructor(size_t size, arc_destructor_fn destructor) {
    return arc_alloc_full_aligned(size, 0, destructor, NULL);
}

void* arc_alloc_with_destructor_aligned(size_t size, size_t alignment,
                                        arc_destructor_fn destructor) {
    return arc_alloc_full_aligned(size, alignment, destructor, NULL);
}

void* arc_alloc_full(size_t size, arc_destructor_fn destructor, arc_scan_fn scanner) {
    return arc_alloc_full_aligned(size, 0, destructor, scanner);
}

void* arc_alloc_full_aligned(size_t size, size_t alignment,
                             arc_destructor_fn destructor, arc_scan_fn scanner) {
    ArcHeader* header;
    void* raw;
    void* user_data;
    ArcHeader** slot;
    uintptr_t raw_addr;
    uintptr_t payload_addr;
    size_t total = 0;
    size_t metadata_size;

    if (size == 0) {
        return NULL;
    }

    alignment = alignment ? alignment : arc_default_alignment();
    if (!is_power_of_two(alignment)) {
        return NULL;
    }
    if (alignment < arc_default_alignment()) {
        alignment = arc_default_alignment();
    }

    metadata_size = sizeof(ArcHeader) + ARC_HEADER_SLOT_SIZE;
    if (!add_sizes(metadata_size, size, &total) ||
        !add_sizes(total, alignment - 1, &total)) {
        return NULL;
    }

    raw = malloc(total);
    if (!raw) {
        return NULL;
    }

    header = (ArcHeader*)raw;
    raw_addr = (uintptr_t)raw + metadata_size;
    payload_addr = (raw_addr + (alignment - 1)) & ~((uintptr_t)alignment - 1U);
    user_data = (void*)payload_addr;
    slot = (ArcHeader**)((uint8_t*)user_data - ARC_HEADER_SLOT_SIZE);

    memset(header, 0, sizeof(*header));
    memset(user_data, 0, size);

    header->allocation_base = raw;
    header->user_data = user_data;
    header->size = size;
    header->alignment = alignment;
    header->strong_count = 1;
    header->weak_count = 1;  // +1 held by strong refs collectively
    header->flags = ARC_FLAG_NONE;
    header->color = ARC_COLOR_BLACK;
    header->destructor = destructor;
    header->scanner = scanner;

    if (destructor) {
        header->flags |= ARC_FLAG_HAS_DESTRUCTOR;
    }

    *slot = header;

    g_arc_stats.total_allocations++;
    g_arc_stats.current_objects++;
    g_arc_stats.current_bytes += size;

    return user_data;
}

void* arc_retain(void* obj) {
    ArcHeader* header;

    if (!obj) {
        return NULL;
    }

    header = get_header(obj);
    if (arc_header_is_finalized(header)) {
        arc_abort_count_error("arc_retain on dead or moved object");
    }
    if ((arc_atomic_load_u32(&header->flags) & ARC_FLAG_MOVED) != 0) {
        arc_abort_count_error("arc_retain on moved object");
    }

    arc_try_inc_count(&header->strong_count, "strong count overflow on retain");
    g_arc_stats.total_retains++;

    return obj;
}

void arc_release(void* obj) {
    if (!obj) {
        return;
    }

    g_arc_stats.total_releases++;
    (void)arc_release_impl(obj);
}

bool arc_release_survived(void* obj) {
    if (!obj) {
        return false;
    }

    g_arc_stats.total_releases++;
    return arc_release_impl(obj);
}

int32_t arc_strong_count(const void* obj) {
    const ArcHeader* header;

    if (!obj) {
        return 0;
    }

    header = get_header_const(obj);
    return arc_atomic_load_i32(&header->strong_count);
}

// --- Weak References ---

ArcWeak arc_weak_create(void* obj) {
    ArcWeak weak = { NULL };
    ArcHeader* header;

    if (!obj) {
        return weak;
    }

    header = get_header(obj);
    if (arc_header_is_finalized(header) || arc_atomic_load_i32(&header->strong_count) <= 0) {
        arc_abort_count_error("arc_weak_create on dead object");
    }

    arc_try_inc_count(&header->weak_count, "weak count overflow on weak create");
    arc_atomic_fetch_or_u32(&header->flags, ARC_FLAG_HAS_WEAK);

    weak.header = header;
    g_arc_stats.weak_refs_created++;

    return weak;
}

void* arc_weak_upgrade(ArcWeak weak) {
    ArcHeader* header = weak.header;
    int32_t strong;

    if (!header) {
        g_arc_stats.weak_upgrade_fails++;
        return NULL;
    }

    strong = arc_atomic_load_i32(&header->strong_count);
    while (true) {
        if (strong <= 0 || arc_header_is_finalized(header)) {
            g_arc_stats.weak_upgrade_fails++;
            return NULL;
        }
        if (strong == INT32_MAX) {
            arc_abort_count_error("strong count overflow during weak upgrade");
        }
        if (arc_atomic_compare_exchange_i32(&header->strong_count, &strong, strong + 1)) {
            g_arc_stats.total_retains++;
            g_arc_stats.weak_upgrades++;
            return ARC_HEADER_TO_OBJ(header);
        }
    }
}

void arc_weak_release(ArcWeak* weak) {
    ArcHeader* header;
    int32_t old_weak;

    if (!weak || !weak->header) {
        return;
    }

    header = weak->header;
    weak->header = NULL;

    old_weak = arc_atomic_fetch_sub_i32(&header->weak_count, 1);
    if (old_weak <= 0) {
        arc_abort_count_error("weak count underflow on weak release");
    }
    if (old_weak == 1 && arc_atomic_load_i32(&header->strong_count) <= 0) {
        arc_free_allocation(header);
    }
}

bool arc_weak_is_alive(ArcWeak weak) {
    if (!weak.header) {
        return false;
    }
    if (arc_header_is_finalized(weak.header)) {
        return false;
    }
    return arc_atomic_load_i32(&weak.header->strong_count) > 0;
}

// --- Object Info ---

ArcHeader* arc_get_header(void* obj) {
    if (!obj) {
        return NULL;
    }
    return get_header(obj);
}

const ArcHeader* arc_get_header_const(const void* obj) {
    if (!obj) {
        return NULL;
    }
    return get_header_const(obj);
}

bool arc_is_deallocating(const void* obj) {
    if (!obj) {
        return false;
    }
    return arc_header_is_deallocating(get_header_const(obj));
}

bool arc_is_moved(const void* obj) {
    if (!obj) {
        return false;
    }
    return (arc_atomic_load_u32(&get_header_const(obj)->flags) & ARC_FLAG_MOVED) != 0;
}

void arc_mark_moved(void* obj) {
    if (!obj) {
        return;
    }
    arc_atomic_fetch_or_u32(&get_header(obj)->flags, ARC_FLAG_MOVED);
}

void arc_mark_acyclic(void* obj) {
    ArcHeader* header;

    if (!obj) {
        return;
    }

    header = get_header(obj);
    arc_atomic_fetch_or_u32(&header->flags, ARC_FLAG_ACYCLIC);
    header->color = ARC_COLOR_GREEN;
}

size_t arc_allocation_alignment(const void* obj) {
    if (!obj) {
        return 0;
    }
    return get_header_const(obj)->alignment;
}

bool arc_cycle_collect(void* obj) {
    ArcHeader* header;

    if (!obj) {
        return false;
    }

    header = get_header(obj);
    if (arc_atomic_load_i32(&header->strong_count) > 0) {
        return false;
    }
    if (!arc_start_deallocation(header)) {
        return false;
    }

    arc_finalize_zero_strong(header);
    return true;
}

// --- Statistics ---

ArcStats arc_get_stats(void) {
    return g_arc_stats;
}

void arc_reset_stats(void) {
    memset(&g_arc_stats, 0, sizeof(ArcStats));
}

/* Called by the cycle collector when it frees ARC-managed objects directly
   (bypassing arc_release) so that global stats stay accurate. */
void arc_notify_freed(size_t size) {
    g_arc_stats.total_frees++;
    if (g_arc_stats.current_objects > 0) {
        g_arc_stats.current_objects--;
    }
    if (g_arc_stats.current_bytes >= size) {
        g_arc_stats.current_bytes -= size;
    } else {
        g_arc_stats.current_bytes = 0;
    }
}

void arc_print_stats(void) {
    printf("=== ARC Statistics ===\n");
    printf("Total allocations:   %llu\n", (unsigned long long)g_arc_stats.total_allocations);
    printf("Total frees:         %llu\n", (unsigned long long)g_arc_stats.total_frees);
    printf("Current objects:     %llu\n", (unsigned long long)g_arc_stats.current_objects);
    printf("Current bytes:       %llu\n", (unsigned long long)g_arc_stats.current_bytes);
    printf("Total retains:       %llu\n", (unsigned long long)g_arc_stats.total_retains);
    printf("Total releases:      %llu\n", (unsigned long long)g_arc_stats.total_releases);
    printf("Weak refs created:   %llu\n", (unsigned long long)g_arc_stats.weak_refs_created);
    printf("Weak upgrades (ok):  %llu\n", (unsigned long long)g_arc_stats.weak_upgrades);
    printf("Weak upgrades (fail):%llu\n", (unsigned long long)g_arc_stats.weak_upgrade_fails);
    printf("=======================\n");
}
