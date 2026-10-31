/* Windows readiness event loop.
 *
 * Casprix exposes readiness watchers, not overlapped operations, so WSAPoll
 * is the correct backend for this API. A future completion-based API can use
 * IOCP separately without pretending that registering a handle starts I/O.
 */
#ifdef _WIN32

#include "event_loop.h"
#include <winsock2.h>
#include <windows.h>
#include <stdlib.h>
#include <limits.h>

typedef struct Timer {
    uint64_t id, due_ms, interval_ms;
    TimerCallback callback;
    void* user_data;
    struct Timer* next;
} Timer;

struct EventLoop {
    CRITICAL_SECTION lock;
    volatile LONG running;
    EventWatcher* watchers;
    size_t watcher_count, watcher_capacity;
    Timer* timers;
    uint64_t next_timer_id;
};

static uint64_t now_ms(void) { return GetTickCount64(); }
static void lock_loop(EventLoop* loop) { EnterCriticalSection(&loop->lock); }
static void unlock_loop(EventLoop* loop) { LeaveCriticalSection(&loop->lock); }

static int timer_timeout(EventLoop* loop, int requested) {
    uint64_t now = now_ms(), nearest = UINT64_MAX;
    lock_loop(loop);
    for (Timer* t = loop->timers; t; t = t->next)
        if (t->due_ms < nearest) nearest = t->due_ms;
    unlock_loop(loop);
    int timeout = requested;
    if (nearest != UINT64_MAX) {
        uint64_t delta = nearest <= now ? 0 : nearest - now;
        if (delta > INT_MAX) delta = INT_MAX;
        if (timeout < 0 || (int)delta < timeout) timeout = (int)delta;
    }
    /* WSAPoll has no external wake descriptor. Bound cross-thread update and
       stop latency without busy-spinning. */
    if (timeout < 0 || timeout > 25) timeout = 25;
    return timeout;
}

static void run_due_timers(EventLoop* loop) {
    for (;;) {
        TimerCallback callback = NULL;
        void* user_data = NULL;
        uint64_t now = now_ms();
        lock_loop(loop);
        Timer** link = &loop->timers;
        while (*link && (*link)->due_ms > now) link = &(*link)->next;
        if (*link) {
            Timer* timer = *link;
            callback = timer->callback;
            user_data = timer->user_data;
            if (timer->interval_ms) timer->due_ms = now + timer->interval_ms;
            else { *link = timer->next; free(timer); }
        }
        unlock_loop(loop);
        if (!callback) break;
        callback(user_data);
    }
}

EventLoop* event_loop_create(void) {
    EventLoop* loop = (EventLoop*)calloc(1, sizeof(EventLoop));
    if (!loop) return NULL;
    InitializeCriticalSection(&loop->lock);
    loop->next_timer_id = 1;
    return loop;
}

void event_loop_destroy(EventLoop* loop) {
    if (!loop) return;
    event_loop_stop(loop);
    Timer* timer = loop->timers;
    while (timer) { Timer* next = timer->next; free(timer); timer = next; }
    free(loop->watchers);
    DeleteCriticalSection(&loop->lock);
    free(loop);
}

bool event_loop_add_watcher(EventLoop* loop, EventHandle handle, int events,
                            EventCallback callback, void* user_data) {
    if (!loop || handle == (EventHandle)INVALID_SOCKET || !callback) return false;
    lock_loop(loop);
    for (size_t i = 0; i < loop->watcher_count; ++i) {
        if (loop->watchers[i].handle == handle) { unlock_loop(loop); return false; }
    }
    if (loop->watcher_count == loop->watcher_capacity) {
        size_t capacity = loop->watcher_capacity ? loop->watcher_capacity * 2 : 16;
        EventWatcher* watchers = (EventWatcher*)realloc(loop->watchers, capacity * sizeof(*watchers));
        if (!watchers) { unlock_loop(loop); return false; }
        loop->watchers = watchers;
        loop->watcher_capacity = capacity;
    }
    EventWatcher* watcher = &loop->watchers[loop->watcher_count++];
    watcher->handle = handle; watcher->events = events;
    watcher->callback = callback; watcher->user_data = user_data; watcher->active = true;
    unlock_loop(loop);
    return true;
}

