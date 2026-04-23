#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

/* --- Atomic Int --- */

typedef struct {
    atomic_int_fast64_t val;
} CpxAtomicInt;

void* cpx_atomic_int_create(int64_t initial) {
    CpxAtomicInt* a = malloc(sizeof(CpxAtomicInt));
    atomic_init(&a->val, initial);
    return a;
}

void cpx_atomic_int_destroy(void* handle) {
    free(handle);
}

int64_t cpx_atomic_int_add(void* handle, int64_t val) {
    return atomic_fetch_add(&((CpxAtomicInt*)handle)->val, val);
}

int64_t cpx_atomic_int_load(void* handle) {
    return atomic_load(&((CpxAtomicInt*)handle)->val);
}

void cpx_atomic_int_store(void* handle, int64_t val) {
    atomic_store(&((CpxAtomicInt*)handle)->val, val);
}

/* --- Mutex --- */

void* cpx_mutex_create() {
    pthread_mutex_t* m = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(m, NULL);
    return m;
}

void cpx_mutex_destroy(void* handle) {
    pthread_mutex_t* m = (pthread_mutex_t*)handle;
    pthread_mutex_destroy(m);
    free(m);
}

void cpx_mutex_lock(void* handle) {
    pthread_mutex_lock((pthread_mutex_t*)handle);
}

void cpx_mutex_unlock(void* handle) {
    pthread_mutex_unlock((pthread_mutex_t*)handle);
}

/* --- Utilities --- */

void* ptr_create_i32() {
    return calloc(1, sizeof(int32_t));
}

void ptr_free(void* p) {
    free(p);
}
