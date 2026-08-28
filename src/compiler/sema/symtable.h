#ifndef SYMTABLE_H
#define SYMTABLE_H

#include "casprix/common.h"
#include "compiler/frontend/ast.h"

typedef enum {
    SYMBOL_VARIABLE,
    SYMBOL_FUNCTION,
    SYMBOL_PARAMETER,
    SYMBOL_CLASS,
    SYMBOL_TRAIT
} SymbolKind;

// Forward declarations
typedef struct FieldSymbol FieldSymbol;
typedef struct MethodSymbol MethodSymbol;
typedef struct ClassSymbol ClassSymbol;
typedef struct TraitMethodSymbol TraitMethodSymbol;
typedef struct TraitSymbol TraitSymbol;

// Field symbol for class members
struct FieldSymbol {
    char* name;
    DataType type;
    char* class_name;  // For TYPE_CLASS fields, the specific class name
    int offset;  // Byte offset from object base (after VTable pointer)
    bool is_const;     // True for const/read-only fields
    bool is_mutable;   // True for mut fields and legacy mutable fields
    bool is_static;    // True for static fields (class-level, not instance)
    AccessModifier access;  // Access control (Public/Private/Protected)
};

// Method symbol for class methods
struct MethodSymbol {
    char* name;
    DataType return_type;
    char* return_class_name;  // For return_type == TYPE_CLASS, the specific class name
    DataType* param_types;
    int param_count;
    bool is_constructor;
    bool is_static;    // True for static methods (no 'this' parameter)
    bool is_abstract;  // True for abstract methods (no implementation)
    int vtable_index;  // Index in VTable (-1 for constructors/static methods)
    AccessModifier access;  // Access control (Public/Private/Protected)
};

// Class symbol
struct ClassSymbol {
    char* name;
    char* parent_class;  // Name of parent class (NULL for base classes)
    struct ClassSymbol* parent;  // Pointer to parent ClassSymbol (NULL for base classes)
    bool is_abstract;  // True if class is abstract (cannot be instantiated)
    FieldSymbol* fields;
    int field_count;
    MethodSymbol* methods;
    int method_count;
    int instance_size;  // Total size in bytes (VTable pointer + all fields)
    char* return_class_name;  // For methods that return a class type
};

// Trait method signature (required-method contract)
struct TraitMethodSymbol {
    char* name;
    DataType return_type;
    char* return_class_name;  // For return_type == TYPE_CLASS
    DataType* param_types;
    int param_count;
    bool has_default;         // True if the trait provides a default implementation
};

// Trait symbol
struct TraitSymbol {
    char* name;
    TraitMethodSymbol* methods;
    int method_count;
};

typedef struct {
    char* name;
    SymbolKind kind;
    DataType type;
    int scope_depth;
    bool is_initialized;
    bool is_mutable;
    bool is_const;

    // For functions
    DataType* param_types;
    int param_count;
    DataType return_type;
    /* When this symbol's callable type has a structured signature that the flat
     * DataType fields cannot express (e.g. `-> lambda(int) -> int`, or a
     * lambda-typed parameter), this holds the full function TypeInfo. Owned by
     * the AST (a borrowed pointer into a Stmt/Parameter); not freed here. */
    struct TypeInfo* return_type_info;
    bool is_extern;
    bool is_async;         // True for async functions
    bool is_closure_value; // Function-valued binding backed by a closure handle
    int closure_capture_count;
    DataType* closure_capture_types;
    int closure_lambda_id;

    // For classes
    ClassSymbol* class_info;

    // For traits
    TraitSymbol* trait_info;

    // Ownership tracking (Casperix)
    void* ownership_data;  // Points to OwnershipInfo structure
} Symbol;

typedef struct {
    Symbol** symbols;
    int count;
    int capacity;
} SymbolTable;

// Symbol table operations
void init_symbol_table(SymbolTable* table);
void free_symbol_table(SymbolTable* table);

bool add_symbol(SymbolTable* table, const char* name, SymbolKind kind, 
                DataType type, int scope_depth);
bool add_function(SymbolTable* table, const char* name, DataType return_type,
                  DataType* param_types, int param_count, int scope_depth);
bool add_extern_function(SymbolTable* table, const char* name, DataType return_type,
                         DataType* param_types, int param_count, int scope_depth);

Symbol* lookup_symbol(SymbolTable* table, const char* name);
void enter_scope(SymbolTable* table, int* scope_depth);
void exit_scope(SymbolTable* table, int* scope_depth);

void print_symbol_table(SymbolTable* table);

// Class-specific operations
bool add_class(SymbolTable* table, const char* name, const char* parent_name, int scope_depth);
ClassSymbol* lookup_class(SymbolTable* table, const char* name);
bool add_field_to_class(ClassSymbol* class_sym, const char* name, DataType type,
                        const char* class_name, bool is_static, AccessModifier access,
                        bool is_const, bool is_mutable);
bool add_method_to_class(ClassSymbol* class_sym, const char* name, DataType return_type,
                         DataType* param_types, int param_count, bool is_constructor, bool is_static, AccessModifier access);
FieldSymbol* find_field(ClassSymbol* class_sym, const char* name);
MethodSymbol* find_method(ClassSymbol* class_sym, const char* name);
bool is_subclass_of(ClassSymbol* child, ClassSymbol* parent);

// Trait-specific operations
bool add_trait(SymbolTable* table, const char* name, int scope_depth);
TraitSymbol* lookup_trait(SymbolTable* table, const char* name);
bool add_method_to_trait(TraitSymbol* trait_sym, const char* name, DataType return_type,
                         const char* return_class_name, DataType* param_types,
                         int param_count, bool has_default);
TraitMethodSymbol* find_trait_method(TraitSymbol* trait_sym, const char* name);

#endif // SYMTABLE_H
