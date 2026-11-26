#include "compiler/ir/mir.h"
#include "vm/cvm_engine.h"
#include "memory/arc.h"
#include "memory/cycle_gc.h"

#include <stdio.h>
#include <stdint.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return false; \
} } while (0)

typedef struct {
    MirModule* module;
    MirFunction* make;
} Fixture;

static Fixture make_fixture(const char* module_name, int managed_offset,
                            bool store_scalar_pointer) {
    Fixture fixture = {0};
    MirParam param = {0};
    MirType* ptr;
    MirFunction* fn;
    MirBlock* entry;
    MirBuilder builder;
    int managed[] = { managed_offset };

    fixture.module = mir_module_create(module_name);
    if (!fixture.module) return fixture;
    ptr = mir_type_ptr(fixture.module, mir_type_i8(fixture.module));
    param.name = "scalar_pointer_bits";
    param.type = ptr;
    fn = mir_module_add_function(fixture.module, "make", ptr, &param, 1);
    if (!fn) return fixture;
    fixture.make = fn;
    entry = mir_function_add_block(fn, "entry");
    if (!entry) return fixture;
    mir_builder_init(&builder, fixture.module, fn);
    mir_builder_set_block(&builder, entry);

    MirValueId child = mir_build_obj_alloc(&builder, "MetadataChild", 16);
    MirValueId parent = mir_build_obj_alloc_ex(
        &builder, "SharedMetadataNode", 32, managed, 1);
    if (managed_offset == 8) {
        MirValueId field = mir_build_get_field_ptr(&builder, parent, 1);
        mir_build_store(&builder, field, child);
        if (store_scalar_pointer) {
            MirValueId scalar = mir_build_get_field_ptr(&builder, parent, 2);
            mir_build_store(&builder, scalar, fn->params[0].value_id);
        }
    } else {
        if (store_scalar_pointer) {
            MirValueId scalar = mir_build_get_field_ptr(&builder, parent, 1);
            mir_build_store(&builder, scalar, fn->params[0].value_id);
        }
        MirValueId field = mir_build_get_field_ptr(&builder, parent, 2);
        mir_build_store(&builder, field, child);
    }
    mir_build_ret(&builder, parent);
    return fixture;
}

static bool create_external_parent(Fixture fixture, CvmState** vm_out,
                                   void* scalar_bits, void** parent_out,
                                   void** child_out, int child_offset) {
    CvmState* vm = cvm_state_create(fixture.module, NULL);
    CvmReg args[1] = { (CvmReg)(uintptr_t)scalar_bits };
    void* parent;
    void* child;
    if (!vm) return false;
    parent = (void*)(uintptr_t)cvm_call_function(vm, fixture.make, args, 1);
    if (!parent || vm->trap_code) {
        cvm_state_destroy(vm);
        return false;
    }
    child = *(void**)((char*)parent + child_offset);
    if (!child) {
        cvm_state_destroy(vm);
        return false;
    }
    (void)arc_retain(parent);
    (void)arc_retain(child);
    *vm_out = vm;
    *parent_out = parent;
    *child_out = child;
    return true;
}

static bool run_different_layout_order(bool release_b_first) {
    Fixture a = make_fixture("metadata_layout_a", 8, false);
    Fixture b = make_fixture("metadata_layout_b", 16, true);
    void* decoy = arc_alloc(16);
    void* pa = NULL;
    void* pb = NULL;
    void* ca = NULL;
    void* cb = NULL;
    CvmState* va = NULL;
    CvmState* vb = NULL;
    ArcWeak weak_a = {0}, weak_b = {0}, weak_decoy = {0};

    CHECK(a.module && a.make && b.module && b.make && decoy);
    CHECK(create_external_parent(a, &va, decoy, &pa, &ca, 8));
    CHECK(create_external_parent(b, &vb, decoy, &pb, &cb, 16));
    weak_a = arc_weak_create(ca);
    weak_b = arc_weak_create(cb);
    weak_decoy = arc_weak_create(decoy);
    CHECK(*(void**)((char*)pb + 8) == decoy);
    cvm_state_destroy(va);
    cvm_state_destroy(vb);
    mir_module_destroy(a.module);
    mir_module_destroy(b.module);
    CHECK(arc_strong_count(pa) == 1 && arc_strong_count(pb) == 1);
    CHECK(arc_strong_count(ca) == 1 && arc_strong_count(cb) == 1);

    if (release_b_first) {
        CHECK(arc_release_survived(pb) == false);
        CHECK(arc_strong_count(decoy) == 1);
        CHECK(!arc_weak_is_alive(weak_b));
        CHECK(arc_strong_count(ca) == 1);
        CHECK(arc_release_survived(pa) == false);
    } else {
        CHECK(arc_release_survived(pa) == false);
        CHECK(arc_strong_count(decoy) == 1);
        CHECK(arc_strong_count(cb) == 1);
        CHECK(arc_release_survived(pb) == false);
        CHECK(!arc_weak_is_alive(weak_b));
    }
    CHECK(!arc_weak_is_alive(weak_a));
    CHECK(arc_weak_is_alive(weak_decoy));
    CHECK(arc_release_survived(decoy) == false);
    CHECK(!arc_weak_is_alive(weak_decoy));
    arc_weak_release(&weak_a);
    arc_weak_release(&weak_b);
    arc_weak_release(&weak_decoy);
    return true;
}

