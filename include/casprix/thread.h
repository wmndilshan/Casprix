// Casprix Thread Library
// Platform-agnostic threading support

#ifndef NUWAN_THREAD_H
#define NUWAN_THREAD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
    #include <windows.h>
    typedef HANDLE thread_handle_t;
    typedef DWORD thread_id_t;
    typedef CRITICAL_SECTION mutex_t;
#else
    #include <pthread.h>
    typedef pthread_t thread_handle_t;
    typedef pthread_t thread_id_t;
    typedef pthread_mutex_t mutex_t;
#endif

// Thread structure (maps to Casprix Thread class)
typedef struct {
    thread_handle_t handle;
    thread_id_t id;
    bool is_running;
    void* (*start_routine)(void*);
    void* arg;
    void* result;
} nuwan_thread_t;

// Mutex structure (maps to Casprix Mutex class)
typedef struct {
    mutex_t lock;
    bool is_locked;
} nuwan_mutex_t;

// Thread API
nuwan_thread_t* nuwan_thread_create(void* (*start_routine)(void*), void* arg);
void nuwan_thread_join(nuwan_thread_t* thread);
void nuwan_thread_detach(nuwan_thread_t* thread);
void nuwan_thread_sleep(int milliseconds);
thread_id_t nuwan_thread_current_id();
void nuwan_thread_yield();
void nuwan_thread_destroy(nuwan_thread_t* thread);

// Mutex API
nuwan_mutex_t* nuwan_mutex_create();
void nuwan_mutex_lock(nuwan_mutex_t* mutex);
void nuwan_mutex_unlock(nuwan_mutex_t* mutex);
bool nuwan_mutex_trylock(nuwan_mutex_t* mutex);
void nuwan_mutex_destroy(nuwan_mutex_t* mutex);

#endif // NUWAN_THREAD_H
