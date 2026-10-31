/**
 * Casprix HTTP Server Implementation
 *
 * Production-grade request handler with:
 *   - Full request line + header + body parsing
 *   - Middleware chain execution
 *   - Response header serialization (Content-Type, custom headers)
 *   - SO_REUSEADDR for fast server restarts
 *   - Query string extraction
 *   - HEAD method support
 *   - Connection: close for HTTP/1.0 compat
 */

#include "http_server.h"
#include "../async/async_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* MinGW printf doesn't support %zu — use %llu with cast */
#ifdef __MINGW32__
#define FMT_ZU "%llu"
#define CAST_ZU(x) ((unsigned long long)(x))
#else
#define FMT_ZU "%zu"
#define CAST_ZU(x) (x)
#endif

/* ========================================================================
 * Reliable I/O helpers
 * ======================================================================== */

/*
 * Send the entire buffer, looping over partial writes. socket_send() (send(2))
 * may transmit fewer bytes than requested; ignoring the return value silently
 * truncates the response. Returns true only if all `len` bytes were sent.
 * Relies on the socket's SO_SNDTIMEO (see socket_set_timeout) so a stalled
 * peer cannot wedge this loop forever — a timeout surfaces as send() <= 0.
 */
static bool send_all(Socket* sock, const void* buf, size_t len) {
    const char* p = (const char*)buf;
    size_t sent_total = 0;
    while (sent_total < len) {
        int n = socket_send(sock, p + sent_total, len - sent_total);
        if (n <= 0) return false;   /* error, EOF, or timeout */
        sent_total += (size_t)n;
    }
    return true;
}

/*
 * Read exactly `need` more bytes into buf[have_off ..]. Used to pull the rest
 * of a request body that didn't fit in the initial read. Returns true iff all
 * bytes were read; a recv() <= 0 (peer closed, error, or SO_RCVTIMEO timeout)
 * aborts and returns false so a slow-body attacker can't hang the worker.
 */
