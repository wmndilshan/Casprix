/**
 * Casperix Compiler - Ownership & Borrowing Checker Implementation
 */

#include "ownership_check.h"
#include "compiler/sema/symtable.h"
#include "support/log.h"
#include "support/error.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ─── Path-sensitive move state ─────────────────────────────────────────────
 *
 * The move bit lives here, in a Symbol*-keyed map, rather than in the
 * per-symbol OwnershipInfo, so branch/loop analysis can fork and merge it.
 */

static OwnMoveEntry* move_find(OwnMoveState* st, void* sym) {
    for (int i = 0; i < st->count; i++) {
        if (st->entries[i].sym == sym) return &st->entries[i];
    }
    return NULL;
}

static void move_set(OwnMoveState* st, void* sym, bool moved, int line) {
    OwnMoveEntry* e = move_find(st, sym);
    if (!e) {
        if (st->count >= st->capacity) {
            int nc = st->capacity < 8 ? 8 : st->capacity * 2;
            st->entries = realloc(st->entries, nc * sizeof(OwnMoveEntry));
            st->capacity = nc;
        }
        e = &st->entries[st->count++];
        e->sym = sym;
    }
    e->moved = moved;
    e->move_line = line;
}

static bool move_is_set(OwnMoveState* st, void* sym) {
    OwnMoveEntry* e = move_find(st, sym);
    return e && e->moved;
}

static int move_line_of(OwnMoveState* st, void* sym) {
    OwnMoveEntry* e = move_find(st, sym);
    return e ? e->move_line : 0;
}

static void state_copy(OwnMoveState* dst, const OwnMoveState* src) {
    dst->count = src->count;
    dst->capacity = src->count > 0 ? src->count : 0;
    dst->entries = NULL;
    if (src->count > 0) {
        dst->entries = malloc(src->count * sizeof(OwnMoveEntry));
        memcpy(dst->entries, src->entries, src->count * sizeof(OwnMoveEntry));
    }
}

void own_state_snapshot(OwnershipChecker* checker, OwnStateSnapshot* out) {
    state_copy(out, &checker->move_state);
}

void own_state_copy(OwnStateSnapshot* out, const OwnStateSnapshot* src) {
    state_copy(out, src);
}

void own_state_restore(OwnershipChecker* checker, const OwnStateSnapshot* snap) {
    free(checker->move_state.entries);
    state_copy(&checker->move_state, snap);
}

void own_state_merge_intersect(OwnStateSnapshot* dst, const OwnStateSnapshot* src) {
    /* Keep an entry moved only if it is moved in `src` too. Anything moved in
     * dst but not moved in src becomes not-moved. */
    for (int i = 0; i < dst->count; i++) {
        if (!dst->entries[i].moved) continue;
        const OwnMoveEntry* s = NULL;
        for (int j = 0; j < src->count; j++) {
            if (src->entries[j].sym == dst->entries[i].sym) { s = &src->entries[j]; break; }
        }
        if (!s || !s->moved) {
            dst->entries[i].moved = false;
        }
    }
}

void own_state_merge_union(OwnStateSnapshot* dst, const OwnStateSnapshot* src) {
    for (int j = 0; j < src->count; j++) {
        if (!src->entries[j].moved) continue;
        OwnMoveEntry* d = NULL;
        for (int i = 0; i < dst->count; i++) {
            if (dst->entries[i].sym == src->entries[j].sym) { d = &dst->entries[i]; break; }
        }
        if (d) {
            if (!d->moved) { d->moved = true; d->move_line = src->entries[j].move_line; }
        } else {
            if (dst->count >= dst->capacity) {
                int nc = dst->capacity < 8 ? 8 : dst->capacity * 2;
                dst->entries = realloc(dst->entries, nc * sizeof(OwnMoveEntry));
                dst->capacity = nc;
            }
            dst->entries[dst->count++] = src->entries[j];
        }
    }
}

