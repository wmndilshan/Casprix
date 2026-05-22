#define _GNU_SOURCE
#include "../async/coroutine.h"
#include <ucontext.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#ifndef MAP_STACK
#define MAP_STACK 0x20000
#endif

struct Coroutine {
    ucontext_t ctx;
    void* stack_base;   // Base of the mmap'd region
    size_t stack_size;  // Total size of the mmap'd region
    bool finished;
    struct Coroutine* parent;
    CoroutineEntry entry;
    void* context;
};

static __thread Coroutine* tls_current_coro = NULL;
static __thread Coroutine* tls_main_coro = NULL;
static __thread Coroutine  tls_main_coro_storage;

static _Atomic size_t g_total_created = 0;
static _Atomic size_t g_total_destroyed = 0;
static _Atomic size_t g_current_alive = 0;
static _Atomic size_t g_total_switches = 0;
static _Atomic size_t g_total_stack_bytes = 0;

static void coro_trampoline(void) {
    Coroutine* current = tls_current_coro;
    if (current && current->entry) {
        current->entry(current->context);
    }
    
    current->finished = true;
    
    // Manual switch back to parent since uc_link is NULL
    if (current->parent) {
        coro_switch(current, current->parent);
    }
    
    // Should never reach here
    fprintf(stderr, "Error: coroutine trampoline returned after switch\n");
    abort();
}

Coroutine* coro_create(CoroutineEntry entry, void* context, size_t stack_size) {
    if (stack_size == 0) stack_size = CORO_DEFAULT_STACK_SIZE;
    if (stack_size < CORO_MIN_STACK_SIZE) stack_size = CORO_MIN_STACK_SIZE;
    
    // Align stack size to page size
    size_t page_size = sysconf(_SC_PAGESIZE);
    stack_size = (stack_size + page_size - 1) & ~(page_size - 1);
    
    // Total allocation: guard page + stack_size
    size_t total_size = stack_size + page_size;
    
    void* stack_base = mmap(NULL, total_size, PROT_READ | PROT_WRITE, 
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack_base == MAP_FAILED) {
        return NULL;
    }
    
    // Install guard page at the bottom (lowest address)
    if (mprotect(stack_base, page_size, PROT_NONE) == -1) {
        munmap(stack_base, total_size);
        return NULL;
    }
    
    Coroutine* coro = (Coroutine*)calloc(1, sizeof(Coroutine));
    if (!coro) {
        munmap(stack_base, total_size);
        return NULL;
    }
    
    coro->stack_base = stack_base;
    coro->stack_size = total_size;
    coro->entry = entry;
    coro->context = context;
    coro->parent = coro_current();
    
    if (getcontext(&coro->ctx) == -1) {
        free(coro);
        munmap(stack_base, total_size);
        return NULL;
    }
    
    coro->ctx.uc_stack.ss_sp = (char*)stack_base + page_size;
    coro->ctx.uc_stack.ss_size = stack_size;
    coro->ctx.uc_stack.ss_flags = 0;
    coro->ctx.uc_link = NULL;
    
    makecontext(&coro->ctx, (void (*)(void))coro_trampoline, 0);
    
    atomic_fetch_add_explicit(&g_total_created, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_current_alive, 1, memory_order_seq_cst);
    atomic_fetch_add_explicit(&g_total_stack_bytes, total_size, memory_order_relaxed);
    
    return coro;
}

void coro_destroy(Coroutine* coro) {
    if (!coro || coro == tls_main_coro) return;
    
    if (coro->stack_base) {
        munmap(coro->stack_base, coro->stack_size);
    }
    
    atomic_fetch_add_explicit(&g_total_destroyed, 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&g_current_alive, 1, memory_order_seq_cst);
    
    free(coro);
}

bool coro_switch(Coroutine* from, Coroutine* to) {
    if (!from || !to || from == to) return false;
    
    tls_current_coro = to;
    atomic_fetch_add_explicit(&g_total_switches, 1, memory_order_relaxed);
    
    if (swapcontext(&from->ctx, &to->ctx) == -1) {
        // This is bad.
        return false;
    }
    
    return true;
}

void coro_yield(void) {
    Coroutine* current = tls_current_coro;
    if (current && current->parent) {
        coro_switch(current, current->parent);
    }
}

Coroutine* coro_current(void) {
    if (!tls_current_coro) {
        return coro_thread_init();
    }
    return tls_current_coro;
}

bool coro_is_finished(const Coroutine* coro) {
    return coro ? coro->finished : true;
}

Coroutine* coro_get_parent(const Coroutine* coro) {
    return coro ? coro->parent : NULL;
}

Coroutine* coro_thread_init(void) {
    if (tls_main_coro) return tls_main_coro;
    
    tls_main_coro = &tls_main_coro_storage;
    tls_main_coro->finished = false;
    tls_main_coro->stack_base = NULL;
    tls_main_coro->stack_size = 0;
    tls_main_coro->parent = NULL;
    tls_main_coro->entry = NULL;
    tls_main_coro->context = NULL;
    
    if (getcontext(&tls_main_coro->ctx) == -1) {
        // Should we abort?
        return NULL;
    }
    
    tls_current_coro = tls_main_coro;
    return tls_main_coro;
}

void coro_thread_cleanup(void) {
    tls_main_coro = NULL;
    tls_current_coro = NULL;
}

Coroutine* coro_get_main(void) {
    return tls_main_coro;
}

CoroutineStats coro_get_stats(void) {
    CoroutineStats stats;
    stats.total_created = atomic_load_explicit(&g_total_created, memory_order_relaxed);
    stats.total_destroyed = atomic_load_explicit(&g_total_destroyed, memory_order_relaxed);
    stats.current_alive = atomic_load_explicit(&g_current_alive, memory_order_seq_cst);
    stats.total_switches = atomic_load_explicit(&g_total_switches, memory_order_relaxed);
    stats.total_stack_bytes = atomic_load_explicit(&g_total_stack_bytes, memory_order_relaxed);
    return stats;
}

void coro_print_stats(void) {
    CoroutineStats stats = coro_get_stats();
    printf("Coroutine Statistics:\n");
    printf("  Total Created:     %zu\n", stats.total_created);
    printf("  Total Destroyed:   %zu\n", stats.total_destroyed);
    printf("  Current Alive:     %zu\n", stats.current_alive);
    printf("  Total Switches:    %zu\n", stats.total_switches);
    printf("  Total Stack Bytes: %zu\n", stats.total_stack_bytes);
}