static bool recv_exact(Socket* sock, char* buf, size_t have_off, size_t need) {
    size_t got = 0;
    while (got < need) {
        int n = socket_recv(sock, buf + have_off + got, need - got);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

static uint64_t counter_increment(volatile uint64_t* value) {
#ifdef _WIN32
    return (uint64_t)InterlockedIncrement64((volatile LONG64*)value);
#else
    return __atomic_add_fetch(value, 1, __ATOMIC_RELAXED);
#endif
}

static uint64_t counter_decrement(volatile uint64_t* value) {
#ifdef _WIN32
    return (uint64_t)InterlockedDecrement64((volatile LONG64*)value);
#else
    return __atomic_sub_fetch(value, 1, __ATOMIC_RELAXED);
#endif
}

static uint64_t counter_load(const volatile uint64_t* value) {
#ifdef _WIN32
    return (uint64_t)InterlockedCompareExchange64((volatile LONG64*)value, 0, 0);
#else
    return __atomic_load_n(value, __ATOMIC_RELAXED);
#endif
}

static bool running_load(const volatile long* value) {
#ifdef _WIN32
    return InterlockedCompareExchange((volatile LONG*)value, 0, 0) != 0;
#else
    return __atomic_load_n(value, __ATOMIC_ACQUIRE) != 0;
#endif
}

static void running_store(volatile long* value, bool running) {
#ifdef _WIN32
    InterlockedExchange((volatile LONG*)value, running ? 1 : 0);
#else
    __atomic_store_n(value, running ? 1 : 0, __ATOMIC_RELEASE);
#endif
}

/* ========================================================================
 * Server lifecycle
 * ======================================================================== */

HttpServerConfig http_server_default_config(void) {
    HttpServerConfig config;
    config.worker_count = 0;
    config.backlog = 1024;
    config.io_timeout_ms = 5000;
    config.max_request_body = 1024u * 1024u;
    return config;
}

HttpServer* http_server_create_with_config(const char* bind_addr, uint16_t port,
                                           const HttpServerConfig* config) {
    if (!bind_addr) return NULL;
    HttpServer* server = (HttpServer*)calloc(1, sizeof(HttpServer));
    if (!server) return NULL;

    server->bind_addr = strdup(bind_addr);
    if (!server->bind_addr) {
        free(server);
        return NULL;
    }
    server->port = port;
    server->routes = NULL;
    server->middlewares = NULL;
    server->middleware_count = 0;
    running_store(&server->running, false);
    server->config = config ? *config : http_server_default_config();
    if (server->config.backlog <= 0) server->config.backlog = 1024;
    if (server->config.io_timeout_ms <= 0) server->config.io_timeout_ms = 5000;
    if (server->config.max_request_body == 0) {
        server->config.max_request_body = 1024u * 1024u;
    }

    server->async_runtime = async_runtime_create(server->config.worker_count);
    if (!server->async_runtime) {
        free(server->bind_addr);
        free(server);
        return NULL;
    }

    return server;
}

HttpServer* http_server_create(const char* bind_addr, uint16_t port) {
    return http_server_create_with_config(bind_addr, port, NULL);
}

void http_server_destroy(HttpServer* server) {
    if (!server) return;

    http_server_stop(server);
    if (server->async_runtime) {
        async_runtime_destroy((AsyncRuntime*)server->async_runtime);
        server->async_runtime = NULL;
    }

    free(server->bind_addr);

    /* Free route list */
    HttpRoute* route = server->routes;
    while (route) {
        HttpRoute* next = route->next;
        free(route->path);
        free(route);
        route = next;
    }

    /* Free middleware array */
    free(server->middlewares);

    if (server->listener) {
        socket_destroy(server->listener);
        server->listener = NULL;
    }

    if (server->socket_initialized) {
        socket_system_cleanup();
        server->socket_initialized = false;
    }

    free(server);
}

/* ========================================================================
 * Route & middleware registration
 * ======================================================================== */

bool http_server_route(HttpServer* server, HttpMethod method, const char* path,
                       RouteHandler handler, void* user_data) {
    if (!server || !path || !handler) return false;

    HttpRoute* route = (HttpRoute*)calloc(1, sizeof(HttpRoute));
    if (!route) return false;
    route->method = method;
    route->path = strdup(path);
    if (!route->path) {
        free(route);
        return false;
    }
    route->handler = handler;
    route->sync_handler = NULL;
    route->user_data = user_data;
    route->next = server->routes;

    server->routes = route;
    return true;
}

bool http_server_route_sync(HttpServer* server, HttpMethod method, const char* path,
                            SyncRouteHandler handler, void* user_data) {
    if (!server || !path || !handler) return false;
    HttpRoute* route = (HttpRoute*)calloc(1, sizeof(HttpRoute));
    if (!route) return false;
    route->path = strdup(path);
    if (!route->path) {
        free(route);
        return false;
    }
    route->method = method;
    route->sync_handler = handler;
    route->user_data = user_data;
    route->next = server->routes;
    server->routes = route;
    return true;
}

/* Internal middleware entry */
typedef struct {
    Middleware fn;
    void* user_data;
} MiddlewareEntry;

bool http_server_use(HttpServer* server, Middleware middleware, void* user_data) {
    if (!server || !middleware) return false;

    int new_count = server->middleware_count + 1;
    MiddlewareEntry* arr = (MiddlewareEntry*)realloc(
        server->middlewares, (size_t)new_count * sizeof(MiddlewareEntry));
    if (!arr) return false;

    arr[new_count - 1].fn = middleware;
    arr[new_count - 1].user_data = user_data;
    server->middlewares = (Middleware*)arr;
    server->middleware_count = new_count;
    return true;
}

/* ========================================================================
 * Route matching
 * ======================================================================== */

static HttpRoute* find_route(HttpServer* server, HttpMethod method, const char* path) {
    const char* q = strchr(path, '?');
    size_t path_length = q ? (size_t)(q - path) : strlen(path);

    HttpRoute* route = server->routes;
    while (route) {
        if (route->method == method && strlen(route->path) == path_length &&
            memcmp(route->path, path, path_length) == 0) {
            return route;
        }
        route = route->next;
    }
    return NULL;
}

/* ========================================================================
 * Request parsing
 * ======================================================================== */

static bool parse_method(const char* str, HttpMethod* method) {
    if (strcmp(str, "GET") == 0)    *method = HTTP_GET;
    else if (strcmp(str, "POST") == 0)   *method = HTTP_POST;
    else if (strcmp(str, "PUT") == 0)    *method = HTTP_PUT;
    else if (strcmp(str, "DELETE") == 0) *method = HTTP_DELETE;
    else if (strcmp(str, "HEAD") == 0)   *method = HTTP_HEAD;
    else return false;
    return true;
}

/* Return the value of the Content-Length header from a parsed request, or 0. */
static bool request_content_length(const HttpRequest* req, size_t maximum,
                                   size_t* content_length) {
    *content_length = 0;
    bool found = false;
    for (int i = 0; i < req->header_count; i++) {
        const char* h = req->headers[i];
        if (!h) continue;
        /* Case-insensitive prefix match on "Content-Length:" */
        const char* name = "content-length:";
        size_t n = strlen(name);
        bool match = true;
        for (size_t j = 0; j < n; j++) {
            char c = h[j];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != name[j]) { match = false; break; }
        }
        if (!match) continue;
        if (found) return false; /* Ambiguous framing enables request smuggling. */
        found = true;
        const char* v = h + n;
        while (*v == ' ' || *v == '\t') v++;
        if (*v < '0' || *v > '9') return false;
        unsigned long long parsed = 0;
        for (; *v >= '0' && *v <= '9'; v++) {
            if (parsed > (18446744073709551615ULL - 9ULL) / 10ULL) return false;
            parsed = parsed * 10 + (unsigned)(*v - '0');
            if (parsed > maximum) return false;
        }
        while (*v == ' ' || *v == '\t') v++;
        if (*v != '\0') return false;
        *content_length = (size_t)parsed;
    }
    return true;
}

/*
 * Parse a raw request buffer. Headers are fully parsed. The body is only
 * partially available here (whatever arrived before parse time): req->body is
 * allocated large enough to hold the full declared Content-Length, the bytes
 * present so far are copied in, and *body_have / *body_want report how many
 * body bytes we have vs. how many the Content-Length header declares. The
 * caller is responsible for reading the remaining (want - have) bytes.
 */
static bool parse_request(const char* raw, int raw_len, HttpRequest* req,
                          size_t max_body_size, size_t* body_have,
                          size_t* body_want) {
    memset(req, 0, sizeof(*req));
    if (body_have) *body_have = 0;
    if (body_want) *body_want = 0;

    /* Find end of request line */
    const char* line_end = strstr(raw, "\r\n");
    if (!line_end) return false;

    /* Parse request line: METHOD PATH HTTP/1.x */
    char method_str[16] = {0};
    char path_buf[1024] = {0};
    char version_buf[16] = {0};
    if (sscanf(raw, "%15s %1023s %15s", method_str, path_buf, version_buf) != 3) return false;
    if (strcmp(version_buf, "HTTP/1.1") != 0 && strcmp(version_buf, "HTTP/1.0") != 0) return false;
    if (path_buf[0] != '/') return false;

    if (!parse_method(method_str, &req->method)) return false;
    req->path = strdup(path_buf);
    req->url = strdup(path_buf);
    if (!req->path || !req->url) {
        free(req->path);
        free(req->url);
        memset(req, 0, sizeof(*req));
        return false;
    }

    /* Parse headers */
    const char* hdr = line_end + 2;
    while (hdr < raw + raw_len) {
        const char* hdr_end = strstr(hdr, "\r\n");
        if (!hdr_end || hdr_end == hdr) break; /* empty line = end of headers */

        if (req->header_count < 32) {
            size_t hdr_len = (size_t)(hdr_end - hdr);
            char* header_copy = (char*)malloc(hdr_len + 1);
            if (!header_copy) return false;
            memcpy(header_copy, hdr, hdr_len);
            header_copy[hdr_len] = '\0';
            req->headers[req->header_count++] = header_copy;
        } else {
            return false;
        }
        hdr = hdr_end + 2;
    }

    /* Find body (after \r\n\r\n) */
    const char* body_start = strstr(raw, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        size_t have = (size_t)(raw_len - (body_start - raw));
        size_t want = 0;
        if (!request_content_length(req, max_body_size, &want)) return false;
        if (have > want) have = want;
        size_t alloc = want;

        if (alloc > 0) {
            req->body = (char*)malloc(alloc + 1);
            if (!req->body) return false;
            memcpy(req->body, body_start, have);
            req->body[have] = '\0';       /* provisional NUL; finalized by caller */
            req->body_length = have;      /* provisional; caller bumps to `want` */
        }
        if (body_have) *body_have = have;
        if (body_want) *body_want = want;
    }

    return true;
}

static void free_parsed_request(HttpRequest* req) {
    free(req->path);
    free(req->url);
    free(req->body);
    for (int i = 0; i < req->header_count; i++) {
        free(req->headers[i]);
    }
}

static bool append_response_header(char* buffer, size_t capacity, size_t* used,
                                   const char* format, ...) {
    if (*used >= capacity) return false;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer + *used, capacity - *used, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= capacity - *used) return false;
    *used += (size_t)written;
    return true;
}

