/** Cross-platform bounded and rendezvous channels. */
#include "channel.h"
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION ChannelMutex;
typedef CONDITION_VARIABLE ChannelCond;
#define mutex_init(m) (InitializeCriticalSection(m), true)
#define mutex_lock(m) EnterCriticalSection(m)
#define mutex_unlock(m) LeaveCriticalSection(m)
#define mutex_destroy(m) DeleteCriticalSection(m)
#define cond_init(c) (InitializeConditionVariable(c), true)
#define cond_wait(c, m) SleepConditionVariableCS(c, m, INFINITE)
#define cond_signal(c) WakeConditionVariable(c)
#define cond_broadcast(c) WakeAllConditionVariable(c)
#define cond_destroy(c) ((void)0)
#else
#include <pthread.h>
typedef pthread_mutex_t ChannelMutex;
typedef pthread_cond_t ChannelCond;
#define mutex_init(m) (pthread_mutex_init((m), NULL) == 0)
#define mutex_lock(m) pthread_mutex_lock(m)
#define mutex_unlock(m) pthread_mutex_unlock(m)
#define mutex_destroy(m) pthread_mutex_destroy(m)
#define cond_init(c) (pthread_cond_init((c), NULL) == 0)
#define cond_wait(c, m) pthread_cond_wait((c), (m))
#define cond_signal(c) pthread_cond_signal(c)
#define cond_broadcast(c) pthread_cond_broadcast(c)
#define cond_destroy(c) pthread_cond_destroy(c)
#endif

struct Channel {
    void** buffer;
    int capacity, size, read_pos, write_pos;
    ChannelMutex lock;
    ChannelCond not_empty, not_full;
    bool closed;
    int waiting_receivers;
};

Channel* channel_create(int capacity) {
    Channel* ch;
    if (capacity < 0) return NULL;
    ch = (Channel*)calloc(1, sizeof(Channel));
    if (!ch) return NULL;
    ch->capacity = capacity;
    ch->buffer = (void**)calloc((size_t)(capacity > 0 ? capacity : 1), sizeof(void*));
    if (!ch->buffer || !mutex_init(&ch->lock)) {
        free(ch->buffer); free(ch); return NULL;
    }
    if (!cond_init(&ch->not_empty) || !cond_init(&ch->not_full)) {
        mutex_destroy(&ch->lock); free(ch->buffer); free(ch); return NULL;
    }
    return ch;
}

bool channel_send(Channel* ch, void* message) {
    if (!ch) return false;
    mutex_lock(&ch->lock);
    if (ch->capacity == 0) {
        while (!ch->closed && (ch->size != 0 || ch->waiting_receivers == 0))
            cond_wait(&ch->not_full, &ch->lock);
        if (ch->closed) { mutex_unlock(&ch->lock); return false; }
        ch->buffer[0] = message; ch->size = 1;
        cond_signal(&ch->not_empty);
        while (!ch->closed && ch->size != 0) cond_wait(&ch->not_full, &ch->lock);
        bool delivered = ch->size == 0;
        mutex_unlock(&ch->lock);
        return delivered;
    }
    while (!ch->closed && ch->size == ch->capacity) cond_wait(&ch->not_full, &ch->lock);
    if (ch->closed) { mutex_unlock(&ch->lock); return false; }
    ch->buffer[ch->write_pos] = message;
    ch->write_pos = (ch->write_pos + 1) % ch->capacity;
    ++ch->size; cond_signal(&ch->not_empty); mutex_unlock(&ch->lock);
    return true;
}

bool channel_try_send(Channel* ch, void* message) {
    if (!ch) return false;
    mutex_lock(&ch->lock);
    if (ch->closed ||
        (ch->capacity == 0 && (ch->size != 0 || ch->waiting_receivers == 0)) ||
        (ch->capacity > 0 && ch->size == ch->capacity)) {
        mutex_unlock(&ch->lock); return false;
    }
    ch->buffer[ch->capacity == 0 ? 0 : ch->write_pos] = message;
    if (ch->capacity > 0) ch->write_pos = (ch->write_pos + 1) % ch->capacity;
    ++ch->size; cond_signal(&ch->not_empty); mutex_unlock(&ch->lock);
    return true;
}

void* channel_recv(Channel* ch) {
    void* message;
    if (!ch) return NULL;
    mutex_lock(&ch->lock);
    ++ch->waiting_receivers; cond_broadcast(&ch->not_full);
    while (!ch->closed && ch->size == 0) cond_wait(&ch->not_empty, &ch->lock);
    --ch->waiting_receivers;
    if (ch->size == 0) { mutex_unlock(&ch->lock); return NULL; }
    message = ch->buffer[ch->capacity == 0 ? 0 : ch->read_pos];
    if (ch->capacity > 0) ch->read_pos = (ch->read_pos + 1) % ch->capacity;
    --ch->size; cond_broadcast(&ch->not_full); mutex_unlock(&ch->lock);
    return message;
}

bool channel_try_recv(Channel* ch, void** out_message) {
    if (!ch || !out_message) return false;
    mutex_lock(&ch->lock);
    if (ch->size == 0) { mutex_unlock(&ch->lock); return false; }
    *out_message = ch->buffer[ch->capacity == 0 ? 0 : ch->read_pos];
    if (ch->capacity > 0) ch->read_pos = (ch->read_pos + 1) % ch->capacity;
    --ch->size; cond_broadcast(&ch->not_full); mutex_unlock(&ch->lock);
    return true;
}

void channel_close(Channel* ch) {
    if (!ch) return;
    mutex_lock(&ch->lock); ch->closed = true;
    cond_broadcast(&ch->not_empty); cond_broadcast(&ch->not_full);
    mutex_unlock(&ch->lock);
}

bool channel_is_closed(const Channel* ch) {
    bool closed;
    if (!ch) return true;
    mutex_lock((ChannelMutex*)&ch->lock); closed = ch->closed;
    mutex_unlock((ChannelMutex*)&ch->lock); return closed;
}

void channel_destroy(Channel* ch) {
    if (!ch) return;
    channel_close(ch);
    cond_destroy(&ch->not_empty); cond_destroy(&ch->not_full);
    mutex_destroy(&ch->lock); free(ch->buffer); free(ch);
}
