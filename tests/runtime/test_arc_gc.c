#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "memory/arc.h"
#include "memory/memory.h"

static int g_pass = 0;
static int g_fail = 0;
static int g_destructor_calls = 0;

#define CHECK(cond, name) do { \
    if (cond) { \
        printf("  [PASS] %s\n", name); \
        g_pass++; \
    } else { \
        printf("  [FAIL] %s\n", name); \
        g_fail++; \
    } \
} while (0)

typedef struct TestNode {
    struct TestNode* strong_ref;
    ArcWeak weak_ref;
    bool has_weak_ref;
} TestNode;

static void test_node_scanner(void* obj, void (*visitor)(void* field_ptr)) {
    TestNode* node = (TestNode*)obj;
    if (node->strong_ref) {
        visitor(node->strong_ref);
    }
}

static void test_node_destructor(void* obj) {
    TestNode* node = (TestNode*)obj;

    g_destructor_calls++;
    if (node->strong_ref) {
        arc_release(node->strong_ref);
        node->strong_ref = NULL;
    }
    if (node->has_weak_ref) {
        arc_weak_release(&node->weak_ref);
        node->has_weak_ref = false;
    }
    arc_release(obj);
}

static void node_set_strong(TestNode* from, TestNode* to) {
    if (to) {
        arc_retain(to);
    }
    if (from->strong_ref) {
        arc_release(from->strong_ref);
    }
    from->strong_ref = to;
}

static void test_alignment_guarantees(void) {
    const size_t alignments[] = { 1, 2, 4, 8, 16, 32, 64 };
    const size_t count = sizeof(alignments) / sizeof(alignments[0]);

    printf("\n--- alignment guarantees ---\n");
    arc_reset_stats();

    for (size_t i = 0; i < count; i++) {
        size_t alignment = alignments[i];
        void* obj = arc_alloc_aligned(24, alignment);
        uintptr_t addr = (uintptr_t)obj;
        char label[64];

        snprintf(label, sizeof(label), "alignment %llu", (unsigned long long)alignment);
        CHECK(obj != NULL, label);
        if (obj) {
            snprintf(label, sizeof(label), "pointer mod %llu == 0",
                     (unsigned long long)alignment);
            CHECK((addr % alignment) == 0, label);

            snprintf(label, sizeof(label), "reported alignment >= %llu",
                     (unsigned long long)alignment);
            CHECK(arc_allocation_alignment(obj) >= alignment, label);
            arc_release(obj);
        }
    }

    CHECK(arc_get_stats().current_objects == 0, "alignment test leaves no live objects");
}

static void test_weak_upgrade_after_release(void) {
    printf("\n--- weak upgrade after release ---\n");
    arc_reset_stats();

    void* obj = arc_alloc(32);
    ArcWeak weak = arc_weak_create(obj);

    CHECK(obj != NULL, "allocated weak-upgrade test object");
    arc_release(obj);
    CHECK(!arc_weak_is_alive(weak), "weak reports dead after final release");
    CHECK(arc_weak_upgrade(weak) == NULL, "weak upgrade fails after destruction starts");
    arc_weak_release(&weak);
    CHECK(arc_get_stats().current_objects == 0, "weak test leaves no live objects");
}

static void test_cycle_collection_exactly_once(void) {
    MemoryManager* mm;
    TestNode* a;
    TestNode* b;

    printf("\n--- cycle collection exact-once ---\n");
    arc_reset_stats();
    g_destructor_calls = 0;

    mm = mem_init();
    CHECK(mm != NULL, "memory manager created");
    if (!mm) {
        return;
    }

    a = (TestNode*)mem_arc_alloc_full(mm, sizeof(TestNode),
                                      test_node_destructor, test_node_scanner);
    b = (TestNode*)mem_arc_alloc_full(mm, sizeof(TestNode),
                                      test_node_destructor, test_node_scanner);
    CHECK(a != NULL && b != NULL, "cycle nodes allocated");
    if (!a || !b) {
        mem_shutdown(mm);
        return;
    }

    node_set_strong(a, b);
    node_set_strong(b, a);

    mem_arc_release(mm, b);
    mem_force_collect_cycles(mm);
    CHECK(g_destructor_calls == 0, "external root keeps cycle alive");
    CHECK(arc_get_stats().current_objects == 2, "cycle remains live while rooted");

    mem_arc_release(mm, a);
    mem_force_collect_cycles(mm);
    CHECK(g_destructor_calls == 2, "cycle destructors run exactly once each");
    CHECK(arc_get_stats().current_objects == 0, "cycle collection frees both nodes");

    mem_shutdown(mm);
}

static void test_weak_edges_do_not_keep_objects_alive(void) {
    TestNode* owner;
    TestNode* target;

    printf("\n--- weak edges do not keep objects alive ---\n");
    arc_reset_stats();
    g_destructor_calls = 0;

    owner = (TestNode*)arc_alloc_with_destructor(sizeof(TestNode), test_node_destructor);
    target = (TestNode*)arc_alloc_with_destructor(sizeof(TestNode), test_node_destructor);
    CHECK(owner != NULL && target != NULL, "weak-edge nodes allocated");
    if (!owner || !target) {
        return;
    }

    owner->weak_ref = arc_weak_create(target);
    owner->has_weak_ref = true;

    arc_release(target);
    CHECK(!arc_weak_is_alive(owner->weak_ref), "weak edge does not retain target");
    arc_release(owner);
    CHECK(g_destructor_calls == 2, "both destructors run with only weak connectivity");
    CHECK(arc_get_stats().current_objects == 0, "weak-edge test leaves no live objects");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== ARC / Cycle-GC Regression Tests ===\n");
    test_alignment_guarantees();
    test_weak_upgrade_after_release();
    test_cycle_collection_exactly_once();
    test_weak_edges_do_not_keep_objects_alive();

    printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
