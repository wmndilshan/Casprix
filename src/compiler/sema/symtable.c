#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include "compiler/sema/symtable.h"

void init_symbol_table(SymbolTable* table) {
    table->count = 0;
    table->capacity = 16;
    table->symbols = ALLOCATE(Symbol*, table->capacity);
}

void free_symbol_table(SymbolTable* table) {
    for (int i = 0; i < table->count; i++) {
        free(table->symbols[i]->name);
        if (table->symbols[i]->param_types) {
            free(table->symbols[i]->param_types);
        }
        if (table->symbols[i]->closure_capture_types) {
            free(table->symbols[i]->closure_capture_types);
        }
        if (table->symbols[i]->ownership_data) {
            free(table->symbols[i]->ownership_data);
        }
        if (table->symbols[i]->kind == SYMBOL_CLASS && table->symbols[i]->class_info) {
            ClassSymbol* cls = table->symbols[i]->class_info;
            free(cls->name);
            // Free fields
            for (int j = 0; j < cls->field_count; j++) {
                free(cls->fields[j].name);
                if (cls->fields[j].class_name) {
                    free(cls->fields[j].class_name);
                }
            }
            if (cls->fields) free(cls->fields);
            // Free methods
            for (int j = 0; j < cls->method_count; j++) {
                free(cls->methods[j].name);
                if (cls->methods[j].param_types) {
                    free(cls->methods[j].param_types);
                }
            }
            if (cls->methods) free(cls->methods);
            free(cls);
        }
        free(table->symbols[i]);
    }
    FREE_ARRAY(Symbol*, table->symbols, table->capacity);
}

bool add_symbol(SymbolTable* table, const char* name, SymbolKind kind,
                DataType type, int scope_depth) {
    // Check for redeclaration in current scope
    for (int i = table->count - 1; i >= 0; i--) {
        if (table->symbols[i]->scope_depth < scope_depth) break;
        
        if (strcmp(table->symbols[i]->name, name) == 0) {
            return false; // Already declared in this scope
        }
    }
    
    // Grow if needed
    if (table->count >= table->capacity) {
        int old_capacity = table->capacity;
        table->capacity = GROW_CAPACITY(old_capacity);
        table->symbols = GROW_ARRAY(Symbol*, table->symbols, old_capacity, table->capacity);
    }
    
    Symbol* symbol = ALLOCATE(Symbol, 1);
    symbol->name = strdup(name);
    symbol->kind = kind;
    symbol->type = type;
    symbol->scope_depth = scope_depth;
    symbol->is_initialized = false;
    symbol->is_mutable = true;
    symbol->is_const = false;
    symbol->param_types = NULL;
    symbol->param_count = 0;
    symbol->return_type = TYPE_ERROR;
    symbol->class_info = NULL;
    symbol->is_extern = false;
    symbol->is_async = false;
    symbol->is_closure_value = false;
    symbol->closure_capture_count = 0;
    symbol->closure_capture_types = NULL;
    symbol->closure_lambda_id = -1;
    symbol->ownership_data = NULL;

    table->symbols[table->count++] = symbol;
    return true;
}

bool add_function(SymbolTable* table, const char* name, DataType return_type,
                  DataType* param_types, int param_count, int scope_depth) {
    if (!add_symbol(table, name, SYMBOL_FUNCTION, return_type, scope_depth)) {
        return false;
    }
    
    Symbol* func = table->symbols[table->count - 1];
    func->return_type = return_type;
    func->param_count = param_count;
    
    if (param_count > 0) {
        func->param_types = ALLOCATE(DataType, param_count);
        memcpy(func->param_types, param_types, sizeof(DataType) * param_count);
    }
    
    return true;
}

bool add_extern_function(SymbolTable* table, const char* name, DataType return_type,
                         DataType* param_types, int param_count, int scope_depth) {
    if (!add_function(table, name, return_type, param_types, param_count, scope_depth)) {
        return false;
    }
    
    Symbol* func = table->symbols[table->count - 1];
    func->is_extern = true;
    return true;
}

Symbol* lookup_symbol(SymbolTable* table, const char* name) {
    // Search from most recent (innermost scope) to oldest
    for (int i = table->count - 1; i >= 0; i--) {
        if (strcmp(table->symbols[i]->name, name) == 0) {
            return table->symbols[i];
        }
    }
    return NULL;
}

