/**
 * Casperix Runtime - Future Implementation
 */

#include "future.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

Future* future_create(void) {
    Future* future = (Future*)calloc(1, sizeof(Future));
    if (!future) return NULL;
    
    future->state = FUTURE_PENDING;
    future->result = NULL;
    future->error = NULL;
    future->callbacks = NULL;
    future->callback_data = NULL;
    future->callback_count = 0;
    future->callback_capacity = 0;
    
    // Create mutex and condition variable
#ifdef _WIN32
    future->mutex = CreateMutex(NULL, FALSE, NULL);
    future->cond = CreateEvent(NULL, TRUE, FALSE, NULL);
#else
    pthread_mutex_t* mutex = malloc(sizeof(pthread_mutex_t));
    pthread_cond_t* cond = malloc(sizeof(pthread_cond_t));
    pthread_mutex_init(mutex, NULL);
    pthread_cond_init(cond, NULL);
    future->mutex = mutex;
    future->cond = cond;
#endif
    
    return future;
}

void future_complete(Future* future, void* result) {
    if (!future || future->state != FUTURE_PENDING) {
        return;
    }
    
#ifdef _WIN32
    WaitForSingleObject(future->mutex, INFINITE);
#else
    pthread_mutex_lock((pthread_mutex_t*)future->mutex);
#endif
    
    future->state = FUTURE_COMPLETED;
    future->result = result;
    
    // Call all callbacks
    for (int i = 0; i < future->callback_count; i++) {
        if (future->callbacks[i]) {
            future->callbacks[i](result, future->callback_data[i]);
        }
    }
    
#ifdef _WIN32
    SetEvent(future->cond);
    ReleaseMutex(future->mutex);
#else
    pthread_cond_broadcast((pthread_cond_t*)future->cond);
    pthread_mutex_unlock((pthread_mutex_t*)future->mutex);
#endif
}

void future_fail(Future* future, void* error) {
    if (!future || future->state != FUTURE_PENDING) {
        return;
    }
    
#ifdef _WIN32
    WaitForSingleObject(future->mutex, INFINITE);
#else
    pthread_mutex_lock((pthread_mutex_t*)future->mutex);
#endif
    
    future->state = FUTURE_FAILED;
    future->error = error;
    
#ifdef _WIN32
    SetEvent(future->cond);
    ReleaseMutex(future->mutex);
#else
    pthread_cond_broadcast((pthread_cond_t*)future->cond);
    pthread_mutex_unlock((pthread_mutex_t*)future->mutex);
#endif
}

void future_cancel(Future* future) {
    if (!future || future->state != FUTURE_PENDING) {
        return;
    }
    
    future->state = FUTURE_CANCELLED;
    
#ifdef _WIN32
    SetEvent(future->cond);
#else
    pthread_cond_broadcast((pthread_cond_t*)future->cond);
#endif
}

void future_then(Future* future, FutureCallback callback, void* user_data) {
    if (!future || !callback) return;
    
#ifdef _WIN32
    WaitForSingleObject(future->mutex, INFINITE);
#else
    pthread_mutex_lock((pthread_mutex_t*)future->mutex);
#endif
    
    // If already complete, call immediately
    if (future->state == FUTURE_COMPLETED) {
        callback(future->result, user_data);
#ifdef _WIN32
        ReleaseMutex(future->mutex);
#else
        pthread_mutex_unlock((pthread_mutex_t*)future->mutex);
#endif
        return;
    }
    
    // Add to callback list
    if (future->callback_count >= future->callback_capacity) {
        int new_capacity = future->callback_capacity == 0 ? 4 : future->callback_capacity * 2;
        future->callbacks = realloc(future->callbacks, sizeof(FutureCallback) * new_capacity);
        future->callback_data = realloc(future->callback_data, sizeof(void*) * new_capacity);
        future->callback_capacity = new_capacity;
    }
    
    future->callbacks[future->callback_count] = callback;
    future->callback_data[future->callback_count] = user_data;
    future->callback_count++;
    
#ifdef _WIN32
    ReleaseMutex(future->mutex);
#else
    pthread_mutex_unlock((pthread_mutex_t*)future->mutex);
#endif
}

void* future_wait(Future* future) {
    if (!future) return NULL;
    
#ifdef _WIN32
    WaitForSingleObject(future->cond, INFINITE);
#else
    pthread_mutex_lock((pthread_mutex_t*)future->mutex);
    while (future->state == FUTURE_PENDING) {
        pthread_cond_wait((pthread_cond_t*)future->cond, (pthread_mutex_t*)future->mutex);
    }
    pthread_mutex_unlock((pthread_mutex_t*)future->mutex);
#endif
    
    return future->result;
}

bool future_is_ready(const Future* future) {
    return future && future->state != FUTURE_PENDING;
}

void future_destroy(Future* future) {
    if (!future) return;
    
    free(future->callbacks);
    free(future->callback_data);
    
#ifdef _WIN32
    CloseHandle(future->mutex);
    CloseHandle(future->cond);
#else
    pthread_mutex_destroy((pthread_mutex_t*)future->mutex);
    pthread_cond_destroy((pthread_cond_t*)future->cond);
    free(future->mutex);
    free(future->cond);
#endif
    
    free(future);
}
