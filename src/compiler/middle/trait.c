/*
 * Casprix Compiler - Trait System Implementation
 */

#include "compiler/middle/trait.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void init_traits(TraitContext* ctx) {
    ctx->traits = NULL;
    ctx->trait_count = 0;
    ctx->implementations = NULL;
    ctx->impl_count = 0;
}

// Define new trait
Trait* define_trait(const char* name, TraitMethod* methods, int method_count) {
    Trait* trait = malloc(sizeof(Trait));
    trait->name = strdup(name);
    trait->methods = malloc(method_count * sizeof(TraitMethod));
    trait->method_count = method_count;
    trait->super_traits = NULL;
    trait->super_count = 0;
    
    // Copy methods
    for (int i = 0; i < method_count; i++) {
        trait->methods[i].name = strdup(methods[i].name);
        trait->methods[i].return_type = methods[i].return_type;
        trait->methods[i].param_count = methods[i].param_count;
        
        if (methods[i].param_count > 0) {
            trait->methods[i].parameters = malloc(methods[i].param_count * sizeof(Parameter));
            memcpy(trait->methods[i].parameters, methods[i].parameters,
                   methods[i].param_count * sizeof(Parameter));
        } else {
            trait->methods[i].parameters = NULL;
        }
    }
    
    return trait;
}

// Get trait method
TraitMethod* get_trait_method(Trait* trait, const char* method_name) {
    for (int i = 0; i < trait->method_count; i++) {
        if (strcmp(trait->methods[i].name, method_name) == 0) {
            return &trait->methods[i];
        }
    }
    return NULL;
}

// Verify trait implementation
bool verify_trait_impl(Trait* trait, ClassStmt* class_stmt) {
    // Check that class has all required methods
    for (int i = 0; i < trait->method_count; i++) {
        TraitMethod* required = &trait->methods[i];
        bool found = false;
        
        // Search class methods
        for (int j = 0; j < class_stmt->method_count; j++) {
            MethodDecl* method = &class_stmt->methods[j];
            
            if (strcmp(method->name, required->name) == 0) {
                // Check signature matches
                if (method->return_type != required->return_type) {
                    fprintf(stderr, "Return type mismatch for %s in trait %s\n",
                            required->name, trait->name);
                    return false;
                }
                
                if (method->param_count != required->param_count) {
                    fprintf(stderr, "Parameter count mismatch for %s in trait %s\n",
                            required->name, trait->name);
                    return false;
                }
                
                found = true;
                break;
            }
        }
        
        if (!found) {
            fprintf(stderr, "Method %s required by trait %s not found in class %s\n",
                    required->name, trait->name, class_stmt->name);
            return false;
        }
    }
    
    return true;
}

// Implement trait for class
bool implement_trait(TraitContext* ctx, Trait* trait, ClassStmt* class_stmt) {
    // Verify implementation
    if (!verify_trait_impl(trait, class_stmt)) {
        return false;
    }
    
    // Create impl record
    TraitImpl* impl = malloc(sizeof(TraitImpl));
    impl->trait = trait;
    impl->implementing_class = class_stmt;
    impl->method_impls = NULL;  // Would link to actual methods
    impl->impl_count = trait->method_count;
    
    // Add to context
    TraitImpl** new_impls = realloc(ctx->implementations,
                                    (ctx->impl_count + 1) * sizeof(TraitImpl*));
    if (!new_impls) {
        free(impl);
        return false;
    }
    ctx->implementations = new_impls;
    ctx->implementations[ctx->impl_count++] = impl;
    
    printf("Class %s implements trait %s\n", class_stmt->name, trait->name);
    
    return true;
}

// Check if class implements trait
bool class_implements_trait(TraitContext* ctx, ClassStmt* class_stmt, const char* trait_name) {
    for (int i = 0; i < ctx->impl_count; i++) {
        TraitImpl* impl = ctx->implementations[i];
        
        if (impl->implementing_class == class_stmt &&
            strcmp(impl->trait->name, trait_name) == 0) {
            return true;
        }
    }
    return false;
}

void free_traits(TraitContext* ctx) {
    if (!ctx) return;
    
    // Free traits
    for (int i = 0; i < ctx->trait_count; i++) {
        Trait* trait = ctx->traits[i];
        free(trait->name);
        
        for (int j = 0; j < trait->method_count; j++) {
            free(trait->methods[j].name);
            if (trait->methods[j].parameters) {
                free(trait->methods[j].parameters);
            }
        }
        free(trait->methods);
        free(trait);
    }
    free(ctx->traits);
    
    // Free implementations
    for (int i = 0; i < ctx->impl_count; i++) {
        free(ctx->implementations[i]);
    }
    free(ctx->implementations);
}
