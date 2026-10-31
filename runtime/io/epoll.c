/* Linux epoll readiness event loop with thread-safe watcher updates. */
#ifndef _WIN32
#include "event_loop.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_EVENTS 128
#define WAKE_TOKEN UINT64_MAX

typedef struct Timer {
    uint64_t id, due_ms, interval_ms;
    TimerCallback callback;
    void* user_data;
    struct Timer* next;
} Timer;

struct EventLoop {
    int epoll_fd, wake_fd;
    pthread_mutex_t lock;
    bool running;
    EventWatcher* watchers;
    size_t watcher_count, watcher_capacity;
    Timer* timers;
    uint64_t next_timer_id;
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static void wake_loop(EventLoop* loop) {
    uint64_t one = 1;
    if (write(loop->wake_fd, &one, sizeof(one)) < 0 && errno != EAGAIN) { }
}
static uint32_t epoll_events(int events) {
    uint32_t result = EPOLLET;
    if (events & EVENT_READ) result |= EPOLLIN;
    if (events & EVENT_WRITE) result |= EPOLLOUT;
    return result;
}
static int timer_timeout(EventLoop* loop, int requested) {
    uint64_t now = now_ms(), nearest = UINT64_MAX;
    pthread_mutex_lock(&loop->lock);
    for (Timer* t = loop->timers; t; t = t->next)
        if (t->due_ms < nearest) nearest = t->due_ms;
    pthread_mutex_unlock(&loop->lock);
    if (nearest != UINT64_MAX) {
        uint64_t delta = nearest <= now ? 0 : nearest - now;
        if (delta > INT_MAX) delta = INT_MAX;
        if (requested < 0 || (int)delta < requested) requested = (int)delta;
    }
    return requested;
}
static void run_due_timers(EventLoop* loop) {
    for (;;) {
        TimerCallback callback = NULL;
        void* user_data = NULL;
        uint64_t now = now_ms();
        pthread_mutex_lock(&loop->lock);
        Timer** link = &loop->timers;
        while (*link && (*link)->due_ms > now) link = &(*link)->next;
        if (*link) {
            Timer* timer = *link;
            callback = timer->callback; user_data = timer->user_data;
            if (timer->interval_ms) timer->due_ms = now + timer->interval_ms;
            else { *link = timer->next; free(timer); }
        }
        pthread_mutex_unlock(&loop->lock);
        if (!callback) break;
        callback(user_data);
    }
}

EventLoop* event_loop_create(void) {
    EventLoop* loop = (EventLoop*)calloc(1, sizeof(EventLoop));
    if (!loop) return NULL;
    loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    loop->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (loop->epoll_fd < 0 || loop->wake_fd < 0 || pthread_mutex_init(&loop->lock, NULL) != 0) {
        if (loop->epoll_fd >= 0) close(loop->epoll_fd);
        if (loop->wake_fd >= 0) close(loop->wake_fd);
        free(loop); return NULL;
    }
    struct epoll_event wake = {0};
    wake.events = EPOLLIN; wake.data.u64 = WAKE_TOKEN;
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, loop->wake_fd, &wake) != 0) {
        event_loop_destroy(loop); return NULL;
    }
    loop->next_timer_id = 1;
    return loop;
}

void event_loop_destroy(EventLoop* loop) {
    if (!loop) return;
    event_loop_stop(loop);
    Timer* timer = loop->timers;
    while (timer) { Timer* next = timer->next; free(timer); timer = next; }
    free(loop->watchers);
    close(loop->wake_fd); close(loop->epoll_fd);
    pthread_mutex_destroy(&loop->lock); free(loop);
}

bool event_loop_add_watcher(EventLoop* loop, EventHandle handle, int events,
                            EventCallback callback, void* user_data) {
    if (!loop || handle < 0 || handle > INT_MAX || !callback) return false;
    pthread_mutex_lock(&loop->lock);
    for (size_t i = 0; i < loop->watcher_count; ++i)
        if (loop->watchers[i].handle == handle) { pthread_mutex_unlock(&loop->lock); return false; }
    if (loop->watcher_count == loop->watcher_capacity) {
        size_t capacity = loop->watcher_capacity ? loop->watcher_capacity * 2 : 16;
        EventWatcher* watchers = realloc(loop->watchers, capacity * sizeof(*watchers));
        if (!watchers) { pthread_mutex_unlock(&loop->lock); return false; }
        loop->watchers = watchers; loop->watcher_capacity = capacity;
    }
    struct epoll_event ev = {0};
    ev.events = epoll_events(events); ev.data.u64 = (uint64_t)handle;
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, (int)handle, &ev) != 0) {
        pthread_mutex_unlock(&loop->lock); return false;
    }
    EventWatcher* watcher = &loop->watchers[loop->watcher_count++];
    watcher->handle = handle; watcher->events = events;
    watcher->callback = callback; watcher->user_data = user_data; watcher->active = true;
    pthread_mutex_unlock(&loop->lock); wake_loop(loop); return true;
}

