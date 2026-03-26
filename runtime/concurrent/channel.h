/**
 * Casperix Runtime - Channel Implementation
 * CSP-style message passing for concurrent tasks
 */

#ifndef CASPERIX_CHANNEL_H
#define CASPERIX_CHANNEL_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

// Channel structure (generic container)
typedef struct Channel {
    void** buffer;           // Ring buffer for messages
    int capacity;            // 0 for unbuffered
    int size;                // Current number of messages
    int read_pos;            // Read position
    int write_pos;           // Write position
    
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    
    bool closed;
    int waiting_senders;
    int waiting_receivers;
} Channel;

// Create a channel
Channel* channel_create(int capacity);

// Send a message (blocks if full)
bool channel_send(Channel* ch, void* message);

// Try to send without blocking
bool channel_try_send(Channel* ch, void* message);

// Receive a message (blocks if empty)
void* channel_recv(Channel* ch);

// Try to receive without blocking
bool channel_try_recv(Channel* ch, void** out_message);

// Close the channel
void channel_close(Channel* ch);

// Check if closed
bool channel_is_closed(const Channel* ch);

// Destroy channel
void channel_destroy(Channel* ch);

#endif // CASPERIX_CHANNEL_H
