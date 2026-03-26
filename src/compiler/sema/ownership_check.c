/**
 * Casperix Compiler - Ownership & Borrowing Checker Implementation
 */

#include "ownership_check.h"
#include "compiler/sema/symtable.h"
#include "support/log.h"
#include <string.h>
#include <stdlib.h>

// Get ownership info from symbol (add to Symbol struct if needed)
static OwnershipInfo* get_ownership_info(Symbol* sym) {
    // For now, we'll store it in Symbol's extra data
    // TODO: Add OwnershipInfo* field to Symbol struct
    if (!sym->ownership_data) {
        sym->ownership_data = calloc(1, sizeof(OwnershipInfo));
        OwnershipInfo* info = (OwnershipInfo*)sym->ownership_data;
        info->state = OWNERSHIP_OWNED;
        info->borrow_count = 0;
        info->has_mut_borrow = false;
    }
    return (OwnershipInfo*)sym->ownership_data;
}

void ownership_checker_init(OwnershipChecker* checker, SemanticAnalyzer* analyzer) {
    checker->analyzer = analyzer;
    checker->current_scope = 0;
    checker->in_unsafe_block = false;
}

bool check_ownership_valid(OwnershipChecker* checker, const char* var_name, int line) {
    // Allow everything in unsafe blocks
    if (checker->in_unsafe_block) {
        return true;
    }

    Symbol* sym = lookup_symbol(checker->analyzer->symbols, var_name);
    if (!sym) {
        // Variable not found - let semantic analyzer handle this
        return true;
    }

    OwnershipInfo* info = get_ownership_info(sym);
    
    if (info->state == OWNERSHIP_MOVED) {
        CPX_LOG(CPX_LOG_ERROR, CPX_LOG_CAT_SEMANTIC,                  "[%d:%d] Use of moved value '%s' (moved at line %d)",
                 line, 0, var_name, info->move_location);
        return false;
    }

    return true;
}

void mark_moved(OwnershipChecker* checker, const char* var_name, int line) {
    if (checker->in_unsafe_block) {
        return;  // No tracking in unsafe
    }

    Symbol* sym = lookup_symbol(checker->analyzer->symbols, var_name);
    if (!sym) return;

    OwnershipInfo* info = get_ownership_info(sym);
    
    // Can't move if there are active borrows
    if (info->borrow_count > 0 || info->has_mut_borrow) {
        CPX_LOG(CPX_LOG_ERROR, CPX_LOG_CAT_SEMANTIC,                  "[%d:%d] Cannot move '%s' while borrowed",
                 line, 0, var_name);
        return;
    }

    info->state = OWNERSHIP_MOVED;
    info->move_location = line;
    
    CPX_LOG(CPX_LOG_DEBUG, CPX_LOG_CAT_SEMANTIC, "Marked '%s' as moved at line %d", var_name, line);
}

bool add_borrow(OwnershipChecker* checker, const char* var_name, int line) {
    if (checker->in_unsafe_block) {
        return true;
    }

    Symbol* sym = lookup_symbol(checker->analyzer->symbols, var_name);
    if (!sym) return false;

    OwnershipInfo* info = get_ownership_info(sym);
    
    // Can't borrow if already has mutable borrow
    if (info->has_mut_borrow) {
        CPX_LOG(CPX_LOG_ERROR, CPX_LOG_CAT_SEMANTIC,                  "[%d:%d] Cannot borrow '%s' immutably while mutably borrowed",
                 line, 0, var_name);
        return false;
    }

    // Can't borrow if moved
    if (info->state == OWNERSHIP_MOVED) {
        CPX_LOG(CPX_LOG_ERROR, CPX_LOG_CAT_SEMANTIC,                  "[%d:%d] Cannot borrow moved value '%s'",
                 line, 0, var_name);
        return false;
    }

    info->borrow_count++;
    info->state = OWNERSHIP_BORROW;
    
    CPX_LOG(CPX_LOG_DEBUG, CPX_LOG_CAT_SEMANTIC, "Added borrow to '%s' (count: %d)", 
             var_name, info->borrow_count);
    
    return true;
}