bool event_loop_remove_watcher(EventLoop* loop, EventHandle handle) {
    if (!loop || handle < 0 || handle > INT_MAX) return false;
    pthread_mutex_lock(&loop->lock);
    for (size_t i = 0; i < loop->watcher_count; ++i) {
        if (loop->watchers[i].handle == handle) {
            epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, (int)handle, NULL);
            loop->watchers[i] = loop->watchers[--loop->watcher_count];
            pthread_mutex_unlock(&loop->lock); wake_loop(loop); return true;
        }
    }
    pthread_mutex_unlock(&loop->lock); return false;
}

bool event_loop_modify_watcher(EventLoop* loop, EventHandle handle, int events) {
    if (!loop || handle < 0 || handle > INT_MAX) return false;
    pthread_mutex_lock(&loop->lock);
    for (size_t i = 0; i < loop->watcher_count; ++i) {
        if (loop->watchers[i].handle == handle) {
            struct epoll_event ev = {0};
            ev.events = epoll_events(events); ev.data.u64 = (uint64_t)handle;
            bool ok = epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, (int)handle, &ev) == 0;
            if (ok) loop->watchers[i].events = events;
            pthread_mutex_unlock(&loop->lock); wake_loop(loop); return ok;
        }
    }
    pthread_mutex_unlock(&loop->lock); return false;
}

bool event_loop_run_once(EventLoop* loop, int timeout_ms) {
    struct epoll_event events[MAX_EVENTS];
    if (!loop) return false;
    int count = epoll_wait(loop->epoll_fd, events, MAX_EVENTS, timer_timeout(loop, timeout_ms));
    if (count < 0) return errno == EINTR;
    for (int i = 0; i < count; ++i) {
        if (events[i].data.u64 == WAKE_TOKEN) {
            uint64_t value;
            while (read(loop->wake_fd, &value, sizeof(value)) > 0) { }
            continue;
        }
        EventWatcher snapshot = {0};
        EventHandle handle = (EventHandle)events[i].data.u64;
        pthread_mutex_lock(&loop->lock);
        for (size_t j = 0; j < loop->watcher_count; ++j)
            if (loop->watchers[j].handle == handle) { snapshot = loop->watchers[j]; break; }
        pthread_mutex_unlock(&loop->lock);
        if (!snapshot.active || !snapshot.callback) continue;
        int ready = 0;
        if (events[i].events & EPOLLIN) ready |= EVENT_READ;
        if (events[i].events & EPOLLOUT) ready |= EVENT_WRITE;
        if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) ready |= EVENT_ERROR;
        snapshot.callback(&snapshot, ready, snapshot.user_data);
    }
    run_due_timers(loop); return true;
}

void event_loop_run(EventLoop* loop) {
    if (!loop) return;
    pthread_mutex_lock(&loop->lock); loop->running = true; pthread_mutex_unlock(&loop->lock);
    for (;;) {
        pthread_mutex_lock(&loop->lock); bool running = loop->running; pthread_mutex_unlock(&loop->lock);
        if (!running) break;
        event_loop_run_once(loop, -1);
    }
}
void event_loop_stop(EventLoop* loop) {
    if (!loop) return;
    pthread_mutex_lock(&loop->lock); loop->running = false; pthread_mutex_unlock(&loop->lock);
    wake_loop(loop);
}
uint64_t event_loop_add_timer(EventLoop* loop, uint64_t delay_ms,
                              TimerCallback callback, void* user_data, bool repeating) {
    if (!loop || !callback) return 0;
    Timer* timer = calloc(1, sizeof(Timer));
    if (!timer) return 0;
    pthread_mutex_lock(&loop->lock);
    timer->id = loop->next_timer_id++; if (!timer->id) timer->id = loop->next_timer_id++;
    timer->due_ms = now_ms() + delay_ms;
    timer->interval_ms = repeating ? (delay_ms ? delay_ms : 1) : 0;
    timer->callback = callback; timer->user_data = user_data;
    timer->next = loop->timers; loop->timers = timer;
    pthread_mutex_unlock(&loop->lock); wake_loop(loop); return timer->id;
}
bool event_loop_cancel_timer(EventLoop* loop, uint64_t timer_id) {
    if (!loop || !timer_id) return false;
    pthread_mutex_lock(&loop->lock);
    Timer** link = &loop->timers;
    while (*link && (*link)->id != timer_id) link = &(*link)->next;
    if (!*link) { pthread_mutex_unlock(&loop->lock); return false; }
    Timer* timer = *link; *link = timer->next; free(timer);
    pthread_mutex_unlock(&loop->lock); wake_loop(loop); return true;
}
#endif