/* ========================================================================
 * Client handler
 * ======================================================================== */

typedef struct {
    HttpServer* server;
    Socket* client;
} ClientContext;

/*
 * Handle one accepted connection to completion: read + parse the request,
 * run middleware/routes, serialize + send the response, close the socket.
 * Synchronous and self-contained — safe to call directly on a worker thread.
 * Exposed (non-static) so tests can drive it on a loopback socket without the
 * listener/threadpool.
 */
void http_server_handle_connection(HttpServer* server, Socket* client) {
    if (!server || !client) {
        socket_destroy(client);
        return;
    }
    counter_increment(&server->active_connections);
    /*
     * Bound every blocking recv()/send() on this accepted connection so a
     * client that opens a socket and then stalls (Slowloris — trickling
     * headers, or a truncated body that never completes) cannot pin this
     * worker forever. Applied to server-accepted connections only; the
     * listener and client-side sockets elsewhere keep their own policy.
     */
    socket_set_timeout(client, server->config.io_timeout_ms);

    char buffer[8192];
    int total = 0;

    /* Read request headers (may arrive in multiple recv calls) */
    while (total < (int)sizeof(buffer) - 1) {
        int received = socket_recv(client, buffer + total, (int)sizeof(buffer) - 1 - total);
        if (received <= 0) break;   /* error, EOF, or SO_RCVTIMEO timeout */
        total += received;
        buffer[total] = '\0';
        /* Stop once the header terminator is in; body (if any) read below. */
        if (strstr(buffer, "\r\n\r\n")) break;
    }

    if (total <= 0) {
        socket_destroy(client);
        counter_decrement(&server->active_connections);
        counter_increment(&server->completed_connections);
        return;
    }

    if (!strstr(buffer, "\r\n\r\n")) {
        const char* too_large = "HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(client, too_large, strlen(too_large));
        socket_destroy(client);
        counter_increment(&server->malformed_requests);
        counter_decrement(&server->active_connections);
        counter_increment(&server->completed_connections);
        return;
    }

    /* Parse the request */
    HttpRequest request;
    size_t body_have = 0, body_want = 0;
    if (!parse_request(buffer, total, &request, server->config.max_request_body,
                       &body_have, &body_want)) {
        /* Malformed request — 400 */
        const char* bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 11\r\nConnection: close\r\n\r\nBad Request";
        send_all(client, bad, strlen(bad));
        free_parsed_request(&request);
        socket_destroy(client);
        counter_increment(&server->malformed_requests);
        counter_decrement(&server->active_connections);
        counter_increment(&server->completed_connections);
        return;
    }

    /*
     * The header-read loop above breaks at \r\n\r\n, so if the body straddled
     * a later TCP segment we only have `body_have` of the declared
     * `body_want` bytes. Pull the rest straight into req->body (already sized
     * to body_want by parse_request). recv_exact() honours SO_RCVTIMEO, so a
     * malicious slow-drip body aborts instead of hanging.
     */
    if (body_want > body_have && request.body) {
        if (!recv_exact(client, request.body, body_have, body_want - body_have)) {
            free_parsed_request(&request);
            const char* bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 23\r\nConnection: close\r\n\r\nIncomplete request body";
            send_all(client, bad, strlen(bad));
            socket_destroy(client);
            counter_increment(&server->malformed_requests);
            counter_decrement(&server->active_connections);
            counter_increment(&server->completed_connections);
            return;
        }
        request.body_length = body_want;
        request.body[body_want] = '\0';
    }

    /* Run middleware chain */
    HttpResponse* mw_response = NULL;
    if (server->middleware_count > 0) {
        MiddlewareEntry* mw_arr = (MiddlewareEntry*)server->middlewares;
        mw_response = (HttpResponse*)calloc(1, sizeof(HttpResponse));
        for (int i = 0; i < server->middleware_count; i++) {
            if (!mw_arr[i].fn(&request, mw_response, mw_arr[i].user_data)) {
                /* Middleware rejected — send its response */
                break;
            }
        }
        if (mw_response->status_code == 0) {
            /* Middleware didn't set a response — continue to route */
            http_response_free(mw_response);
            mw_response = NULL;
        }
    }

    HttpResponse* response = mw_response;

    if (!response) {
        /* Find matching route */
        HttpRoute* route = find_route(server, request.method, request.path);

        if (route) {
            if (route->sync_handler) {
                response = route->sync_handler(&request, route->user_data);
            } else {
                Future* future = route->handler(&request, route->user_data);
                if (future) {
                    response = (HttpResponse*)future_wait(future);
                    future_destroy(future);
                }
            }
            if (!response) response = http_response_error(500, "Handler returned NULL");
        } else {
            response = http_response_error(404, "Not Found");
        }
    }

    /* Build and send HTTP response */
    if (response) {
        char resp_buf[16384];
        size_t off = 0;
        bool header_ok = true;

        /* Status line */
        header_ok = append_response_header(resp_buf, sizeof(resp_buf), &off,
                                           "HTTP/1.1 %d %s\r\n",
                                           response->status_code,
                                           response->status_text ? response->status_text : "OK");

        /* Content-Length */
        if (header_ok) header_ok = append_response_header(
            resp_buf, sizeof(resp_buf), &off, "Content-Length: " FMT_ZU "\r\n",
            CAST_ZU(response->body_length));

        /* User-set headers (Content-Type, etc.) */
        for (int i = 0; i < response->header_count; i++) {
            if (header_ok && response->headers[i]) {
                header_ok = append_response_header(resp_buf, sizeof(resp_buf), &off,
                                                   "%s\r\n", response->headers[i]);
            }
        }

        /* Connection header */
        if (header_ok) header_ok = append_response_header(
            resp_buf, sizeof(resp_buf), &off, "Connection: close\r\n");

        /* End of headers */
        if (header_ok) header_ok = append_response_header(
            resp_buf, sizeof(resp_buf), &off, "\r\n");

        /* Send headers — loop over partial writes, don't discard the count */
        if (header_ok && send_all(client, resp_buf, off)) {
            /* Send body (skip for HEAD) */
            if (request.method != HTTP_HEAD && response->body && response->body_length > 0) {
                send_all(client, response->body, response->body_length);
            }
        }

        http_response_free(response);
    }

    free_parsed_request(&request);
    socket_destroy(client);
    counter_decrement(&server->active_connections);
    counter_increment(&server->completed_connections);
}