bool own_state_equal(const OwnStateSnapshot* a, const OwnStateSnapshot* b) {
    /* Compare only the set of symbols that are `moved` in each. */
    for (int i = 0; i < a->count; i++) {
        if (!a->entries[i].moved) continue;
        bool found = false;
        for (int j = 0; j < b->count; j++) {
            if (b->entries[j].sym == a->entries[i].sym && b->entries[j].moved) { found = true; break; }
        }
        if (!found) return false;
    }
    for (int j = 0; j < b->count; j++) {
        if (!b->entries[j].moved) continue;
        bool found = false;
        for (int i = 0; i < a->count; i++) {
            if (a->entries[i].sym == b->entries[j].sym && a->entries[i].moved) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

void own_state_free(OwnStateSnapshot* snap) {
    if (!snap) return;
    free(snap->entries);
    snap->entries = NULL;
    snap->count = snap->capacity = 0;
}

void own_state_forget_scope(OwnershipChecker* checker, int scope_level) {
    OwnMoveState* st = &checker->move_state;
    int w = 0;
    for (int i = 0; i < st->count; i++) {
        Symbol* s = (Symbol*)st->entries[i].sym;
        if (s && s->scope_depth == scope_level) continue; /* drop */
        st->entries[w++] = st->entries[i];
    }
    st->count = w;
}

void ownership_reset_function(OwnershipChecker* checker) {
    if (!checker) return;
    free(checker->move_state.entries);
    checker->move_state.entries = NULL;
    checker->move_state.count = 0;
    checker->move_state.capacity = 0;
    checker->suppress_diagnostics = false;
}

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
        info->is_linear_view = false;
        info->parent_string  = NULL;
        info->consume_count  = 0;
    }
    return (OwnershipInfo*)sym->ownership_data;
}

void register_linear_view(OwnershipChecker* checker,
                          const char* view_name,
                          const char* parent_string_name,
                          int line) {
    if (!checker || !view_name) return;

    Symbol* sym = lookup_symbol(checker->analyzer->symbols, view_name);
    if (!sym) return;

    OwnershipInfo* info = get_ownership_info(sym);
    info->is_linear_view = true;
    info->parent_string  = parent_string_name;   /* borrowed; AST-lived  */
    info->consume_count  = 0;
    info->state          = OWNERSHIP_OWNED;

    if (!parent_string_name) {
        /* Unknown parent — the drop planner cannot prove safety and will
         * fire its own diagnostic when the surrounding scope exits.  We
         * emit a dedicated, earlier note here so the user sees both the
         * declaration site and the eventual drop-site diagnostic. */
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "StringView '%s' has no inferable parent String; "
                 "the linear-view checker cannot prove its safety",
                 view_name);
        report_semantic_error(line, 0, msg);
        return;
    }

    CPX_LOG(CPX_LOG_DEBUG, CPX_LOG_CAT_SEMANTIC,
            "Registered linear view '%s' borrowing from '%s' at line %d",
            view_name, parent_string_name, line);
}

void linear_view_consume(OwnershipChecker* checker,
                         const char* var_name,
                         int line) {
    if (!checker || !var_name) return;
    if (checker->in_unsafe_block) return;

    Symbol* sym = lookup_symbol(checker->analyzer->symbols, var_name);
    if (!sym || !sym->ownership_data) return;

    OwnershipInfo* info = (OwnershipInfo*)sym->ownership_data;
    if (!info->is_linear_view) return;       /* not a view — nothing to do */

    info->consume_count++;

    if (info->consume_count > 1 || move_is_set(&checker->move_state, sym)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Use of consumed StringView '%s' (linear values may be "
                 "consumed at most once; previously consumed at line %d)",
                 var_name, info->move_location);
        report_semantic_error(line, 0, msg);
        return;
    }

    /* First consume → record the move on the current path so the regular
     * ownership checker forbids any subsequent read through
     * check_ownership_valid. */
    move_set(&checker->move_state, sym, true, line);
    info->move_location  = line;
}

void ownership_checker_init(OwnershipChecker* checker, SemanticAnalyzer* analyzer) {
    checker->analyzer = analyzer;
    checker->current_scope = 0;
    checker->in_unsafe_block = false;
    checker->move_state.entries = NULL;
    checker->move_state.count = 0;
    checker->move_state.capacity = 0;
    checker->suppress_diagnostics = false;
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

    if (move_is_set(&checker->move_state, sym)) {
        if (!checker->suppress_diagnostics) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Use of moved value '%s' (moved at line %d)",
                     var_name, move_line_of(&checker->move_state, sym));
            report_semantic_error(line, 0, msg);
        }
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
        if (!checker->suppress_diagnostics) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Cannot move '%s' while borrowed", var_name);
            report_semantic_error(line, 0, msg);
        }
        return;
    }

    /* A second move on the same path is a use-after-move; the read of the
     * variable inside the `move` expression is already reported by
     * check_ownership_valid, so just record the (idempotent) move here. */
    move_set(&checker->move_state, sym, true, line);
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
        char msg[256];
        snprintf(msg, sizeof(msg), "Cannot borrow '%s' immutably while mutably borrowed", var_name);
        report_semantic_error(line, 0, msg);
        return false;
    }

    // Can't borrow if moved
    if (move_is_set(&checker->move_state, sym)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Cannot borrow moved value '%s'", var_name);
        report_semantic_error(line, 0, msg);
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
        char msg[256];
        snprintf(msg, sizeof(msg), "Cannot borrow '%s' mutably while borrowed", var_name);
        report_semantic_error(line, 0, msg);
        return false;
    }

    // Can't borrow if moved
    if (move_is_set(&checker->move_state, sym)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Cannot borrow moved value '%s'", var_name);
        report_semantic_error(line, 0, msg);
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

    /* Drop path-sensitive move entries for the symbols leaving scope — their
     * Symbol* is about to be freed by exit_scope(). */
    own_state_forget_scope(checker, scope_level);

    CPX_LOG(CPX_LOG_DEBUG, CPX_LOG_CAT_SEMANTIC, "Validated scope end at level %d", scope_level);
}
