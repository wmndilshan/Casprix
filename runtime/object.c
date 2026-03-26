// Casperix Runtime - Object System Implementation

#include "object.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Global state
static TypeId g_next_type_id = TYPE_ID_OBJECT + 1;
static ObjectStats g_obj_stats = {0};

// --- Object Lifecycle ---

void* obj_alloc(VTable* vtable, arc_destructor_fn destructor, arc_scan_fn scanner) {
    if (!vtable) return NULL;

    // Allocate via ARC: the user data region is [ObjectHeader][fields...]
    void* obj = arc_alloc_full(vtable->instance_size, destructor, scanner);
    if (!obj) return NULL;

    // Initialize object header
    ObjectHeader* header = (ObjectHeader*)obj;
    header->vtable = vtable;

    // Fields are already zero-initialized by arc_alloc_full
    g_obj_stats.objects_created++;

    return obj;
}

// --- Type Checking ---

bool obj_is_instance(void* obj, TypeId type_id) {
    g_obj_stats.type_checks++;

    if (!obj) return false;
    VTable* vt = obj_get_vtable(obj);

    // Walk the inheritance chain
    while (vt) {
        if (vt->type_id == type_id) return true;
        vt = vt->parent;
    }
    return false;
}

bool obj_is_instance_of(void* obj, VTable* target_vtable) {
    if (!obj || !target_vtable) return false;
    return obj_is_instance(obj, target_vtable->type_id);
}

void* obj_dynamic_cast(void* obj, TypeId target_type_id) {
    g_obj_stats.dynamic_casts++;

    if (obj_is_instance(obj, target_type_id)) {
        return obj;
    }
    g_obj_stats.cast_failures++;
    return NULL;
}

// --- VTable Management ---

TypeId obj_next_type_id(void) {
    return g_next_type_id++;
}

VTable* vtable_create(const char* class_name, VTable* parent,
                      uint32_t instance_size, uint32_t num_methods,
                      uint32_t num_fields) {
    VTable* vt = (VTable*)malloc(sizeof(VTable));
    if (!vt) return NULL;

    vt->class_name = class_name;
    vt->type_id = obj_next_type_id();
    vt->parent = parent;
    vt->instance_size = instance_size;
    vt->num_methods = num_methods;
    vt->num_fields = num_fields;

    if (num_methods > 0) {
        vt->methods = (void**)calloc(num_methods, sizeof(void*));
        if (!vt->methods) {
            free(vt);
            return NULL;
        }

        // Inherit parent methods if any
        if (parent && parent->methods) {
            uint32_t copy_count = parent->num_methods < num_methods
                                ? parent->num_methods : num_methods;
            memcpy(vt->methods, parent->methods, copy_count * sizeof(void*));
        }
    } else {
        vt->methods = NULL;
    }

    g_obj_stats.vtables_created++;
    return vt;
}

void vtable_set_method(VTable* vt, uint32_t index, void* method_ptr) {
    if (!vt || index >= vt->num_methods) return;
    vt->methods[index] = method_ptr;
}

void vtable_destroy(VTable* vt) {
    if (!vt) return;
    free(vt->methods);
    free(vt);
}

// --- Statistics ---

ObjectStats obj_get_stats(void) {
    return g_obj_stats;
}

void obj_print_stats(void) {
    printf("=== Object System Statistics ===\n");
    printf("Objects created:   %llu\n", (unsigned long long)g_obj_stats.objects_created);
    printf("VTables created:   %llu\n", (unsigned long long)g_obj_stats.vtables_created);
    printf("Virtual calls:     %llu\n", (unsigned long long)g_obj_stats.virtual_calls);
    printf("Type checks:       %llu\n", (unsigned long long)g_obj_stats.type_checks);
    printf("Dynamic casts:     %llu\n", (unsigned long long)g_obj_stats.dynamic_casts);
    printf("Cast failures:     %llu\n", (unsigned long long)g_obj_stats.cast_failures);
    printf("================================\n");
}
