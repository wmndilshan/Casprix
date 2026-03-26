/**
 * Casperix Runtime - Channel Implementation
 */

#include "channel.h"
#include <stdlib.h>
#include <string.h>

Channel* channel_create(int capacity) {
    Channel* ch = (Channel*)calloc(1, sizeof(Channel));
    if (!ch) return NULL;
    
    ch->capacity = capacity;
    ch->size = 0;
    ch->read_pos = 0;
    ch->write_pos = 0;
    ch->closed = false;
    ch->waiting_senders = 0;
    ch->waiting_receivers = 0;
    
    if (capacity > 0) {
        ch->buffer = (void**)calloc(capacity, sizeof(void*));
        if (!ch->buffer) {
            free(ch);
            return NULL;
        }
    } else {
        ch->buffer = NULL;
    }
    
    pthread_mutex_init(&ch->lock, NULL);
    pthread_cond_init(&ch->not_empty, NULL);
    pthread_cond_init(&ch->not_full, NULL);
    
    return ch;
}

bool channel_send(Channel* ch, void* message) {
    if (!ch) return false;
    
    pthread_mutex_lock(&ch->lock);
    
    // Can't send to closed channel
    if (ch->closed) {
        pthread_mutex_unlock(&ch->lock);
        return false;
    }
    
    // Wait while full
    while (ch->size >= ch->capacity && !ch->closed) {
        ch->waiting_senders++;
        pthread_cond_wait(&ch->not_full, &ch->lock);
        ch->waiting_senders--;
    }
    
    if (ch->closed) {
        pthread_mutex_unlock(&ch->lock);
        return false;
    }
    
    // Add to buffer
    if (ch->capacity > 0) {
        ch->buffer[ch->write_pos] = message;
        ch->write_pos = (ch->write_pos + 1) % ch->capacity;
        ch->size++;
    }
    
    // Signal receivers
    pthread_cond_signal(&ch->not_empty);
    pthread_mutex_unlock(&ch->lock);
    
    return true;
}

bool channel_try_send(Channel* ch, void* message) {
    if (!ch) return false;
    
    pthread_mutex_lock(&ch->lock);
    
    if (ch->closed || (ch->capacity > 0 && ch->size >= ch->capacity)) {
        pthread_mutex_unlock(&ch->lock);
        return false;
    }
    
    if (ch->capacity > 0) {
        ch->buffer[ch->write_pos] = message;
        ch->write_pos = (ch->write_pos + 1) % ch->capacity;
        ch->size++;
    }
    
    pthread_cond_signal(&ch->not_empty);
    pthread_mutex_unlock(&ch->lock);
    
    return true;
}

void* channel_recv(Channel* ch) {
    if (!ch) return NULL;
    
    pthread_mutex_lock(&ch->lock);
    
    // Wait while empty and not closed
    while (ch->size == 0 && !ch->closed) {
        ch->waiting_receivers++;
        pthread_cond_wait(&ch->not_empty, &ch->lock);
        ch->waiting_receivers--;
    }
    
    if (ch->size == 0 && ch->closed) {
        pthread_mutex_unlock(&ch->lock);
        return NULL;  // Channel closed and empty
    }
    
    // Get from buffer
    void* message = NULL;
    if (ch->capacity > 0 && ch->size > 0) {
        message = ch->buffer[ch->read_pos];
        ch->read_pos = (ch->read_pos + 1) % ch->capacity;
        ch->size--;
    }
    
    // Signal senders
    pthread_cond_signal(&ch->not_full);
    pthread_mutex_unlock(&ch->lock);
    
    return message;
}

bool channel_try_recv(Channel* ch, void** out_message) {
    if (!ch || !out_message) return false;
    
    pthread_mutex_lock(&ch->lock);
    
    if (ch->size == 0) {
        pthread_mutex_unlock(&ch->lock);
        return false;
    }
    
    if (ch->capacity > 0) {
        *out_message = ch->buffer[ch->read_pos];
        ch->read_pos = (ch->read_pos + 1) % ch->capacity;
        ch->size--;
    }
    
    pthread_cond_signal(&ch->not_full);
    pthread_mutex_unlock(&ch->lock);
    
    return true;
}

void channel_close(Channel* ch) {
    if (!ch) return;
    
    pthread_mutex_lock(&ch->lock);
    ch->closed = true;
    
    // Wake up all waiting threads
    pthread_cond_broadcast(&ch->not_empty);
    pthread_cond_broadcast(&ch->not_full);
    
    pthread_mutex_unlock(&ch->lock);
}

bool channel_is_closed(const Channel* ch) {
    return ch && ch->closed;
}

void channel_destroy(Channel* ch) {
    if (!ch) return;
    
    free(ch->buffer);
    pthread_mutex_destroy(&ch->lock);
    pthread_cond_destroy(&ch->not_empty);
    pthread_cond_destroy(&ch->not_full);
    free(ch);
}
