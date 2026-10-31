#include "runtime/async/async_runtime.h"
#include "runtime/concurrent/channel.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* CMake's Release configuration defines NDEBUG; runtime tests must still
   evaluate checks because many checks perform the operation under test. */
#undef assert
#define assert(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
        abort(); \
    } \
} while (0)

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#define sleep_ms(ms) Sleep(ms)
typedef HANDLE TestThread;
#else
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#define sleep_ms(ms) usleep((ms) * 1000)
typedef pthread_t TestThread;
#endif

static volatile long work_count;
static volatile long callback_count;
static volatile long timer_fired;
static volatile long socket_fired;

static long load_count(volatile long* value) {
#ifdef _WIN32
    return InterlockedCompareExchange((volatile LONG*)value, 0, 0);
#else
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}
static void increment_count(volatile long* value) {
#ifdef _WIN32
    InterlockedIncrement((volatile LONG*)value);
#else
    __atomic_add_fetch(value, 1, __ATOMIC_RELEASE);
#endif
}

static void* count_job(void* arg) {
    increment_count(&work_count);
    return arg;
}
static void reentrant_future_callback(void* result, void* user_data) {
    Future* future = (Future*)user_data;
    assert(result == NULL);
    /* This locks the same future. Completion must invoke us unlocked. */
    assert(future_is_ready(future));
    increment_count(&callback_count);
}
static void timer_callback(void* unused) {
    (void)unused;
    increment_count(&timer_fired);
}
static void socket_callback(EventWatcher* watcher, int events, void* unused) {
    char byte;
    (void)unused;
    if (events & EVENT_READ) {
#ifdef _WIN32
        recv((SOCKET)watcher->handle, &byte, 1, 0);
#else
        recv((int)watcher->handle, &byte, 1, 0);
#endif
        increment_count(&socket_fired);
    }
}

typedef struct ReceiverArgs { Channel* channel; void* result; } ReceiverArgs;
#ifdef _WIN32
static unsigned __stdcall receiver_main(void* arg)
#else
static void* receiver_main(void* arg)
#endif
{
    ReceiverArgs* args = (ReceiverArgs*)arg;
    args->result = channel_recv(args->channel);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void test_channels(void) {
    int first = 1, second = 2;
    Channel* buffered = channel_create(2);
    assert(buffered);
    assert(channel_try_send(buffered, &first));
    assert(channel_try_send(buffered, &second));
    assert(!channel_try_send(buffered, &first));
    assert(channel_recv(buffered) == &first);
    assert(channel_recv(buffered) == &second);
    channel_destroy(buffered);

    Channel* rendezvous = channel_create(0);
    ReceiverArgs args = {rendezvous, NULL};
#ifdef _WIN32
    TestThread thread = (HANDLE)_beginthreadex(NULL, 0, receiver_main, &args, 0, NULL);
    assert(thread);
#else
    TestThread thread;
    assert(pthread_create(&thread, NULL, receiver_main, &args) == 0);
#endif
    sleep_ms(10);
    assert(channel_send(rendezvous, &first));
#ifdef _WIN32
    WaitForSingleObject(thread, INFINITE); CloseHandle(thread);
#else
    pthread_join(thread, NULL);
#endif
    assert(args.result == &first);
    channel_destroy(rendezvous);
}

static void test_socket_readiness(EventLoop* loop) {
#ifdef _WIN32
    WSADATA data;
    assert(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    SOCKET receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SOCKET sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(receiver != INVALID_SOCKET && sender != INVALID_SOCKET);
#else
    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    assert(receiver >= 0 && sender >= 0);
#endif
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(receiver, (struct sockaddr*)&address, sizeof(address)) == 0);
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    assert(getsockname(receiver, (struct sockaddr*)&address, &length) == 0);
    assert(event_loop_add_watcher(loop, (EventHandle)receiver, EVENT_READ, socket_callback, NULL));
    assert(sendto(sender, "x", 1, 0, (struct sockaddr*)&address, sizeof(address)) == 1);
    for (int i = 0; i < 200 && !load_count(&socket_fired); ++i) sleep_ms(1);
    assert(load_count(&socket_fired) == 1);
    assert(event_loop_remove_watcher(loop, (EventHandle)receiver));
#ifdef _WIN32
    closesocket(sender); closesocket(receiver); WSACleanup();
#else
    close(sender); close(receiver);
#endif
}

int main(void) {
    enum { JOBS = 5000 };
    Future** futures = (Future**)calloc(JOBS, sizeof(Future*));
    assert(futures);
    AsyncRuntime* runtime = async_runtime_create(4);
    assert(runtime && async_runtime_is_running(runtime));

    for (int i = 0; i < JOBS; ++i) {
        futures[i] = async_runtime_submit(runtime, count_job, (void*)(size_t)i);
        assert(futures[i]);
    }
    Future* callback_future = async_runtime_submit(runtime, count_job, NULL);
    assert(callback_future);
    future_wait(callback_future);
    future_then(callback_future, reentrant_future_callback, callback_future);
    assert(load_count(&callback_count) == 1);
    future_destroy(callback_future);

    for (int i = 0; i < JOBS; ++i) {
        assert(future_wait(futures[i]) == (void*)(size_t)i);
        future_destroy(futures[i]);
    }
    free(futures);
    assert(load_count(&work_count) == JOBS + 1);

    assert(event_loop_add_timer(async_runtime_event_loop(runtime), 10, timer_callback, NULL, false));
    for (int i = 0; i < 200 && !load_count(&timer_fired); ++i) sleep_ms(1);
    assert(load_count(&timer_fired) == 1);
    test_socket_readiness(async_runtime_event_loop(runtime));
    test_channels();

    AsyncRuntimeStats stats = async_runtime_get_stats(runtime);
    assert(stats.workers.tasks_completed == JOBS + 1);
    assert(stats.active_tasks == 0);
    async_runtime_destroy(runtime);
    puts("async runtime: scheduler, futures, timers, socket readiness, channels OK");
    return 0;
}