static void* handle_client_task(void* arg) {
    ClientContext* ctx = (ClientContext*)arg;
    HttpServer* server = ctx->server;
    Socket* client = ctx->client;
    free(ctx);

    /*
     * Run the handler directly on this worker thread. The previous version
     * spun a coroutine per connection and immediately switched into it — but
     * that switch was against the process-global main coroutine, so multiple
     * pool workers doing it concurrently corrupted each other's saved stack
     * pointer. The handler is fully synchronous, so the coroutine hop bought
     * nothing; calling it inline is both correct and simpler.
     */
    http_server_handle_connection(server, client);
    return NULL;
}

static bool submit_client(HttpServer* server, Socket* client) {
    ClientContext* ctx = (ClientContext*)malloc(sizeof(ClientContext));
    if (!ctx) return false;
    ctx->server = server;
    ctx->client = client;
    if (!async_runtime_submit_detached((AsyncRuntime*)server->async_runtime,
                                       handle_client_task, ctx)) {
        free(ctx);
        return false;
    }
    return true;
}

/* The listener is readiness-driven. Drain accept() until the non-blocking
   socket reports EWOULDBLOCK, then return immediately to the shared loop. */
static void accept_ready(EventWatcher* watcher, int events, void* user_data) {
    (void)watcher;
    HttpServer* server = (HttpServer*)user_data;
    if (!server || !running_load(&server->running) || !(events & EVENT_READ)) return;
    for (;;) {
        Socket* client = socket_accept(server->listener);
        if (!client) break;
        counter_increment(&server->accepted_connections);
        /* Listener readiness is non-blocking; the compatibility request
           handler uses bounded blocking I/O on workers. Windows can inherit
           FIONBIO from the listener, so normalize every accepted socket. */
        if (!socket_set_nonblocking(client, false)) {
            counter_increment(&server->rejected_connections);
            socket_destroy(client);
            continue;
        }
        if (!submit_client(server, client)) {
            counter_increment(&server->rejected_connections);
            socket_destroy(client);
        }
    }
}

