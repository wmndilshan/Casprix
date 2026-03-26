/**
 * Casperix Compiler - Drop Planner Implementation
 *
 * Tracks variable lifetimes and determines cleanup actions at scope exit.
 */

#include "drop_planner.h"
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* ─── Lifecycle ─── */

void drop_planner_init(DropPlanner* dp) {
    memset(dp, 0, sizeof(DropPlanner));
    dp->current_scope = 0;
    dp->scope_start[0] = 0;
}

void drop_planner_reset(DropPlanner* dp) {
    dp->count         = 0;
    dp->current_scope = 0;
    dp->scope_start[0] = 0;
}

/* ─── Scope management ─── */

void drop_planner_enter_scope(DropPlanner* dp) {
    dp->current_scope++;
    assert(dp->current_scope < MAX_DROP_SCOPES && "Drop planner: scope nesting overflow");
    dp->scope_start[dp->current_scope] = dp->count;
}

void drop_planner_exit_scope(DropPlanner* dp) {
    assert(dp->current_scope > 0 && "Drop planner: scope underflow");

    /* Remove entries from the exiting scope
       (they are conceptually destroyed — the codegen already emitted drops) */
    dp->count = dp->scope_start[dp->current_scope];
    dp->current_scope--;
}

/* ─── Registration ─── */

void drop_planner_register(DropPlanner* dp, const char* var_name,
                            int stack_offset, DropKind kind,
                            const char* dtor_name,
                            bool is_param) {
    assert(dp->count < MAX_DROP_ENTRIES && "Drop planner: too many entries");

    DropEntry* e   = &dp->entries[dp->count++];
    e->var_name    = var_name;
    e->scope_level = dp->current_scope;
    e->stack_offset = stack_offset;
    e->kind        = kind;
    e->dtor_name   = dtor_name;
    e->is_moved    = false;
    e->is_borrowed = false;
    e->is_param    = is_param;
}

/* ─── Mutation ─── */

static DropEntry* find_entry(DropPlanner* dp, const char* var_name) {
    /* Search backwards for most recent declaration */
    for (int i = dp->count - 1; i >= 0; i--) {
        if (dp->entries[i].var_name &&
            strcmp(dp->entries[i].var_name, var_name) == 0) {
            return &dp->entries[i];
        }
    }
    return NULL;
}

void drop_planner_mark_moved(DropPlanner* dp, const char* var_name) {
    DropEntry* e = find_entry(dp, var_name);
    if (e) e->is_moved = true;
}

void drop_planner_mark_borrowed(DropPlanner* dp, const char* var_name) {
    DropEntry* e = find_entry(dp, var_name);
    if (e) e->is_borrowed = true;
}

/* ─── Drop queries ─── */

/* Internal buffer for reverse-order results */
static DropEntry g_drop_result[MAX_DROP_ENTRIES];
static int       g_drop_result_count = 0;

const DropEntry* drop_planner_get_scope_drops(DropPlanner* dp, int* count) {
    int start = dp->scope_start[dp->current_scope];
    int end   = dp->count;

    g_drop_result_count = 0;

    /* Walk in reverse order (LIFO — last declared is dropped first) */
    for (int i = end - 1; i >= start; i--) {
        DropEntry* e = &dp->entries[i];

        /* Skip entries that should not be dropped */
        if (e->kind == DROP_NONE)   continue;
        if (e->kind == DROP_REGION) continue;  /* Region handles bulk free */
        if (e->is_moved)            continue;  /* Ownership transferred   */
        if (e->is_borrowed)         continue;  /* Not the owner           */

        g_drop_result[g_drop_result_count++] = *e;
    }

    if (count) *count = g_drop_result_count;
    return g_drop_result;
}

const DropEntry* drop_planner_get_drops_to_scope(DropPlanner* dp,
                                                   int target_scope,
                                                   int* count) {
    g_drop_result_count = 0;

    /* Walk all entries from current back to target scope (for early return) */
    for (int i = dp->count - 1; i >= 0; i--) {
        DropEntry* e = &dp->entries[i];

        if (e->scope_level < target_scope) break;

        if (e->kind == DROP_NONE)   continue;
        if (e->kind == DROP_REGION) continue;
        if (e->is_moved)            continue;
        if (e->is_borrowed)         continue;

        g_drop_result[g_drop_result_count++] = *e;
    }

    if (count) *count = g_drop_result_count;
    return g_drop_result;
}

/* ─── Debug ─── */

static const char* drop_kind_str(DropKind k) {
    switch (k) {
    case DROP_NONE:        return "NONE";
    case DROP_ARC:         return "ARC";
    case DROP_RC:          return "RC";
    case DROP_DTOR:        return "DTOR";
    case DROP_REGION:      return "REGION";
    case DROP_SCOPE_GUARD: return "GUARD";
    default:               return "?";
    }
}

void drop_planner_print(DropPlanner* dp) {
    printf("=== Drop Planner (scope=%d, entries=%d) ===\n",
           dp->current_scope, dp->count);
    printf("%-16s %-6s %-8s %-8s %-6s %-6s %s\n",
           "Variable", "Scope", "Kind", "Offset", "Moved", "Borrow", "Dtor");
    printf("--------------------------------------------------------------\n");

    for (int i = 0; i < dp->count; i++) {
        DropEntry* e = &dp->entries[i];
        printf("%-16s %-6d %-8s %-8d %-6s %-6s %s\n",
               e->var_name ? e->var_name : "<?>",
               e->scope_level,
               drop_kind_str(e->kind),
               e->stack_offset,
               e->is_moved ? "YES" : "no",
               e->is_borrowed ? "YES" : "no",
               e->dtor_name ? e->dtor_name : "-");
    }
    printf("==============================================================\n");
}
