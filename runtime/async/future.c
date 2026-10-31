/**
 * Casprix Runtime - Future Implementation
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
    if (!future->mutex || !future->cond) {
        if (future->mutex) CloseHandle(future->mutex);
        if (future->cond) CloseHandle(future->cond);
        free(future);
        return NULL;
    }
#else
    pthread_mutex_t* mutex = malloc(sizeof(pthread_mutex_t));
    pthread_cond_t* cond = malloc(sizeof(pthread_cond_t));
    if (!mutex || !cond || pthread_mutex_init(mutex, NULL) != 0) {
        free(mutex);
        free(cond);
        free(future);
        return NULL;
    }
    if (pthread_cond_init(cond, NULL) != 0) {
        pthread_mutex_destroy(mutex);
        free(mutex);
        free(cond);
        free(future);
        return NULL;
    }
    future->mutex = mutex;
    future->cond = cond;
#endif
    
    return future;
}

static void future_lock(Future* future) {
#ifdef _WIN32
    WaitForSingleObject(future->mutex, INFINITE);
#else
    pthread_mutex_lock((pthread_mutex_t*)future->mutex);
#endif
}

static void future_unlock(Future* future) {
#ifdef _WIN32
    ReleaseMutex(future->mutex);
#else
    pthread_mutex_unlock((pthread_mutex_t*)future->mutex);
#endif
}

static void future_signal(Future* future) {
#ifdef _WIN32
    SetEvent(future->cond);
#else
    pthread_cond_broadcast((pthread_cond_t*)future->cond);
#endif
}

static void future_finish(Future* future, FutureState state, void* value) {
    FutureCallback* callbacks;
    void** callback_data;
    int callback_count;

    if (!future) return;
    future_lock(future);
    if (future->state != FUTURE_PENDING) {
        future_unlock(future);
        return;
    }
    future->state = state;
    if (state == FUTURE_COMPLETED) future->result = value;
    else if (state == FUTURE_FAILED) future->error = value;

    callbacks = future->callbacks;
    callback_data = future->callback_data;
    callback_count = future->callback_count;
    future->callbacks = NULL;
    future->callback_data = NULL;
    future->callback_count = 0;
    future->callback_capacity = 0;
    future_signal(future);
    future_unlock(future);

    /* Callbacks may wait on or destroy other futures. Never run them while
       holding this future's mutex. */
    for (int i = 0; i < callback_count; ++i) {
        if (callbacks[i]) callbacks[i](state == FUTURE_COMPLETED ? value : NULL,
                                       callback_data[i]);
    }
    free(callbacks);
    free(callback_data);
}

void future_complete(Future* future, void* result) {
    future_finish(future, FUTURE_COMPLETED, result);
}

void future_fail(Future* future, void* error) {
    future_finish(future, FUTURE_FAILED, error);
}

void future_cancel(Future* future) {
    future_finish(future, FUTURE_CANCELLED, NULL);
}

void future_then(Future* future, FutureCallback callback, void* user_data) {
    if (!future || !callback) return;
    
    future_lock(future);
    
    // If already complete, call immediately
    if (future->state == FUTURE_COMPLETED) {
        void* result = future->result;
        future_unlock(future);
        callback(result, user_data);
        return;
    }
    if (future->state != FUTURE_PENDING) {
        future_unlock(future);
        callback(NULL, user_data);
        return;
    }
    
    // Add to callback list
    if (future->callback_count >= future->callback_capacity) {
        int new_capacity = future->callback_capacity == 0 ? 4 : future->callback_capacity * 2;
        FutureCallback* callbacks = malloc(sizeof(FutureCallback) * new_capacity);
        void** callback_data = malloc(sizeof(void*) * new_capacity);
        if (!callbacks || !callback_data) {
            free(callbacks);
            free(callback_data);
            future_unlock(future);
            return;
        }
        if (future->callback_count > 0) {
            memcpy(callbacks, future->callbacks,
                   sizeof(FutureCallback) * future->callback_count);
            memcpy(callback_data, future->callback_data,
                   sizeof(void*) * future->callback_count);
        }
        free(future->callbacks);
        free(future->callback_data);
        future->callbacks = callbacks;
        future->callback_data = callback_data;
        future->callback_capacity = new_capacity;
    }
    
    future->callbacks[future->callback_count] = callback;
    future->callback_data[future->callback_count] = user_data;
    future->callback_count++;
    
    future_unlock(future);
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
    return future && future_get_state(future) != FUTURE_PENDING;
}

FutureState future_get_state(const Future* future) {
    FutureState state;
    if (!future) return FUTURE_CANCELLED;
    future_lock((Future*)future);
    state = future->state;
    future_unlock((Future*)future);
    return state;
}

void* future_get_error(const Future* future) {
    void* error;
    if (!future) return NULL;
    future_lock((Future*)future);
    error = future->error;
    future_unlock((Future*)future);
    return error;
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
