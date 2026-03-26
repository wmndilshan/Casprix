/**
 * Casperix Runtime - Linux epoll Event Loop Backend
 * High-performance event loop using epoll for Linux
 */

#ifndef _WIN32

#include "../event_loop.h"
#include <sys/epoll.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#define MAX_EVENTS 128

// epoll backend data
typedef struct {
    int epoll_fd;
    struct epoll_event events[MAX_EVENTS];
} EpollBackend;

EventLoop* event_loop_create(void) {
    EventLoop* loop = (EventLoop*)calloc(1, sizeof(EventLoop));
    if (!loop) return NULL;
    
    EpollBackend* backend = (EpollBackend*)calloc(1, sizeof(EpollBackend));
    if (!backend) {
        free(loop);
        return NULL;
    }
    
    // Create epoll instance
    backend->epoll_fd = epoll_create1(0);
    if (backend->epoll_fd < 0) {
        free(backend);
        free(loop);
        return NULL;
    }
    
    loop->backend_data = backend;
    loop->running = false;
    loop->watchers = NULL;
    loop->watcher_count = 0;
    loop->watcher_capacity = 0;
    
    return loop;
}

void event_loop_destroy(EventLoop* loop) {
    if (!loop) return;
    
    EpollBackend* backend = (EpollBackend*)loop->backend_data;
    if (backend) {
        if (backend->epoll_fd >= 0) {
            close(backend->epoll_fd);
        }
        free(backend);
    }
    
    // Free watchers
    for (int i = 0; i < loop->watcher_count; i++) {
        free(loop->watchers[i]);
    }
    free(loop->watchers);
    free(loop);
}

bool event_loop_add_watcher(EventLoop* loop, int fd, int events, EventCallback callback, void* user_data) {
    if (!loop) return false;
    
    EpollBackend* backend = (EpollBackend*)loop->backend_data;
    
    // Create watcher
    EventWatcher* watcher = (EventWatcher*)calloc(1, sizeof(EventWatcher));
    watcher->fd = fd;
    watcher->events = events;
    watcher->callback = callback;
    watcher->user_data = user_data;
    watcher->active = true;
    
    // Add to epoll
    struct epoll_event ev;
    ev.events = 0;
    if (events & EVENT_READ) ev.events |= EPOLLIN;
    if (events & EVENT_WRITE) ev.events |= EPOLLOUT;
    ev.events |= EPOLLET;  // Edge-triggered mode
    ev.data.ptr = watcher;
    
    if (epoll_ctl(backend->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        free(watcher);
        return false;
    }
    
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
    
    EpollBackend* backend = (EpollBackend*)loop->backend_data;
    
    // Remove from epoll
    epoll_ctl(backend->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    
    // Remove from watcher list
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

bool event_loop_modify_watcher(EventLoop* loop, int fd, int events) {
    if (!loop) return false;
    
    EpollBackend* backend = (EpollBackend*)loop->backend_data;
    
    // Find watcher
    EventWatcher* watcher = NULL;
    for (int i = 0; i < loop->watcher_count; i++) {
        if (loop->watchers[i]->fd == fd) {
            watcher = loop->watchers[i];
            break;
        }
    }
    
    if (!watcher) return false;
    
    watcher->events = events;
    
    // Modify epoll entry
    struct epoll_event ev;
    ev.events = 0;
    if (events & EVENT_READ) ev.events |= EPOLLIN;
    if (events & EVENT_WRITE) ev.events |= EPOLLOUT;
    ev.events |= EPOLLET;
    ev.data.ptr = watcher;
    
    return epoll_ctl(backend->epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0;
}

bool event_loop_run_once(EventLoop* loop, int timeout_ms) {
    if (!loop) return false;
    
    EpollBackend* backend = (EpollBackend*)loop->backend_data;
    
    int nfds = epoll_wait(backend->epoll_fd, backend->events, MAX_EVENTS, timeout_ms);
    
    if (nfds < 0) {
        if (errno == EINTR) return true;  // Interrupted, try again
        return false;
    }
    
    // Process events
    for (int i = 0; i < nfds; i++) {
        EventWatcher* watcher = (EventWatcher*)backend->events[i].data.ptr;
        
        if (!watcher || !watcher->active) continue;
        
        int events = 0;
        if (backend->events[i].events & EPOLLIN) events |= EVENT_READ;
        if (backend->events[i].events & EPOLLOUT) events |= EVENT_WRITE;
        if (backend->events[i].events & (EPOLLERR | EPOLLHUP)) events |= EVENT_ERROR;
        
        // Call callback
        if (watcher->callback) {
            watcher->callback(watcher, events, watcher->user_data);
        }
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
}

uint64_t event_loop_add_timer(EventLoop* loop, uint64_t delay_ms, TimerCallback callback, void* user_data, bool repeating) {
    // TODO: Implement using timerfd_create
    return 0;
}

bool event_loop_cancel_timer(EventLoop* loop, uint64_t timer_id) {
    // TODO: Implement timer cancellation
    return false;
}

#endif // !_WIN32
