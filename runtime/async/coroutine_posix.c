#include "coroutine.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Assembly hooks
extern void coro_context_switch(void** from_sp, void* to_sp);
extern void* coro_context_init(void* stack_top, void* entry, void* context);

struct Coroutine {
    void* sp;           // Saved stack pointer
    void* stack_base;   // Allocated stack memory
    size_t stack_size;
    bool finished;
    Coroutine* parent;
    CoroutineEntry entry;
    void* context;
};

static Coroutine g_main_coro = { .finished = false };

static void coro_wrapper(void* context) {
    Coroutine* current = coro_current();
    current->entry(current->context);
    current->finished = true;
    coro_yield();
}

Coroutine* coro_create(CoroutineEntry entry, void* context, size_t stack_size) {
    if (stack_size == 0) stack_size = CORO_DEFAULT_STACK_SIZE;
    
    Coroutine* coro = malloc(sizeof(Coroutine));
    coro->stack_base = malloc(stack_size);
    coro->stack_size = stack_size;
    coro->finished = false;
    coro->entry = entry;
    coro->context = context;
    coro->parent = coro_current();
    
    // x86-64 stacks grow downwards
    void* stack_top = (char*)coro->stack_base + stack_size;

    // Prepare the initial context on the new stack.
    // coro_context_switch pops 6 callee-saved slots (48 bytes) then `ret`s (8),
    // consuming 56 bytes total before coro_wrapper's first instruction runs.
    // The SysV x86-64 ABI requires rsp ≡ 8 (mod 16) at a function's entry (the
    // state right after a `call`). malloc()'d stack_base and stack_size are
    // both 16-aligned, so we reserve one extra 8-byte gap here: 56 + 8 = 64,
    // leaving entry rsp = stack_top - 8 ≡ 8 (mod 16). Without this, the compiler's
    // aligned SSE/AVX stores (vmovdqa) in the entry function fault.
    void** sp = (void**)stack_top;
    --sp; // 16-byte alignment gap (see above)

    *(--sp) = (void*)coro_wrapper; // Return address for the first switch-in
    
    // Push callee-saved registers (dummy values for rbp, rbx, r12, r13, r14, r15)
    for (int i = 0; i < 6; i++) {
        *(--sp) = 0;
    }
    
    coro->sp = sp;
    return coro;
}

void coro_destroy(Coroutine* coro) {
    if (!coro || coro == &g_main_coro) return;
    free(coro->stack_base);
    free(coro);
}

static _Thread_local Coroutine* g_current_coro = NULL;

bool coro_switch(Coroutine* from, Coroutine* to) {
    if (!from || !to || from == to) return false;
    
    g_current_coro = to;
    coro_context_switch(&from->sp, to->sp);
    return true;
}

void coro_yield(void) {
    Coroutine* current = coro_current();
    if (current && current->parent) {
        coro_switch(current, current->parent);
    }
}

Coroutine* coro_current(void) {
    if (!g_current_coro) g_current_coro = &g_main_coro;
    return g_current_coro;
}

bool coro_is_finished(const Coroutine* coro) {
    return coro ? coro->finished : true;
}

Coroutine* coro_get_parent(const Coroutine* coro) {
    return coro ? coro->parent : NULL;
}

Coroutine* coro_thread_init(void) {
    g_current_coro = &g_main_coro;
    return &g_main_coro;
}

void coro_thread_cleanup(void) {
    // Stub
}

Coroutine* coro_get_main(void) {
    return &g_main_coro;
}

CoroutineStats coro_get_stats(void) {
    CoroutineStats stats = {0};
    return stats;
}

void coro_print_stats(void) {
    printf("Coroutine stats not supported on this platform\n");
}
