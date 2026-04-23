#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

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

#define NUM_SHARDS 16

typedef struct {
    int32_t id;
    float value;
    char name[64];
} SimulationData;

typedef struct {
    SimulationData* data;
    int32_t count;
    int32_t capacity;
    pthread_mutex_t lock;
} Shard;

static Shard g_shards[NUM_SHARDS];

void simulation_storage_init() {
    for (int i = 0; i < NUM_SHARDS; i++) {
        g_shards[i].capacity = 1024;
        g_shards[i].data = malloc(sizeof(SimulationData) * g_shards[i].capacity);
        g_shards[i].count = 0;
        pthread_mutex_init(&g_shards[i].lock, NULL);
    }
}

static int get_shard_idx(int32_t id) {
    return id % NUM_SHARDS;
}

bool simulation_create(int32_t id, const char* name, float value) {
    int s_idx = get_shard_idx(id);
    Shard* s = &g_shards[s_idx];
    
    pthread_mutex_lock(&s->lock);
    
    // Check if exists
    for (int i = 0; i < s->count; i++) {
        if (s->data[i].id == id) {
            pthread_mutex_unlock(&s->lock);
            return false;
        }
    }
    
    if (s->count >= s->capacity) {
        s->capacity *= 2;
        s->data = realloc(s->data, sizeof(SimulationData) * s->capacity);
    }
    
    s->data[s->count].id = id;
    s->data[s->count].value = value;
    strncpy(s->data[s->count].name, name, 63);
    s->count++;
    
    pthread_mutex_unlock(&s->lock);
    return true;
}

bool simulation_get(int32_t id, char* out_name, float* out_value) {
    int s_idx = get_shard_idx(id);
    Shard* s = &g_shards[s_idx];
    
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < s->count; i++) {
        if (s->data[i].id == id) {
            strcpy(out_name, s->data[i].name);
            *out_value = s->data[i].value;
            pthread_mutex_unlock(&s->lock);
            return true;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return false;
}

bool simulation_update(int32_t id, float delta) {
    int s_idx = get_shard_idx(id);
    Shard* s = &g_shards[s_idx];
    
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < s->count; i++) {
        if (s->data[i].id == id) {
            s->data[i].value += delta;
            pthread_mutex_unlock(&s->lock);
            return true;
        }
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