void enter_scope(SymbolTable* table, int* scope_depth) {
    (*scope_depth)++;
}

void exit_scope(SymbolTable* table, int* scope_depth) {
    // Remove all symbols from the exiting scope
    while (table->count > 0 && 
           table->symbols[table->count - 1]->scope_depth == *scope_depth) {
        Symbol* symbol = table->symbols[--table->count];
        free(symbol->name);
        if (symbol->param_types) {
            free(symbol->param_types);
        }
        if (symbol->closure_capture_types) {
            free(symbol->closure_capture_types);
        }
        if (symbol->ownership_data) {
            free(symbol->ownership_data);
        }
        free(symbol);
    }
    (*scope_depth)--;
}

void print_symbol_table(SymbolTable* table) {
    printf("\n=== Symbol Table ===\n");
    for (int i = 0; i < table->count; i++) {
        Symbol* sym = table->symbols[i];
        printf("%-20s | Kind: %d | Type: %d | Scope: %d\n",
               sym->name, sym->kind, sym->type, sym->scope_depth);
    }
    printf("====================\n\n");
}

// Class-specific operations
bool add_class(SymbolTable* table, const char* name, const char* parent_name, int scope_depth) {
    // Check for redeclaration
    for (int i = table->count - 1; i >= 0; i--) {
        if (table->symbols[i]->scope_depth < scope_depth) break;
        if (strcmp(table->symbols[i]->name, name) == 0) {
            return false; // Already declared
        }
    }

    // Create class symbol
    if (!add_symbol(table, name, SYMBOL_CLASS, TYPE_CLASS, scope_depth)) {
        return false;
    }

    Symbol* class_symbol = table->symbols[table->count - 1];
    ClassSymbol* cls = ALLOCATE(ClassSymbol, 1);
    cls->name = strdup(name);
    cls->parent = NULL;
    cls->is_abstract = false;  //Will be set to true if class has abstract methods
    cls->fields = NULL;
    cls->field_count = 0;
    cls->methods = NULL;
    cls->method_count = 0;
    cls->instance_size = 8; // Start with VTable pointer (8 bytes on x64)

    // Resolve parent if specified
    if (parent_name) {
        ClassSymbol* parent = lookup_class(table, parent_name);
        if (!parent) {
            free(cls->name);
            free(cls);
            table->count--; // Remove the symbol we just added
            return false; // Parent class not found
        }
        cls->parent = parent;
        cls->parent_class = strdup(parent_name);  // Store parent class name
        cls->instance_size = parent->instance_size; // Inherit parent size
    } else {
        cls->parent_class = NULL;
    }

    class_symbol->class_info = cls;
    return true;
}

ClassSymbol* lookup_class(SymbolTable* table, const char* name) {
    for (int i = table->count - 1; i >= 0; i--) {
        if (table->symbols[i]->kind == SYMBOL_CLASS &&
            strcmp(table->symbols[i]->name, name) == 0) {
            return table->symbols[i]->class_info;
        }
    }
    return NULL;
}

bool add_field_to_class(ClassSymbol* class_sym, const char* name, DataType type,
                        const char* class_name, bool is_static, AccessModifier access,
                        bool is_const, bool is_mutable) {
    // Check for duplicate field
    for (int i = 0; i < class_sym->field_count; i++) {
        if (strcmp(class_sym->fields[i].name, name) == 0) {
            return false; // Duplicate field
        }
    }

    // Check parent for duplicate (only for instance fields)
    if (!is_static && class_sym->parent) {
        if (find_field(class_sym->parent, name) != NULL) {
            return false; // Field already in parent
        }
    }

    // Allocate space for new field
    class_sym->fields = realloc(class_sym->fields,
                                sizeof(FieldSymbol) * (class_sym->field_count + 1));

    FieldSymbol* field = &class_sym->fields[class_sym->field_count];
    field->name = strdup(name);
    field->type = type;
    field->class_name = class_name ? strdup(class_name) : NULL;
    field->is_const = is_const;
    field->is_mutable = is_mutable;
    field->is_static = is_static;
    field->access = access;
    field->offset = is_static ? -1 : class_sym->instance_size;  // Static fields don't have instance offset

    // Calculate field size based on type (only for instance fields)
    if (!is_static) {
        int field_size = 8; // Default to 8 bytes (pointer/int64)
        switch (type) {
            case TYPE_INT:
            case TYPE_FLOAT:
            case TYPE_BOOL:
            case TYPE_STRING:
            case TYPE_CLASS:
                field_size = 8;
                break;
            default:
                field_size = 8;
        }

        class_sym->instance_size += field_size;
    }
    class_sym->field_count++;
    return true;
}