bool add_mut_borrow(OwnershipChecker* checker, const char* var_name, int line) {
    if (checker->in_unsafe_block) {
        return true;
    }

    Symbol* sym = lookup_symbol(checker->analyzer->symbols, var_name);
    if (!sym) return false;

    OwnershipInfo* info = get_ownership_info(sym);
    
    // Can't have mutable borrow if any borrows exist
    if (info->borrow_count > 0 || info->has_mut_borrow) {
        CPX_LOG(CPX_LOG_ERROR, CPX_LOG_CAT_SEMANTIC,                  "[%d:%d] Cannot borrow '%s' mutably while borrowed",
                 line, 0, var_name);
        return false;
    }

    // Can't borrow if moved
    if (info->state == OWNERSHIP_MOVED) {
        CPX_LOG(CPX_LOG_ERROR, CPX_LOG_CAT_SEMANTIC,                  "[%d:%d] Cannot borrow moved value '%s'",
                 line, 0, var_name);
        return false;
    }

    info->has_mut_borrow = true;
    info->state = OWNERSHIP_BORROW_MUT;
    
    CPX_LOG(CPX_LOG_DEBUG, CPX_LOG_CAT_SEMANTIC, "Added mutable borrow to '%s'", var_name);
    
    return true;
}

void release_borrow(OwnershipChecker* checker, const char* var_name) {
    if (checker->in_unsafe_block) {
        return;
    }

    Symbol* sym = lookup_symbol(checker->analyzer->symbols, var_name);
    if (!sym) return;

    OwnershipInfo* info = get_ownership_info(sym);
    
    if (info->has_mut_borrow) {
        info->has_mut_borrow = false;
    } else if (info->borrow_count > 0) {
        info->borrow_count--;
    }

    // Return to owned state if no borrows left
    if (info->borrow_count == 0 && !info->has_mut_borrow) {
        info->state = OWNERSHIP_OWNED;
    }
}

bool is_move_expr(Expr* expr) {
    // A variable prefixed with `move` keyword has is_move = true
    if (!expr) return false;
    if (expr->type == EXPR_VARIABLE && expr->as.variable.is_move) {
        return true;
    }
    return false;
}

void validate_scope_end(OwnershipChecker* checker, int scope_level) {
    /*
     * At end of scope:
     *   1. Release all borrows held by variables at this scope level
     *   2. Warn about owned values that were never used (potential leak)
     *
     * This iterates the symbol table's current scope for symbols declared
     * at `scope_level` and cleans up their ownership state.
     */
    if (!checker || !checker->analyzer || !checker->analyzer->symbols) {
        return;
    }

    SymbolTable* table = checker->analyzer->symbols;

    /* Walk all symbols — those at `scope_level` are going out of scope */
    for (int i = 0; i < table->count; i++) {
        Symbol* sym = table->symbols[i];
        if (!sym) continue;
        if (sym->scope_depth != scope_level) continue;
        if (!sym->ownership_data) continue;

        OwnershipInfo* info = (OwnershipInfo*)sym->ownership_data;

        /* Release any outstanding borrows */
        if (info->borrow_count > 0) {
            CPX_LOG(CPX_LOG_DEBUG, CPX_LOG_CAT_SEMANTIC,                      "Auto-releasing %d borrow(s) on '%s' at scope end (level %d)",
                     info->borrow_count, sym->name, scope_level);
            info->borrow_count = 0;
        }
        if (info->has_mut_borrow) {
            CPX_LOG(CPX_LOG_DEBUG, CPX_LOG_CAT_SEMANTIC,                      "Auto-releasing mutable borrow on '%s' at scope end (level %d)",
                     sym->name, scope_level);
            info->has_mut_borrow = false;
        }

        /* If the value is still OWNED and was never moved, it will be
           dropped by the drop planner.  Nothing to warn about here —
           this is normal RAII.  Reset state for symbol reuse. */
        if (info->state == OWNERSHIP_OWNED || info->state == OWNERSHIP_BORROW ||
            info->state == OWNERSHIP_BORROW_MUT) {
            info->state = OWNERSHIP_OWNED;
        }
    }

    CPX_LOG(CPX_LOG_DEBUG, CPX_LOG_CAT_SEMANTIC, "Validated scope end at level %d", scope_level);
}
