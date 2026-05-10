#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include "../../include/casprix/collections.h"
#include "../../src/support/arena.h"

/* ============================================================
 * Atomic Primitives
 * ============================================================ */

int64_t atomic_int_create(int64_t initial) {
    atomic_int_fast64_t* a = malloc(sizeof(atomic_int_fast64_t));
    atomic_init(a, initial);
    return (intptr_t)a;
}

void atomic_int_destroy(intptr_t handle) {
    free((void*)handle);
}

int64_t atomic_int_add(intptr_t handle, int64_t val) {
    return atomic_fetch_add((atomic_int_fast64_t*)handle, val);
}

int64_t atomic_int_load(intptr_t handle) {
    return atomic_load((atomic_int_fast64_t*)handle);
}

void atomic_int_store(intptr_t handle, int64_t val) {
    atomic_store((atomic_int_fast64_t*)handle, val);
}

/* ============================================================
 * Mutex Primitives
 * ============================================================ */

intptr_t mutex_create() {
    pthread_mutex_t* m = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(m, NULL);
    return (intptr_t)m;
}

void mutex_destroy(intptr_t handle) {
    pthread_mutex_t* m = (pthread_mutex_t*)handle;
    pthread_mutex_destroy(m);
    free(m);
}

void mutex_lock(intptr_t handle) {
    pthread_mutex_lock((pthread_mutex_t*)handle);
}

void mutex_unlock(intptr_t handle) {
    pthread_mutex_unlock((pthread_mutex_t*)handle);
}

/* ============================================================
 * Simulation State Storage (Simple Sharded Map)
 * ============================================================ */

typedef struct {
    int32_t id;
    float value;
    char name[64];
} SimulationData;

typedef struct {
    NuwanSwissMap* map;
    pthread_mutex_t lock;
} Shard;

static Shard g_shards[NUM_SHARDS];
static Arena* g_sim_arena = NULL;

void simulation_storage_init() {
    if (g_sim_arena == NULL) {
        g_sim_arena = arena_create();
    }
    for (int i = 0; i < NUM_SHARDS; i++) {
        g_shards[i].map = nuwan_swiss_new(g_sim_arena);
        // Pre-reserve for "thousands" of requests
        nuwan_swiss_reserve(g_shards[i].map, 1024);
        pthread_mutex_init(&g_shards[i].lock, NULL);
    }
}

static int get_shard_idx(int32_t id) {
    return id % NUM_SHARDS;
}

bool simulation_create(int32_t id, const char* name, float value) {
    char key[16];
    snprintf(key, 16, "%d", id);
    
    int s_idx = get_shard_idx(id);
    Shard* s = &g_shards[s_idx];
    
    pthread_mutex_lock(&s->lock);
    
    if (nuwan_swiss_has(s->map, key)) {
        pthread_mutex_unlock(&s->lock);
        return false;
    }
    
    // Store data. In a real system we'd store a pointer to a struct.
    // Here we'll pack the float value into the int64_t for simplicity in this demo,
    // or we could use the Arena to allocate a struct.
    SimulationData* d = arena_alloc(g_sim_arena, sizeof(SimulationData));
    d->id = id;
    d->value = value;
    strncpy(d->name, name, 63);
    
    nuwan_swiss_put(s->map, key, (intptr_t)d);
    
    pthread_mutex_unlock(&s->lock);
    return true;
}

bool simulation_get(int32_t id, char* out_name, float* out_value) {
    char key[16];
    snprintf(key, 16, "%d", id);
    
    int s_idx = get_shard_idx(id);
    Shard* s = &g_shards[s_idx];
    
    pthread_mutex_lock(&s->lock);
    intptr_t ptr;
    if (nuwan_swiss_get_opt(s->map, key, &ptr)) {
        SimulationData* d = (SimulationData*)ptr;
        strcpy(out_name, d->name);
        *out_value = d->value;
        pthread_mutex_unlock(&s->lock);
        return true;
    }
    pthread_mutex_unlock(&s->lock);
    return false;
}

bool simulation_update(int32_t id, float delta) {
    char key[16];
    snprintf(key, 16, "%d", id);
    
    int s_idx = get_shard_idx(id);
    Shard* s = &g_shards[s_idx];
    
    pthread_mutex_lock(&s->lock);
    intptr_t ptr;
    if (nuwan_swiss_get_opt(s->map, key, &ptr)) {
        SimulationData* d = (SimulationData*)ptr;
        d->value += delta;
        pthread_mutex_unlock(&s->lock);
        return true;
    }
    pthread_mutex_unlock(&s->lock);
    return false;
}

/* ============================================================
 * FFI Pointer Helpers
 * ============================================================ */

void* ptr_create_f32() {
    return malloc(sizeof(float));
}

float ptr_get_f32(void* p) {
    float val = *(float*)p;
    free(p);
    return val;
}

void* ptr_create_buf(int32_t size) {
    return malloc(size);
}

const char* ptr_to_string(void* p) {
    return (const char*)p;
}