/* ========================================================================
 * Server listen loop
 * ======================================================================== */

bool http_server_listen(HttpServer* server) {
    if (!server || running_load(&server->running) || server->listener) return false;

    if (!socket_system_init()) return false;
    server->socket_initialized = true;

    server->listener = socket_create(SOCKET_TYPE_TCP);
    if (!server->listener) {
        socket_system_cleanup();
        server->socket_initialized = false;
        return false;
    }

    /* Allow port reuse for fast server restarts */
    int reuse = 1;
    socket_set_option(server->listener, SOL_SOCKET, SO_REUSEADDR,
                      &reuse, sizeof(reuse));

    if (!socket_bind(server->listener, server->bind_addr, server->port)) {
        fprintf(stderr, "Failed to bind to %s:%d\n", server->bind_addr, server->port);
        socket_destroy(server->listener);
        server->listener = NULL;
        socket_system_cleanup();
        server->socket_initialized = false;
        return false;
    }

    if (!socket_listen(server->listener, server->config.backlog)) {
        fprintf(stderr, "Failed to listen\n");
        socket_destroy(server->listener);
        server->listener = NULL;
        socket_system_cleanup();
        server->socket_initialized = false;
        return false;
    }

    if (!socket_set_nonblocking(server->listener, true)) {
        socket_destroy(server->listener);
        server->listener = NULL;
        socket_system_cleanup();
        server->socket_initialized = false;
        return false;
    }

    EventLoop* loop = async_runtime_event_loop((AsyncRuntime*)server->async_runtime);
    running_store(&server->running, true);
    if (!event_loop_add_watcher(loop, (EventHandle)server->listener->handle,
                                EVENT_READ, accept_ready, server)) {
        running_store(&server->running, false);
        socket_destroy(server->listener);
        server->listener = NULL;
        socket_system_cleanup();
        server->socket_initialized = false;
        return false;
    }
    printf("HTTP Server listening on %s:%d (non-blocking accept, async workers)\n",
           server->bind_addr, server->port);

    while (running_load(&server->running)) {
#ifdef _WIN32
        Sleep(10);
#else
        struct timespec wait_time = {0, 10 * 1000 * 1000};
        nanosleep(&wait_time, NULL);
#endif
    }
    event_loop_remove_watcher(loop, (EventHandle)server->listener->handle);

    return true;
}

