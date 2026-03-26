/**
 * Casperix Runtime - Event Loop
 * Platform-agnostic event loop for async I/O
 */

#ifndef CASPERIX_EVENT_LOOP_H
#define CASPERIX_EVENT_LOOP_H

#include <stdint.h>
#include <stdbool.h>


// Event types
typedef enum {
    EVENT_READ = 1 << 0,
    EVENT_WRITE = 1 << 1,
    EVENT_ERROR = 1 << 2
} EventType;

// Forward declarations
typedef struct EventLoop EventLoop;
typedef struct EventWatcher EventWatcher;

// Event callback
typedef void (*EventCallback)(EventWatcher* watcher, int events, void* user_data);

// Event watcher (for file descriptors/sockets)
struct EventWatcher {
    int fd;
    int events;           // Events to watch (EVENT_READ | EVENT_WRITE)
    EventCallback callback;
    void* user_data;
    bool active;
};

// Timer callback
typedef void (*TimerCallback)(void* user_data);

// Event loop structure
struct EventLoop {
    void* backend_data;   // Platform-specific data
    bool running;
    int watcher_count;
    EventWatcher** watchers;
    int watcher_capacity;
};

// Create event loop
EventLoop* event_loop_create(void);

// Destroy event loop
void event_loop_destroy(EventLoop* loop);

// Run event loop (blocks until stopped)
void event_loop_run(EventLoop* loop);

// Stop event loop
void event_loop_stop(EventLoop* loop);

// Add watcher for file descriptor
bool event_loop_add_watcher(EventLoop* loop, int fd, int events, EventCallback callback, void* user_data);

// Remove watcher
bool event_loop_remove_watcher(EventLoop* loop, int fd);

// Modify watcher events
bool event_loop_modify_watcher(EventLoop* loop, int fd, int events);

// Add timer (returns timer ID)
uint64_t event_loop_add_timer(EventLoop* loop, uint64_t delay_ms, TimerCallback callback, void* user_data, bool repeating);

// Cancel timer
bool event_loop_cancel_timer(EventLoop* loop, uint64_t timer_id);

// Run one iteration (for integration with other loops)
bool event_loop_run_once(EventLoop* loop, int timeout_ms);

#endif // CASPERIX_EVENT_LOOP_H
