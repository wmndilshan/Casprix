#include "../runtime.h"
#include "../../include/casprix/stdlib.h"
#include "../../include/casprix/thread.h"
#include <stdlib.h>

// ============================================================================
// Thread Implementation
// ============================================================================

#ifdef _WIN32
    static DWORD WINAPI thread_function_wrapper(LPVOID lpParam) {
        nuwan_thread_t* thread = (nuwan_thread_t*)lpParam;
        thread->result = thread->start_routine(thread->arg);
        thread->is_running = false;
        return 0;
    }

    nuwan_thread_t* nuwan_thread_create(void* (*start_routine)(void*), void* arg) {
        if (!start_routine) return NULL;

        nuwan_thread_t* thread = (nuwan_thread_t*)calloc(1, sizeof(nuwan_thread_t));
        if (!thread) return NULL;

        thread->start_routine = start_routine;
        thread->arg = arg;
        thread->is_running = true;

        thread->handle = CreateThread(NULL, 0, thread_function_wrapper, thread, 0, &thread->id);
        if (!thread->handle) {
            free(thread);
            return NULL;
        }

        return thread;
    }

    void nuwan_thread_join(nuwan_thread_t* thread) {
        if (!thread || !thread->handle) return;
        WaitForSingleObject(thread->handle, INFINITE);
        CloseHandle(thread->handle);
        thread->handle = NULL;
    }

    void nuwan_thread_detach(nuwan_thread_t* thread) {
        if (!thread || !thread->handle) return;
        CloseHandle(thread->handle);
        thread->handle = NULL;
    }

    void nuwan_thread_sleep(int milliseconds) {
        Sleep(milliseconds);
    }

    thread_id_t nuwan_thread_current_id() {
        return GetCurrentThreadId();
    }

    void nuwan_thread_yield() {
        Sleep(0);
    }

#else  // POSIX
    static void* thread_function_wrapper(void* arg) {
        nuwan_thread_t* thread = (nuwan_thread_t*)arg;
        thread->result = thread->start_routine(thread->arg);
        thread->is_running = false;
        return thread->result;
    }

    nuwan_thread_t* nuwan_thread_create(void* (*start_routine)(void*), void* arg) {
        if (!start_routine) return NULL;

        nuwan_thread_t* thread = (nuwan_thread_t*)calloc(1, sizeof(nuwan_thread_t));
        if (!thread) return NULL;

        thread->start_routine = start_routine;
        thread->arg = arg;
        thread->is_running = true;

        if (pthread_create(&thread->handle, NULL, thread_function_wrapper, thread) != 0) {
            free(thread);
            return NULL;
        }

        thread->id = thread->handle;
        return thread;
    }

    void nuwan_thread_join(nuwan_thread_t* thread) {
        if (!thread) return;
        pthread_join(thread->handle, NULL);
    }

    void nuwan_thread_detach(nuwan_thread_t* thread) {
        if (!thread) return;
        pthread_detach(thread->handle);
    }

    void nuwan_thread_sleep(int milliseconds) {
        usleep(milliseconds * 1000);
    }

    thread_id_t nuwan_thread_current_id() {
        return pthread_self();
    }

    void nuwan_thread_yield() {
        sched_yield();
    }
#endif

void nuwan_thread_destroy(nuwan_thread_t* thread) {
    if (!thread) return;
    free(thread);
}

// ============================================================================
// Mutex Implementation
// ============================================================================

nuwan_mutex_t* nuwan_mutex_create() {
    nuwan_mutex_t* mutex = (nuwan_mutex_t*)malloc(sizeof(nuwan_mutex_t));
    if (!mutex) return NULL;

    mutex->is_locked = false;

#ifdef _WIN32
    InitializeCriticalSection(&mutex->lock);
#else
    if (pthread_mutex_init(&mutex->lock, NULL) != 0) {
        free(mutex);
        return NULL;
    }
#endif

    return mutex;
}

void nuwan_mutex_lock(nuwan_mutex_t* mutex) {
    if (!mutex) return;

#ifdef _WIN32
    EnterCriticalSection(&mutex->lock);
#else
    pthread_mutex_lock(&mutex->lock);
#endif

    mutex->is_locked = true;
}

void nuwan_mutex_unlock(nuwan_mutex_t* mutex) {
    if (!mutex) return;

#ifdef _WIN32
    LeaveCriticalSection(&mutex->lock);
#else
    pthread_mutex_unlock(&mutex->lock);
#endif

    mutex->is_locked = false;
}

bool nuwan_mutex_trylock(nuwan_mutex_t* mutex) {
    if (!mutex) return false;

#ifdef _WIN32
    if (TryEnterCriticalSection(&mutex->lock)) {
        mutex->is_locked = true;
        return true;
    }
    return false;
#else
    if (pthread_mutex_trylock(&mutex->lock) == 0) {
        mutex->is_locked = true;
        return true;
    }
    return false;
#endif
}

void nuwan_mutex_destroy(nuwan_mutex_t* mutex) {
    if (!mutex) return;

#ifdef _WIN32
    DeleteCriticalSection(&mutex->lock);
#else
    pthread_mutex_destroy(&mutex->lock);
#endif

    free(mutex);
}
