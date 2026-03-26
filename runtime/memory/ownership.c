// Casperix Runtime - Ownership & Borrow Checker Implementation
// Debug-mode runtime borrow checking

#include "ownership.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef CASPERIX_DEBUG_BORROWS

static BorrowRecord g_borrows[MAX_BORROW_RECORDS];
static int g_borrow_count = 0;
static bool g_borrow_checker_initialized = false;

void borrow_checker_init(void) {
    memset(g_borrows, 0, sizeof(g_borrows));
    g_borrow_count = 0;
    g_borrow_checker_initialized = true;
}

void borrow_checker_shutdown(void) {
    if (g_borrow_count > 0) {
        fprintf(stderr, "BORROW CHECK WARNING: %d borrows still active at shutdown:\n",
                g_borrow_count);
        for (int i = 0; i < g_borrow_count; i++) {
            fprintf(stderr, "  - %s%s at %s:%d (addr=%p)\n",
                    g_borrows[i].is_mutable ? "&mut " : "&",
                    g_borrows[i].borrower,
                    g_borrows[i].file,
                    g_borrows[i].line,
                    g_borrows[i].address);
        }
    }
    g_borrow_checker_initialized = false;
}

void borrow_check_borrow(const void* addr, const char* name,
                         const char* file, int line) {
    if (!g_borrow_checker_initialized) return;

    // Check for existing mutable borrow
    borrow_check_assert_no_mut(addr, file, line);

    if (g_borrow_count >= MAX_BORROW_RECORDS) {
        fprintf(stderr, "BORROW CHECK: too many active borrows (max %d)\n",
                MAX_BORROW_RECORDS);
        return;
    }

    BorrowRecord* rec = &g_borrows[g_borrow_count++];
    rec->address = addr;
    rec->borrower = name;
    rec->file = file;
    rec->line = line;
    rec->is_mutable = false;
}

void borrow_check_borrow_mut(const void* addr, const char* name,
                             const char* file, int line) {
    if (!g_borrow_checker_initialized) return;

    // Check for any existing borrows
    borrow_check_assert_no_borrows(addr, file, line);

    if (g_borrow_count >= MAX_BORROW_RECORDS) {
        fprintf(stderr, "BORROW CHECK: too many active borrows (max %d)\n",
                MAX_BORROW_RECORDS);
        return;
    }

    BorrowRecord* rec = &g_borrows[g_borrow_count++];
    rec->address = addr;
    rec->borrower = name;
    rec->file = file;
    rec->line = line;
    rec->is_mutable = true;
}

void borrow_check_release(const void* addr, const char* name) {
    if (!g_borrow_checker_initialized) return;

    for (int i = 0; i < g_borrow_count; i++) {
        if (g_borrows[i].address == addr &&
            strcmp(g_borrows[i].borrower, name) == 0) {
            // Shift remaining records
            for (int j = i; j < g_borrow_count - 1; j++) {
                g_borrows[j] = g_borrows[j + 1];
            }
            g_borrow_count--;
            return;
        }
    }
}

void borrow_check_assert_no_mut(const void* addr, const char* file, int line) {
    if (!g_borrow_checker_initialized) return;

    for (int i = 0; i < g_borrow_count; i++) {
        if (g_borrows[i].address == addr && g_borrows[i].is_mutable) {
            fprintf(stderr,
                    "BORROW CHECK ERROR at %s:%d: "
                    "cannot borrow - already mutably borrowed by '%s' at %s:%d\n",
                    file, line,
                    g_borrows[i].borrower,
                    g_borrows[i].file,
                    g_borrows[i].line);
            abort();
        }
    }
}

void borrow_check_assert_no_borrows(const void* addr, const char* file, int line) {
    if (!g_borrow_checker_initialized) return;

    for (int i = 0; i < g_borrow_count; i++) {
        if (g_borrows[i].address == addr) {
            fprintf(stderr,
                    "BORROW CHECK ERROR at %s:%d: "
                    "cannot mutably borrow - already %sborrowed by '%s' at %s:%d\n",
                    file, line,
                    g_borrows[i].is_mutable ? "mutably " : "",
                    g_borrows[i].borrower,
                    g_borrows[i].file,
                    g_borrows[i].line);
            abort();
        }
    }
}

void borrow_check_assert_not_moved(const void* addr, const char* name,
                                   const char* file, int line) {
    if (!g_borrow_checker_initialized) return;

    if (addr == NULL) {
        fprintf(stderr,
                "BORROW CHECK ERROR at %s:%d: "
                "use of moved value '%s'\n",
                file, line, name);
        abort();
    }
}

#endif // CASPERIX_DEBUG_BORROWS

/* ── Non-inline scope_guard_drop for use by generated code ── */

void scope_guard_drop_extern(void* guard_ptr) {
    ScopeGuard* guard = (ScopeGuard*)guard_ptr;
    if (guard && guard->value && guard->drop) {
        guard->drop(guard->value);
        guard->value = NULL;
    }
}