bool add_method_to_class(ClassSymbol* class_sym, const char* name, DataType return_type,
                         DataType* param_types, int param_count, bool is_constructor, bool is_static, AccessModifier access) {
    // Check for duplicate method in this class
    for (int i = 0; i < class_sym->method_count; i++) {
        if (strcmp(class_sym->methods[i].name, name) == 0) {
            return false; // Duplicate method
        }
    }

    // Check if overriding parent method (static methods can't override)
    int vtable_index = -1;
    if (!is_constructor && !is_static && class_sym->parent) {
        MethodSymbol* parent_method = find_method(class_sym->parent, name);
        if (parent_method) {
            // Override: verify signature matches
            if (parent_method->return_type != return_type ||
                parent_method->param_count != param_count) {
                return false; // Signature mismatch
            }
            for (int i = 0; i < param_count; i++) {
                if (parent_method->param_types[i] != param_types[i]) {
                    return false; // Parameter type mismatch
                }
            }
            vtable_index = parent_method->vtable_index; // Use same VTable slot
        }
    }

    // Assign VTable index if not overriding (static methods and constructors get -1)
    if (!is_constructor && !is_static && vtable_index == -1) {
        // Count total methods in hierarchy (current class + all parents)
        int max_index = -1;
        // Check methods already added to this class
        for (int i = 0; i < class_sym->method_count; i++) {
            if (class_sym->methods[i].vtable_index > max_index) {
                max_index = class_sym->methods[i].vtable_index;
            }
        }
        // Check parent classes
        ClassSymbol* current = class_sym->parent;
        while (current) {
            for (int i = 0; i < current->method_count; i++) {
                if (current->methods[i].vtable_index > max_index) {
                    max_index = current->methods[i].vtable_index;
                }
            }
            current = current->parent;
        }
        vtable_index = max_index + 1;
    }

    // Allocate space for new method
    class_sym->methods = realloc(class_sym->methods,
                                 sizeof(MethodSymbol) * (class_sym->method_count + 1));

    MethodSymbol* method = &class_sym->methods[class_sym->method_count];
    method->name = strdup(name);
    method->return_type = return_type;
    method->return_class_name = NULL;  // Can be set later if needed
    method->param_count = param_count;
    method->is_static = is_static;
    method->is_constructor = is_constructor;
    method->is_abstract = false;  // Will be set from AST if needed
    method->vtable_index = vtable_index;
    method->access = access;

    if (param_count > 0) {
        method->param_types = ALLOCATE(DataType, param_count);
        memcpy(method->param_types, param_types, sizeof(DataType) * param_count);
    } else {
        method->param_types = NULL;
    }

    class_sym->method_count++;
    return true;
}

FieldSymbol* find_field(ClassSymbol* class_sym, const char* name) {
    // Search this class
    for (int i = 0; i < class_sym->field_count; i++) {
        if (strcmp(class_sym->fields[i].name, name) == 0) {
            return &class_sym->fields[i];
        }
    }

    // Search parent
    if (class_sym->parent) {
        return find_field(class_sym->parent, name);
    }

    return NULL;
}

MethodSymbol* find_method(ClassSymbol* class_sym, const char* name) {
    // Search this class
    for (int i = 0; i < class_sym->method_count; i++) {
        if (strcmp(class_sym->methods[i].name, name) == 0) {
            return &class_sym->methods[i];
        }
    }

    // Search parent
    if (class_sym->parent) {
        return find_method(class_sym->parent, name);
    }

    return NULL;
}

bool is_subclass_of(ClassSymbol* child, ClassSymbol* parent) {
    if (!child || !parent) return false;

    ClassSymbol* current = child->parent;
    while (current) {
        if (current == parent) {
            return true;
        }
        current = current->parent;
    }

    return false;
}