static bool run_identical_layout_sharing(void) {
    Fixture a = make_fixture("metadata_same_a", 8, false);
    Fixture b = make_fixture("metadata_same_b", 8, false);
    void* decoy = arc_alloc(16);
    void *pa = NULL, *pb = NULL, *ca = NULL, *cb = NULL;
    CvmState *va = NULL, *vb = NULL;
    ArcWeak weak_a = {0}, weak_b = {0};
    CHECK(a.module && a.make && b.module && b.make && decoy);
    CHECK(create_external_parent(a, &va, decoy, &pa, &ca, 8));
    CHECK(create_external_parent(b, &vb, decoy, &pb, &cb, 8));
    weak_a = arc_weak_create(ca);
    weak_b = arc_weak_create(cb);
    cvm_state_destroy(va);
    cvm_state_destroy(vb);
    mir_module_destroy(a.module);
    mir_module_destroy(b.module);
    CHECK(arc_release_survived(pa) == false);
    CHECK(!arc_weak_is_alive(weak_a));
    CHECK(arc_strong_count(pb) == 1 && arc_strong_count(cb) == 1);
    CHECK(arc_release_survived(pb) == false);
    CHECK(!arc_weak_is_alive(weak_b));
    CHECK(arc_release_survived(decoy) == false);
    arc_weak_release(&weak_a);
    arc_weak_release(&weak_b);
    return true;
}

static bool reject_layout_case(const char* name, int size,
                               const int* offsets, int count,
                               bool force_missing_offsets) {
    MirModule* module = mir_module_create(name);
    MirType* ptr = mir_type_ptr(module, mir_type_i8(module));
    MirFunction* fn = mir_module_add_function(module, "main", ptr, NULL, 0);
    MirBlock* entry = mir_function_add_block(fn, "entry");
    MirBuilder builder;
    mir_builder_init(&builder, module, fn);
    mir_builder_set_block(&builder, entry);
    MirValueId obj = mir_build_obj_alloc_ex(&builder, "Malformed", size,
                                            offsets, count);
    mir_build_ret(&builder, obj);
    MirInst* alloc = entry->first;
    while (alloc && alloc->opcode != MIR_OBJ_ALLOC) alloc = alloc->next;
    if (alloc) {
        if (force_missing_offsets) {
            alloc->as.obj_alloc.managed_field_count = 1;
            alloc->as.obj_alloc.managed_offsets = NULL;
        } else if (count < 0) {
            /* The builder normalizes negative counts; exercise malformed MIR. */
            alloc->as.obj_alloc.managed_field_count = count;
        }
    }
    bool verifier_rejects = mir_verify_module(module, NULL) != 0;
    CvmState* vm = cvm_state_create(module, NULL);
    if (!vm) { mir_module_destroy(module); return false; }
    (void)cvm_run(vm, "main", NULL, 0);
    bool runtime_rejects = vm->trap_code == CVM_TRAP_INVALID_OBJECT_LAYOUT;
    cvm_state_destroy(vm);
    mir_module_destroy(module);
    return verifier_rejects && runtime_rejects;
}

static bool run_malformed_layout_cases(void) {
    const int duplicate[] = { 8, 8 };
    const int misaligned[] = { 12 };
    const int overlaps_header[] = { 0 };
    const int out_of_bounds[] = { 32 };
    const int extends_past_end[] = { 24 };
    const int overflow_offset[] = { INT32_MAX - 7 };
    const int valid[] = { 8 };
    CHECK(reject_layout_case("bad_duplicate", 32, duplicate, 2, false));
    CHECK(reject_layout_case("bad_misaligned", 32, misaligned, 1, false));
    CHECK(reject_layout_case("bad_header_overlap", 32, overlaps_header, 1, false));
    CHECK(reject_layout_case("bad_oob", 32, out_of_bounds, 1, false));
    CHECK(reject_layout_case("bad_past_end", 28, extends_past_end, 1, false));
    CHECK(reject_layout_case("bad_overflow", 32, overflow_offset, 1, false));
    CHECK(reject_layout_case("bad_missing_offsets", 32, valid, 1, true));
    CHECK(reject_layout_case("bad_count", 32, valid, -1, false));
    CHECK(reject_layout_case("bad_size", 0, NULL, 0, false));
    return true;
}

