/**
 * Casperix Runtime - Windows IOCP Event Loop Backend
 * I/O Completion Ports for high-performance async I/O on Windows
 */

#ifdef _WIN32

#include "event_loop.h"
#include <windows.h>
#include <io.h>
#include <stdlib.h>
#include <stdio.h>

// Per-operation context
typedef struct {
    OVERLAPPED overlapped;
    EventCallback callback;
    void* user_data;
    int fd;
    int event_type;
} IOOperation;

// IOCP backend data
typedef struct {
    HANDLE iocp_handle;
    HANDLE* thread_handles;
    int thread_count;
    bool running;
} IOCPBackend;

EventLoop* event_loop_create(void) {
    EventLoop* loop = (EventLoop*)calloc(1, sizeof(EventLoop));
    if (!loop) return NULL;
    
    IOCPBackend* backend = (IOCPBackend*)calloc(1, sizeof(IOCPBackend));
    if (!backend) {
        free(loop);
        return NULL;
    }
    
    // Create IOCP handle with auto-detect thread count
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int num_threads = si.dwNumberOfProcessors;
    
    backend->iocp_handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, num_threads);
    if (!backend->iocp_handle) {
        free(backend);
        free(loop);
        return NULL;
    }
    
    backend->thread_count = num_threads;
    backend->running = false;
    
    loop->backend_data = backend;
    loop->running = false;
    loop->watchers = NULL;
    loop->watcher_count = 0;
    loop->watcher_capacity = 0;
    
    return loop;
}

void event_loop_destroy(EventLoop* loop) {
    if (!loop) return;
    
    IOCPBackend* backend = (IOCPBackend*)loop->backend_data;
    if (backend) {
        if (backend->iocp_handle) {
            CloseHandle(backend->iocp_handle);
        }
        free(backend);
    }
    
    free(loop->watchers);
    free(loop);
}

bool event_loop_add_watcher(EventLoop* loop, int fd, int events, EventCallback callback, void* user_data) {
    if (!loop) return false;
    
    IOCPBackend* backend = (IOCPBackend*)loop->backend_data;
    
    // Associate handle with IOCP
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    if (handle == INVALID_HANDLE_VALUE) return false;
    
    HANDLE result = CreateIoCompletionPort(handle, backend->iocp_handle, (ULONG_PTR)fd, 0);
    if (!result) return false;
    
    // Create watcher
    EventWatcher* watcher = (EventWatcher*)calloc(1, sizeof(EventWatcher));
    watcher->fd = fd;
    watcher->events = events;
    watcher->callback = callback;
    watcher->user_data = user_data;
    watcher->active = true;
    
    // Add to watcher list
    if (loop->watcher_count >= loop->watcher_capacity) {
        int new_capacity = loop->watcher_capacity == 0 ? 16 : loop->watcher_capacity * 2;
        loop->watchers = (EventWatcher**)realloc(loop->watchers, sizeof(EventWatcher*) * new_capacity);
        loop->watcher_capacity = new_capacity;
    }
    
    loop->watchers[loop->watcher_count++] = watcher;
    
    return true;
}

bool event_loop_remove_watcher(EventLoop* loop, int fd) {
    if (!loop) return false;
    
    for (int i = 0; i < loop->watcher_count; i++) {
        if (loop->watchers[i]->fd == fd) {
            free(loop->watchers[i]);
            // Shift remaining watchers
            for (int j = i; j < loop->watcher_count - 1; j++) {
                loop->watchers[j] = loop->watchers[j + 1];
            }
            loop->watcher_count--;
            return true;
        }
    }
    
    return false;
}

bool event_loop_run_once(EventLoop* loop, int timeout_ms) {
    if (!loop) return false;
    
    IOCPBackend* backend = (IOCPBackend*)loop->backend_data;
    
    DWORD bytes_transferred;
    ULONG_PTR completion_key;
    OVERLAPPED* overlapped;
    
    BOOL success = GetQueuedCompletionStatus(
        backend->iocp_handle,
        &bytes_transferred,
        &completion_key,
        &overlapped,
        timeout_ms == -1 ? INFINITE : timeout_ms
    );
    
    if (!success && overlapped == NULL) {
        // Timeout or error
        return false;
    }
    
    if (overlapped) {
        IOOperation* op = CONTAINING_RECORD(overlapped, IOOperation, overlapped);
        
        // Find corresponding watcher
        for (int i = 0; i < loop->watcher_count; i++) {
            if (loop->watchers[i]->fd == op->fd) {
                // Call callback
                if (loop->watchers[i]->callback) {
                    loop->watchers[i]->callback(loop->watchers[i], op->event_type, op->user_data);
                }
                break;
            }
        }
        
        free(op);
    }
    
    return true;
}

void event_loop_run(EventLoop* loop) {
    if (!loop) return;
    
    loop->running = true;
    
    while (loop->running) {
        event_loop_run_once(loop, -1);  // Block indefinitely
    }
}

void event_loop_stop(EventLoop* loop) {
    if (!loop) return;
    loop->running = false;
    
    // Post quit message to IOCP to wake up waiting threads
    IOCPBackend* backend = (IOCPBackend*)loop->backend_data;
    PostQueuedCompletionStatus(backend->iocp_handle, 0, 0, NULL);
}

bool event_loop_modify_watcher(EventLoop* loop, int fd, int events) {
    if (!loop) return false;
    
    for (int i = 0; i < loop->watcher_count; i++) {
        if (loop->watchers[i]->fd == fd) {
            loop->watchers[i]->events = events;
            return true;
        }
    }
    
    return false;
}

uint64_t event_loop_add_timer(EventLoop* loop, uint64_t delay_ms, TimerCallback callback, void* user_data, bool repeating) {
    // TODO: Implement timer using Windows timer queue
    return 0;
}

bool event_loop_cancel_timer(EventLoop* loop, uint64_t timer_id) {
    // TODO: Implement timer cancellation
    return false;
}

#endif /* _WIN32 */
