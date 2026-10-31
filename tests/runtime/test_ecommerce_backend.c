#include "examples/ecommerce/backend.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
typedef HANDLE TestThread;
static unsigned __stdcall server_main(void* value) {
    ecommerce_backend_listen((EcommerceBackend*)value);
    return 0;
}
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t TestThread;
static void* server_main(void* value) {
    ecommerce_backend_listen((EcommerceBackend*)value);
    return NULL;
}
#endif

static int failures = 0;
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); failures++; } \
} while (0)

static TestThread start_server(EcommerceBackend* backend) {
#ifdef _WIN32
    return (HANDLE)_beginthreadex(NULL, 0, server_main, backend, 0, NULL);
#else
    pthread_t thread;
    pthread_create(&thread, NULL, server_main, backend);
    return thread;
#endif
}

static void join_server(TestThread thread) {
#ifdef _WIN32
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    pthread_join(thread, NULL);
#endif
}

static int request(uint16_t port, const char* wire, char* response, size_t capacity) {
    Socket* socket = socket_create(SOCKET_TYPE_TCP);
    if (!socket || !socket_connect(socket, "127.0.0.1", port)) {
        socket_destroy(socket);
        return -1;
    }
    socket_set_timeout(socket, 2000);
    size_t wire_length = strlen(wire);
    size_t sent = 0;
    while (sent < wire_length) {
        int count = socket_send(socket, wire + sent, wire_length - sent);
        if (count <= 0) {
            socket_destroy(socket);
            return -1;
        }
        sent += (size_t)count;
    }

    size_t used = 0;
    while (used + 1 < capacity) {
        int count = socket_recv(socket, response + used, capacity - used - 1);
        if (count <= 0) break;
        used += (size_t)count;
    }
    response[used] = '\0';
    socket_destroy(socket);
    return (int)used;
}

int main(void) {
    const uint16_t port = 18082;
    EcommerceBackend* backend = ecommerce_backend_create("127.0.0.1", port, 8);
    CHECK(backend != NULL, "backend creation");
    if (!backend) return 1;
    TestThread server = start_server(backend);
    int ready_attempts = 0;
    while (!ecommerce_backend_is_running(backend) && ready_attempts++ < 500) {
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10000);
#endif
    }
    CHECK(ecommerce_backend_is_running(backend), "server readiness");

    char response[4096];
    request(port, "GET /health HTTP/1.1\r\nHost: test\r\n\r\n", response, sizeof(response));
    CHECK(strstr(response, "200 OK") && strstr(response, "casprix-store"), "health endpoint");

    request(port, "GET /api/products HTTP/1.1\r\nHost: test\r\n\r\n", response, sizeof(response));
    CHECK(strstr(response, "200 OK") && strstr(response, "Keyboard"), "product listing");

    HttpResponse* client_response = http_get("http://127.0.0.1:18082/api/products");
    CHECK(client_response && client_response->body &&
          strstr(client_response->body, "Monitor"), "HTTP client reads complete response");
    http_response_free(client_response);
    CHECK(http_get("https://unsupported.example/") == NULL, "unsupported URL rejected safely");

    request(port, "POST /api/cart?product=1&quantity=2 HTTP/1.1\r\nHost: test\r\nContent-Length: 0\r\n\r\n", response, sizeof(response));
    CHECK(strstr(response, "201 Created") && strstr(response, "\"quantity\":2"), "add to cart");

    request(port, "GET /api/cart HTTP/1.1\r\nHost: test\r\n\r\n", response, sizeof(response));
    CHECK(strstr(response, "\"quantity\":2"), "read cart");

    request(port, "POST /api/orders HTTP/1.1\r\nHost: test\r\nContent-Length: 0\r\n\r\n", response, sizeof(response));
    CHECK(strstr(response, "201 Created") && strstr(response, "\"order_id\":1000"), "create order");

    request(port, "PATCH /health HTTP/1.1\r\nHost: test\r\n\r\n", response, sizeof(response));
    CHECK(strstr(response, "400 Bad Request"), "unknown method rejected");

    request(port, "POST /api/cart HTTP/1.1\r\nHost: test\r\nContent-Length: 0\r\nContent-Length: 1\r\n\r\n", response, sizeof(response));
    CHECK(strstr(response, "400 Bad Request"), "ambiguous content length rejected");

    request(port, "POST /api/cart HTTP/1.1\r\nHost: test\r\nContent-Length: 70000\r\n\r\n", response, sizeof(response));
    CHECK(strstr(response, "400 Bad Request"), "oversized body rejected without allocation");

    ecommerce_backend_stop(backend);
    join_server(server);
    HttpServerStats stats = ecommerce_backend_stats(backend);
    CHECK(stats.accepted_connections == 9, "all connections accepted");
    CHECK(stats.completed_connections == 9, "all connections completed");
    CHECK(stats.rejected_connections == 0, "no overload rejection");
    CHECK(stats.malformed_requests == 3, "malformed request accounting");
    CHECK(stats.active_connections == 0, "no leaked active connections");
    ecommerce_backend_destroy(backend);

    if (failures) return 1;
    puts("E-commerce backend integration and HTTP hardening tests passed");
    return 0;
}