void http_server_stop(HttpServer* server) {
    if (!server) return;
    running_store(&server->running, false);
}

bool http_server_is_running(const HttpServer* server) {
    return server && running_load(&server->running);
}

HttpServerStats http_server_get_stats(const HttpServer* server) {
    HttpServerStats stats = {0};
    if (!server) return stats;
    stats.accepted_connections = counter_load(&server->accepted_connections);
    stats.completed_connections = counter_load(&server->completed_connections);
    stats.rejected_connections = counter_load(&server->rejected_connections);
    stats.malformed_requests = counter_load(&server->malformed_requests);
    stats.active_connections = counter_load(&server->active_connections);
    return stats;
}

/* ========================================================================
 * Response helpers
 * ======================================================================== */

HttpResponse* http_response_ok(const char* body) {
    HttpResponse* response = (HttpResponse*)calloc(1, sizeof(HttpResponse));
    if (!response) return NULL;
    response->status_code = 200;
    response->status_text = strdup("OK");
    if (body) {
        response->body = strdup(body);
        response->body_length = strlen(body);
    }
    return response;
}

HttpResponse* http_response_json(const char* json) {
    HttpResponse* response = http_response_ok(json);
    if (response && response->header_count < 32) {
        response->headers[response->header_count++] = strdup("Content-Type: application/json");
    }
    return response;
}

HttpResponse* http_response_error(int status_code, const char* message) {
    HttpResponse* response = (HttpResponse*)calloc(1, sizeof(HttpResponse));
    if (!response) return NULL;
    response->status_code = status_code;

    switch (status_code) {
        case 400: response->status_text = strdup("Bad Request"); break;
        case 403: response->status_text = strdup("Forbidden"); break;
        case 404: response->status_text = strdup("Not Found"); break;
        case 405: response->status_text = strdup("Method Not Allowed"); break;
        case 500: response->status_text = strdup("Internal Server Error"); break;
        case 503: response->status_text = strdup("Service Unavailable"); break;
        default:  response->status_text = strdup("Error");
    }

    if (message) {
        response->body = strdup(message);
        response->body_length = strlen(message);
    }
    return response;
}