bool event_loop_remove_watcher(EventLoop* loop, EventHandle handle) {
    if (!loop) return false;
    lock_loop(loop);
    for (size_t i = 0; i < loop->watcher_count; ++i) {
        if (loop->watchers[i].handle == handle) {
            loop->watchers[i] = loop->watchers[--loop->watcher_count];
            unlock_loop(loop); return true;
        }
    }
    unlock_loop(loop); return false;
}

bool event_loop_modify_watcher(EventLoop* loop, EventHandle handle, int events) {
    if (!loop) return false;
    lock_loop(loop);
    for (size_t i = 0; i < loop->watcher_count; ++i) {
        if (loop->watchers[i].handle == handle) {
            loop->watchers[i].events = events; unlock_loop(loop); return true;
        }
    }
    unlock_loop(loop); return false;
}

bool event_loop_run_once(EventLoop* loop, int timeout_ms) {
    WSAPOLLFD* fds = NULL;
    EventWatcher* snapshots = NULL;
    size_t count;
    if (!loop) return false;
    lock_loop(loop);
    count = loop->watcher_count;
    if (count) {
        fds = (WSAPOLLFD*)calloc(count, sizeof(*fds));
        snapshots = (EventWatcher*)malloc(count * sizeof(*snapshots));
        if (!fds || !snapshots) {
            free(fds); free(snapshots); unlock_loop(loop); return false;
        }
        for (size_t i = 0; i < count; ++i) {
            snapshots[i] = loop->watchers[i];
            fds[i].fd = (SOCKET)snapshots[i].handle;
            if (snapshots[i].events & EVENT_READ) fds[i].events |= POLLRDNORM;
            if (snapshots[i].events & EVENT_WRITE) fds[i].events |= POLLWRNORM;
        }
    }
    unlock_loop(loop);
    int ready = count ? WSAPoll(fds, (ULONG)count, timer_timeout(loop, timeout_ms))
                      : (Sleep((DWORD)timer_timeout(loop, timeout_ms)), 0);
    if (ready == SOCKET_ERROR) { free(fds); free(snapshots); return false; }
    for (size_t i = 0; ready > 0 && i < count; ++i) {
        if (!fds[i].revents) continue;
        int events = 0;
        if (fds[i].revents & (POLLRDNORM | POLLRDBAND)) events |= EVENT_READ;
        if (fds[i].revents & (POLLWRNORM | POLLWRBAND)) events |= EVENT_WRITE;
        if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) events |= EVENT_ERROR;
        if (events && snapshots[i].callback)
            snapshots[i].callback(&snapshots[i], events, snapshots[i].user_data);
        --ready;
    }
    free(fds); free(snapshots);
    run_due_timers(loop);
    return true;
}

void event_loop_run(EventLoop* loop) {
    if (!loop) return;
    InterlockedExchange(&loop->running, 1);
    while (InterlockedCompareExchange(&loop->running, 0, 0))
        event_loop_run_once(loop, -1);
}

void event_loop_stop(EventLoop* loop) {
    if (loop) InterlockedExchange(&loop->running, 0);
}

uint64_t event_loop_add_timer(EventLoop* loop, uint64_t delay_ms,
                              TimerCallback callback, void* user_data, bool repeating) {
    Timer* timer;
    if (!loop || !callback) return 0;
    timer = (Timer*)calloc(1, sizeof(Timer));
    if (!timer) return 0;
    lock_loop(loop);
    timer->id = loop->next_timer_id++;
    if (!timer->id) timer->id = loop->next_timer_id++;
    timer->due_ms = now_ms() + delay_ms;
    timer->interval_ms = repeating ? (delay_ms ? delay_ms : 1) : 0;
    timer->callback = callback; timer->user_data = user_data;
    timer->next = loop->timers; loop->timers = timer;
    unlock_loop(loop);
    return timer->id;
}

bool event_loop_cancel_timer(EventLoop* loop, uint64_t timer_id) {
    if (!loop || !timer_id) return false;
    lock_loop(loop);
    Timer** link = &loop->timers;
    while (*link && (*link)->id != timer_id) link = &(*link)->next;
    if (!*link) { unlock_loop(loop); return false; }
    Timer* timer = *link; *link = timer->next; free(timer);
    unlock_loop(loop); return true;
}
#endif