static bool run_cycle_survivor_edges(void) {
    MirModule* module = mir_module_create("metadata_cycle_survivor");
    MirType* ptr = mir_type_ptr(module, mir_type_i8(module));
    MirFunction* fn = mir_module_add_function(module, "main", ptr, NULL, 0);
    MirBlock* entry = mir_function_add_block(fn, "entry");
    MirBuilder builder;
    int layout_a[] = { 8, 16 };
    int layout_b[] = { 16, 24 };
    mir_builder_init(&builder, module, fn);
    mir_builder_set_block(&builder, entry);
    MirValueId a = mir_build_obj_alloc_ex(&builder, "CycleShared", 40,
                                          layout_a, 2);
    MirValueId b = mir_build_obj_alloc_ex(&builder, "CycleShared", 40,
                                          layout_b, 2);
    MirValueId c = mir_build_obj_alloc(&builder, "CycleSurvivor", 16);
    mir_build_arc_retain(&builder, b);
    mir_build_store(&builder, mir_build_get_field_ptr(&builder, a, 1), b);
    mir_build_arc_retain(&builder, a);
    mir_build_store(&builder, mir_build_get_field_ptr(&builder, b, 2), a);
    mir_build_arc_retain(&builder, c);
    mir_build_store(&builder, mir_build_get_field_ptr(&builder, b, 3), c);
    mir_build_arc_release(&builder, b); /* relinquish B's allocation owner */
    mir_build_ret(&builder, a);

    CvmState* vm = cvm_state_create(module, NULL);
    CHECK(vm != NULL);
    void* external_a = (void*)(uintptr_t)cvm_run(vm, "main", NULL, 0);
    CHECK(external_a != NULL && vm->trap_code == 0);
    void* peer_b = *(void**)((char*)external_a + 8);
    void* survivor_c = peer_b ? *(void**)((char*)peer_b + 24) : NULL;
    CHECK(peer_b && survivor_c);
    (void)arc_retain(survivor_c);
    ArcWeak weak_a = arc_weak_create(external_a);
    ArcWeak weak_b = arc_weak_create(peer_b);
    ArcWeak weak_c = arc_weak_create(survivor_c);
    CHECK(arc_release_survived(external_a));

    CycleCollector* collector = cycle_gc_create();
    CHECK(collector != NULL);
    cycle_gc_set_threshold(collector, 1);
    cycle_gc_add_suspect(collector, peer_b);
    cycle_gc_add_suspect(collector, external_a);
    int collected = cycle_gc_collect(collector);
    cycle_gc_destroy(collector);
    CHECK(collected >= 2);
    CHECK(!arc_weak_is_alive(weak_a) && !arc_weak_is_alive(weak_b));
    CHECK(arc_weak_is_alive(weak_c));

    cvm_state_destroy(vm);
    mir_module_destroy(module);
    CHECK(arc_weak_is_alive(weak_c));
    CHECK(arc_strong_count(survivor_c) == 1);
    CHECK(!arc_release_survived(survivor_c));
    CHECK(!arc_weak_is_alive(weak_c));
    arc_weak_release(&weak_a);
    arc_weak_release(&weak_b);
    arc_weak_release(&weak_c);
    return true;
}

