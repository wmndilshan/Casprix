#include "examples/ecommerce/backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
typedef HANDLE BenchThread;
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t BenchThread;
#endif

typedef struct LoadContext {
    const char* host;
    uint16_t port;
    long request_count;
    volatile long next_request;
    volatile long successes;
    volatile long failures;
} LoadContext;

static long atomic_increment(volatile long* value) {
#ifdef _WIN32
    return InterlockedIncrement((volatile LONG*)value);
#else
    return __atomic_add_fetch(value, 1, __ATOMIC_RELAXED);
#endif
}

static bool send_all_client(Socket* socket, const char* data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        int count = socket_send(socket, data + sent, length - sent);
        if (count <= 0) return false;
        sent += (size_t)count;
    }
    return true;
}

static int execute_request(LoadContext* context) {
    static const char request[] =
        "GET /api/products HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n";
    Socket* socket = socket_create(SOCKET_TYPE_TCP);
    if (!socket) return 0;
    socket_set_timeout(socket, 3000);
    if (!socket_connect(socket, context->host, context->port) ||
        !send_all_client(socket, request, sizeof(request) - 1)) {
        socket_destroy(socket);
        return 0;
    }

    char response[2048];
    int received = socket_recv(socket, response, sizeof(response) - 1);
    socket_destroy(socket);
    if (received <= 0) return 0;
    response[received] = '\0';
    return strstr(response, "HTTP/1.1 200 OK") != NULL;
}

#ifdef _WIN32
static unsigned __stdcall load_worker(void* argument) {
#else
static void* load_worker(void* argument) {
#endif
    LoadContext* context = (LoadContext*)argument;
    for (;;) {
        long index = atomic_increment(&context->next_request) - 1;
        if (index >= context->request_count) break;
        if (execute_request(context)) atomic_increment(&context->successes);
        else atomic_increment(&context->failures);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifdef _WIN32
static unsigned __stdcall server_worker(void* argument) {
#else
static void* server_worker(void* argument) {
#endif
    ecommerce_backend_listen((EcommerceBackend*)argument);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static double monotonic_seconds(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency;
    LARGE_INTEGER value;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&value);
    return (double)value.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
#endif
}

static BenchThread start_thread(
#ifdef _WIN32
    unsigned (__stdcall *entry)(void*),
#else
    void* (*entry)(void*),
#endif
    void* argument) {
#ifdef _WIN32
    return (HANDLE)_beginthreadex(NULL, 0, entry, argument, 0, NULL);
#else
    pthread_t thread;
    if (pthread_create(&thread, NULL, entry, argument) != 0) return (pthread_t)0;
    return thread;
#endif
}

static void join_thread(BenchThread thread) {
#ifdef _WIN32
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    pthread_join(thread, NULL);
#endif
}

int main(int argc, char** argv) {
    long request_count = argc > 1 ? strtol(argv[1], NULL, 10) : 5000;
    int concurrency = argc > 2 ? atoi(argv[2]) : 64;
    int server_workers = argc > 3 ? atoi(argv[3]) : 32;
    uint16_t port = 18081;
    if (request_count < 1 || concurrency < 1 || concurrency > 512 || server_workers < 1) {
        fprintf(stderr, "usage: bench_http_concurrency [requests] [concurrency<=512] [server-workers]\n");
        return 2;
    }

    EcommerceBackend* backend = ecommerce_backend_create("127.0.0.1", port,
                                                           (size_t)server_workers);
    if (!backend) {
        fprintf(stderr, "failed to create ecommerce backend\n");
        return 1;
    }
    BenchThread server_thread = start_thread(server_worker, backend);
    if (!server_thread) {
        fprintf(stderr, "failed to start server thread\n");
        ecommerce_backend_destroy(backend);
        return 1;
    }
    int ready_attempts = 0;
    while (!ecommerce_backend_is_running(backend) && ready_attempts++ < 500) {
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10000);
#endif
    }
    if (!ecommerce_backend_is_running(backend)) {
        fprintf(stderr, "server did not become ready\n");
        join_thread(server_thread);
        ecommerce_backend_destroy(backend);
        return 1;
    }

    LoadContext context = {"127.0.0.1", port, request_count, 0, 0, 0};
    BenchThread* threads = (BenchThread*)calloc((size_t)concurrency, sizeof(BenchThread));
    double started = monotonic_seconds();
    int started_threads = 0;
    for (; started_threads < concurrency; started_threads++) {
        threads[started_threads] = start_thread(load_worker, &context);
        if (!threads[started_threads]) break;
    }
    if (started_threads != concurrency) {
        fprintf(stderr, "started only %d of %d client threads\n", started_threads, concurrency);
    }
    for (int i = 0; i < started_threads; i++) join_thread(threads[i]);
    double elapsed = monotonic_seconds() - started;
    free(threads);

    ecommerce_backend_stop(backend);
    join_thread(server_thread);
    HttpServerStats stats = ecommerce_backend_stats(backend);
    ecommerce_backend_destroy(backend);

    printf("requests=%ld concurrency=%d started_clients=%d server_workers=%d\n",
           request_count, concurrency, started_threads, server_workers);
    printf("success=%ld failure=%ld elapsed=%.3fs throughput=%.0f req/s\n",
           context.successes, context.failures, elapsed,
           elapsed > 0.0 ? (double)context.successes / elapsed : 0.0);
    printf("accepted=%llu completed=%llu rejected=%llu malformed=%llu active=%llu\n",
           (unsigned long long)stats.accepted_connections,
           (unsigned long long)stats.completed_connections,
           (unsigned long long)stats.rejected_connections,
           (unsigned long long)stats.malformed_requests,
           (unsigned long long)stats.active_connections);
    return started_threads == concurrency && context.failures == 0 &&
           context.successes == request_count ? 0 : 1;
}
