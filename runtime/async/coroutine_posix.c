#include "coroutine.h"
#include <stdlib.h>
#include <stdio.h>

// Minimal stubs for POSIX (Linux/macOS)
// Real implementation would use ucontext.h or assembly

struct Coroutine {
    bool finished;
};

static Coroutine g_main_coro = { .finished = false };

Coroutine* coro_create(CoroutineEntry entry, void* context, size_t stack_size) {
    // Stub: return NULL or a dummy
    return NULL; 
}

void coro_destroy(Coroutine* coro) {
    // Stub
}

bool coro_switch(Coroutine* from, Coroutine* to) {
    // Stub
    return false;
}

void coro_yield(void) {
    // Stub
}

Coroutine* coro_current(void) {
    return &g_main_coro;
}

bool coro_is_finished(const Coroutine* coro) {
    return coro ? coro->finished : true;
}

Coroutine* coro_get_parent(const Coroutine* coro) {
    return NULL;
}

Coroutine* coro_thread_init(void) {
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
