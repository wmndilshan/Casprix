// Casperix Runtime - Object System
// Unified object layout: [ArcHeader][ObjectHeader][fields...]
//
// Every class instance has:
//   1. ArcHeader (from arc.h) - reference counting, destructor, cycle scanner
//   2. ObjectHeader - vtable pointer, type ID
//   3. Fields - inline field data
//
// VTable layout:
//   [class_name][type_id][parent_vtable][instance_size][num_methods]
//   [method_0_ptr][method_1_ptr]...[method_N_ptr]
//
// The compiler generates:
//   - VTable data in .data section
//   - Destructor function (__dtor_ClassName) for releasing ref-counted fields
//   - Scanner function (__scan_ClassName) for cycle collector
//   - Allocates objects via obj_alloc() instead of raw malloc

#ifndef CASPERIX_OBJECT_H
#define CASPERIX_OBJECT_H

#include "memory/arc.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Type ID for runtime type checking
typedef uint32_t TypeId;

// Special type IDs
#define TYPE_ID_INVALID  0
#define TYPE_ID_OBJECT   1   // Base object type

// --- VTable ---

typedef struct VTable {
    const char* class_name;        // Class name (for debugging/RTTI)
    TypeId type_id;                // Unique type identifier
    struct VTable* parent;         // Parent class vtable (NULL for root)
    uint32_t instance_size;        // Total size of object (including ObjectHeader)
    uint32_t num_methods;          // Number of entries in methods array
    uint32_t num_fields;           // Number of instance fields
    void** methods;                // Array of function pointers (virtual methods)
} VTable;

// --- Object Header ---
// Placed immediately after ArcHeader in memory

typedef struct {
    VTable* vtable;                // Pointer to class vtable
} ObjectHeader;

// --- Object Memory Layout ---
// Full layout: [ArcHeader][ObjectHeader][field_0][field_1]...[field_N]
//
// Access patterns:
//   void* arc_ptr = arc_alloc(...)     -> points to ObjectHeader
//   ObjectHeader* obj = (ObjectHeader*)arc_ptr
//   obj->vtable                        -> VTable*
//   (char*)obj + sizeof(ObjectHeader)  -> first field
//   (char*)obj + field_offset          -> specific field

// Field offset from the start of user data (after ArcHeader)
// ObjectHeader is sizeof(void*) = 8 bytes on x64
#define OBJ_HEADER_SIZE    sizeof(ObjectHeader)
#define OBJ_FIELD_OFFSET   OBJ_HEADER_SIZE

// --- Object Lifecycle API ---

// Allocate a new object of the given class
// - size: total user data size (ObjectHeader + all fields)
// - vtable: pointer to the class VTable
// - destructor: generated destructor for releasing ref-counted fields
// - scanner: generated scanner for cycle collector (NULL if no ref fields)
// Returns pointer to ObjectHeader (which is also the ARC user data pointer)
void* obj_alloc(VTable* vtable, arc_destructor_fn destructor, arc_scan_fn scanner);

// Get the vtable from an object pointer
static inline VTable* obj_get_vtable(void* obj) {
    if (!obj) return NULL;
    return ((ObjectHeader*)obj)->vtable;
}

// Get class name from object
static inline const char* obj_get_class_name(void* obj) {
    VTable* vt = obj_get_vtable(obj);
    return vt ? vt->class_name : "<null>";
}

// Get type ID from object
static inline TypeId obj_get_type_id(void* obj) {
    VTable* vt = obj_get_vtable(obj);
    return vt ? vt->type_id : TYPE_ID_INVALID;
}

// --- Field Access ---

// Get pointer to a field by byte offset (from start of user data)
static inline void* obj_field_ptr(void* obj, uint32_t offset) {
    return (char*)obj + offset;
}

// Read/write field helpers for common types
static inline int64_t obj_get_int(void* obj, uint32_t offset) {
    return *(int64_t*)((char*)obj + offset);
}

static inline void obj_set_int(void* obj, uint32_t offset, int64_t value) {
    *(int64_t*)((char*)obj + offset) = value;
}

static inline double obj_get_float(void* obj, uint32_t offset) {
    return *(double*)((char*)obj + offset);
}

static inline void obj_set_float(void* obj, uint32_t offset, double value) {
    *(double*)((char*)obj + offset) = value;
}

static inline void* obj_get_ref(void* obj, uint32_t offset) {
    return *(void**)((char*)obj + offset);
}

static inline void obj_set_ref(void* obj, uint32_t offset, void* value) {
    *(void**)((char*)obj + offset) = value;
}

// --- Virtual Method Dispatch ---

// Call a virtual method by vtable index
// Returns the function pointer; caller casts to correct signature
static inline void* obj_virtual_method(void* obj, uint32_t method_index) {
    VTable* vt = obj_get_vtable(obj);
    if (!vt || method_index >= vt->num_methods) return NULL;
    return vt->methods[method_index];
}

// --- Type Checking (RTTI) ---

// Check if object is an instance of the class with given type_id
bool obj_is_instance(void* obj, TypeId type_id);

// Check if object is an instance of the class or any subclass
bool obj_is_instance_of(void* obj, VTable* target_vtable);

// Dynamic cast: returns obj if it's an instance of target type, NULL otherwise
void* obj_dynamic_cast(void* obj, TypeId target_type_id);

// --- VTable Management ---

// Global type ID counter
TypeId obj_next_type_id(void);

// Create a vtable (usually called from compiler-generated init code)
VTable* vtable_create(const char* class_name, VTable* parent,
                      uint32_t instance_size, uint32_t num_methods,
                      uint32_t num_fields);

// Set a method in the vtable
void vtable_set_method(VTable* vt, uint32_t index, void* method_ptr);

// Free a vtable (usually called at program shutdown)
void vtable_destroy(VTable* vt);

// --- Object Reference Helpers ---
// These manage ARC reference counts when assigning object fields

// Assign a ref-counted field: retains new value, releases old value
// obj: the object containing the field
// offset: byte offset of the field
// new_value: new object reference to store
static inline void obj_assign_ref(void* obj, uint32_t offset, void* new_value) {
    void** field = (void**)((char*)obj + offset);
    void* old = *field;
    if (new_value) arc_retain(new_value);
    *field = new_value;
    if (old) arc_release(old);
}

// --- Statistics ---

typedef struct {
    size_t objects_created;
    size_t vtables_created;
    size_t virtual_calls;
    size_t type_checks;
    size_t dynamic_casts;
    size_t cast_failures;
} ObjectStats;

ObjectStats obj_get_stats(void);
void obj_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif // CASPERIX_OBJECT_H