static bool run_residual_vm_owner_contract(void) {
    MirModule* module = mir_module_create("residual_vm_owners");
    MirType* ptr = mir_type_ptr(module, mir_type_i8(module));
    MirFunction* fn = mir_module_add_function(module, "main", ptr, NULL, 0);
    MirBlock* entry = mir_function_add_block(fn, "entry");
    MirBuilder builder;
    mir_builder_init(&builder, module, fn);
    mir_builder_set_block(&builder, entry);
    MirValueId object = mir_build_obj_alloc(&builder, "ResidualOwner", 16);
    mir_build_arc_retain(&builder, object);
    mir_build_arc_retain(&builder, object);
    mir_build_arc_release(&builder, object);
    mir_build_ret(&builder, object);

    CvmState* vm = cvm_state_create(module, NULL);
    CHECK(vm != NULL);
    void* external = (void*)(uintptr_t)cvm_run(vm, "main", NULL, 0);
    CHECK(external && vm->trap_code == 0);
    ArcWeak weak = arc_weak_create(external);
    CHECK(arc_strong_count(external) == 2);
    cvm_state_destroy(vm);
    mir_module_destroy(module);
    CHECK(arc_weak_is_alive(weak));
    /* An explicit MIR release accounts this allocation in the VM. Its two
     * remaining strong owners transfer to the embedding caller at return. */
    CHECK(arc_strong_count(external) == 2);
    CHECK(arc_release_survived(external));
    CHECK(!arc_release_survived(external));
    CHECK(!arc_weak_is_alive(weak));
    arc_weak_release(&weak);
    return true;
}

static bool run_managed_arithmetic_trap_cleanup(void) {
    MirModule* module = mir_module_create("managed_arithmetic_trap");
    MirType* ptr = mir_type_ptr(module, mir_type_i8(module));
    MirType* i64 = mir_type_i64(module);
    MirFunction* fn = mir_module_add_function(module, "main", ptr, NULL, 0);
    MirBlock* entry = mir_function_add_block(fn, "entry");
    MirBuilder builder;
    const int child_field[] = { 8 };
    CvmState* vm = NULL;
    void *child = NULL, *parent = NULL;
    ArcWeak weak_child = {0}, weak_parent = {0};
    bool held_child = false, held_parent = false;
    bool ok = false;

    if (!module || !fn || !entry) goto cleanup;
    mir_builder_init(&builder, module, fn);
    mir_builder_set_block(&builder, entry);
    MirValueId child_id = mir_build_obj_alloc(&builder, "TrapCleanupChild", 16);
    MirValueId parent_id = mir_build_obj_alloc_ex(&builder, "TrapCleanupParent", 16,
                                                   child_field, 1);
    mir_build_arc_retain(&builder, child_id); /* parent field owns one child ref */
    mir_build_store(&builder, mir_build_get_field_ptr(&builder, parent_id, 1), child_id);
    MirValueId one = mir_build_const_int(&builder, 1, i64);
    MirValueId zero = mir_build_const_int(&builder, 0, i64);
    (void)mir_build_div(&builder, one, zero);
    mir_build_ret(&builder, parent_id);

    vm = cvm_state_create(module, NULL);
    if (!vm) goto cleanup;
    (void)cvm_run(vm, "main", NULL, 0);
    if (vm->trap_code != CVM_TRAP_DIVISION_BY_ZERO || vm->heap_obj_count != 2)
        goto cleanup;
    child = vm->heap_objs[0];
    parent = vm->heap_objs[1];
    if (!child || !parent) goto cleanup;

    /* Simulate an embedding caller preserving both objects after the trap. */
    (void)arc_retain(child);
    held_child = true;
    (void)arc_retain(parent);
    held_parent = true;
    weak_child = arc_weak_create(child);
    weak_parent = arc_weak_create(parent);
    cvm_state_destroy(vm);
    vm = NULL;
    mir_module_destroy(module);
    module = NULL;

    if (!arc_weak_is_alive(weak_child) || !arc_weak_is_alive(weak_parent) ||
        arc_strong_count(parent) != 1 || arc_strong_count(child) != 2)
        goto cleanup;
    (void)arc_release_survived(parent); /* parent destructor releases its child edge */
    parent = NULL;
    held_parent = false;
    if (arc_weak_is_alive(weak_parent) || arc_strong_count(child) != 1)
        goto cleanup;
    (void)arc_release_survived(child);
    child = NULL;
    held_child = false;
    if (arc_weak_is_alive(weak_child)) goto cleanup;
    ok = true;

cleanup:
    if (vm) cvm_state_destroy(vm);
    if (module) mir_module_destroy(module);
    if (held_parent && parent) (void)arc_release_survived(parent);
    if (held_child && child) (void)arc_release_survived(child);
    arc_weak_release(&weak_parent);
    arc_weak_release(&weak_child);
    return ok;
}

int main(void) {
    bool ok = run_different_layout_order(true) &&
              run_different_layout_order(false) &&
              run_identical_layout_sharing() &&
              run_malformed_layout_cases() &&
              run_cycle_survivor_edges() &&
              run_residual_vm_owner_contract() &&
              run_managed_arithmetic_trap_cleanup();
    if (!ok) return 1;
    puts("CVM class metadata identity and lifetime: PASS");
    return 0;
}
