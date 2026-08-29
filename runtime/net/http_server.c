/**
 * Casperix HTTP Server Implementation
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
#include "../async/scheduler.h"
#include "../async/coroutine.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

/* ========================================================================
 * Server lifecycle
 * ======================================================================== */

HttpServer* http_server_create(const char* bind_addr, uint16_t port) {
    HttpServer* server = (HttpServer*)calloc(1, sizeof(HttpServer));
    if (!server) return NULL;

    server->bind_addr = strdup(bind_addr);
    server->port = port;
    server->routes = NULL;
    server->middlewares = NULL;
    server->middleware_count = 0;
    server->running = false;
    
    // Create a worker pool with thread count equal to CPU cores
    server->pool = pool_create(0); 

    return server;
}

void http_server_destroy(HttpServer* server) {
    if (!server) return;

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
    route->method = method;
    route->path = strdup(path);
    route->handler = handler;
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
    /* Strip query string for matching */
    char clean_path[512];
    const char* q = strchr(path, '?');
    if (q) {
        size_t len = (size_t)(q - path);
        if (len >= sizeof(clean_path)) len = sizeof(clean_path) - 1;
        memcpy(clean_path, path, len);
        clean_path[len] = '\0';
    } else {
        snprintf(clean_path, sizeof(clean_path), "%s", path);
    }

    HttpRoute* route = server->routes;
    while (route) {
        if (route->method == method && strcmp(route->path, clean_path) == 0) {
            return route;
        }
        route = route->next;
    }
    return NULL;
}

/* ========================================================================
 * Request parsing
 * ======================================================================== */

static HttpMethod parse_method(const char* str) {
    if (strcmp(str, "GET") == 0)    return HTTP_GET;
    if (strcmp(str, "POST") == 0)   return HTTP_POST;
    if (strcmp(str, "PUT") == 0)    return HTTP_PUT;
    if (strcmp(str, "DELETE") == 0) return HTTP_DELETE;
    if (strcmp(str, "HEAD") == 0)   return HTTP_HEAD;
    return HTTP_GET;
}

/* Return the value of the Content-Length header from a parsed request, or 0. */
static size_t request_content_length(const HttpRequest* req) {
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
        const char* v = h + n;
        while (*v == ' ' || *v == '\t') v++;
        if (*v < '0' || *v > '9') return 0;
        /* Hard cap the declared body size so a bogus/huge Content-Length can't
         * drive a giant malloc in parse_request. 64 MiB is well beyond any
         * legitimate request this server handles. */
        const unsigned long long CAP = 64ULL * 1024 * 1024;
        unsigned long long parsed = 0;
        for (; *v >= '0' && *v <= '9'; v++) {
            parsed = parsed * 10 + (unsigned)(*v - '0');
            if (parsed > CAP) { parsed = CAP; break; }
        }
        return (size_t)parsed;
    }
    return 0;
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
                          size_t* body_have, size_t* body_want) {
    memset(req, 0, sizeof(*req));
    if (body_have) *body_have = 0;
    if (body_want) *body_want = 0;

    /* Find end of request line */
    const char* line_end = strstr(raw, "\r\n");
    if (!line_end) return false;

    /* Parse request line: METHOD PATH HTTP/1.x */
    char method_str[16] = {0};
    char path_buf[1024] = {0};
    if (sscanf(raw, "%15s %1023s", method_str, path_buf) != 2) return false;

    req->method = parse_method(method_str);
    req->path = strdup(path_buf);
    req->url = strdup(path_buf);

    /* Parse headers */
    const char* hdr = line_end + 2;
    while (hdr < raw + raw_len) {
        const char* hdr_end = strstr(hdr, "\r\n");
        if (!hdr_end || hdr_end == hdr) break; /* empty line = end of headers */

        if (req->header_count < 32) {
            size_t hdr_len = (size_t)(hdr_end - hdr);
            char* header_copy = (char*)malloc(hdr_len + 1);
            memcpy(header_copy, hdr, hdr_len);
            header_copy[hdr_len] = '\0';
            req->headers[req->header_count++] = header_copy;
        }
        hdr = hdr_end + 2;
    }

    /* Find body (after \r\n\r\n) */
    const char* body_start = strstr(raw, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        size_t have = (size_t)(raw_len - (body_start - raw));
        size_t want = request_content_length(req);
        /* If Content-Length is absent/zero, fall back to "whatever arrived". */
        size_t alloc = (want > have) ? want : have;

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
    /*
     * Bound every blocking recv()/send() on this accepted connection so a
     * client that opens a socket and then stalls (Slowloris — trickling
     * headers, or a truncated body that never completes) cannot pin this
     * worker forever. Applied to server-accepted connections only; the
     * listener and client-side sockets elsewhere keep their own policy.
     */
    socket_set_timeout(client, SOCKET_DEFAULT_IO_TIMEOUT_MS);

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
        return;
    }

    /* Parse the request */
    HttpRequest request;
    size_t body_have = 0, body_want = 0;
    if (!parse_request(buffer, total, &request, &body_have, &body_want)) {
        /* Malformed request — 400 */
        const char* bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 11\r\nConnection: close\r\n\r\nBad Request";
        send_all(client, bad, strlen(bad));
        socket_destroy(client);
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
            Future* future = route->handler(&request, route->user_data);
            if (future) {
                response = (HttpResponse*)future_wait(future);
                future_destroy(future);
            } else {
                response = http_response_error(500, "Handler returned NULL");
            }
        } else {
            response = http_response_error(404, "Not Found");
        }
    }

    /* Build and send HTTP response */
    if (response) {
        char resp_buf[16384];
        int off = 0;

        /* Status line */
        off += snprintf(resp_buf + off, sizeof(resp_buf) - off,
                        "HTTP/1.1 %d %s\r\n",
                        response->status_code,
                        response->status_text ? response->status_text : "OK");

        /* Content-Length */
        off += snprintf(resp_buf + off, sizeof(resp_buf) - off,
                        "Content-Length: " FMT_ZU "\r\n",
                        CAST_ZU(response->body_length));

        /* User-set headers (Content-Type, etc.) */
        for (int i = 0; i < response->header_count; i++) {
            if (response->headers[i]) {
                off += snprintf(resp_buf + off, sizeof(resp_buf) - off,
                                "%s\r\n", response->headers[i]);
            }
        }

        /* Connection header */
        off += snprintf(resp_buf + off, sizeof(resp_buf) - off,
                        "Connection: close\r\n");

        /* End of headers */
        off += snprintf(resp_buf + off, sizeof(resp_buf) - off, "\r\n");

        /* snprintf returns the would-be length; if the header block was
         * truncated, clamp so send_all() doesn't read past resp_buf. */
        if (off < 0) off = 0;
        if (off > (int)sizeof(resp_buf)) off = (int)sizeof(resp_buf);

        /* Send headers — loop over partial writes, don't discard the count */
        if (send_all(client, resp_buf, (size_t)off)) {
            /* Send body (skip for HEAD) */
            if (request.method != HTTP_HEAD && response->body && response->body_length > 0) {
                send_all(client, response->body, response->body_length);
            }
        }

        http_response_free(response);
    }

    free_parsed_request(&request);
    socket_destroy(client);
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

/* ========================================================================
 * Server listen loop
 * ======================================================================== */

bool http_server_listen(HttpServer* server) {
    if (!server) return false;

    socket_system_init();

    server->listener = socket_create(SOCKET_TYPE_TCP);
    if (!server->listener) return false;

    /* Allow port reuse for fast server restarts */
    int reuse = 1;
    socket_set_option(server->listener, SOL_SOCKET, SO_REUSEADDR,
                      &reuse, sizeof(reuse));

    if (!socket_bind(server->listener, server->bind_addr, server->port)) {
        fprintf(stderr, "Failed to bind to %s:%d\n", server->bind_addr, server->port);
        socket_destroy(server->listener);
        server->listener = NULL;
        return false;
    }

    if (!socket_listen(server->listener, 128)) {
        fprintf(stderr, "Failed to listen\n");
        socket_destroy(server->listener);
        server->listener = NULL;
        return false;
    }

    printf("HTTP Server listening on %s:%d (Concurrent)\n", server->bind_addr, server->port);
    server->running = true;

    while (server->running) {
        Socket* client = socket_accept(server->listener);
        if (client) {
            ClientContext* ctx = malloc(sizeof(ClientContext));
            ctx->server = server;
            ctx->client = client;
            
            // Dispatch to worker pool
            if (!pool_submit(server->pool, handle_client_task, ctx)) {
                socket_destroy(client);
                free(ctx);
            }
        }
    }

    socket_system_cleanup();
    return true;
}

void http_server_stop(HttpServer* server) {
    if (!server) return;
    server->running = false;
}

/* ========================================================================
 * Response helpers
 * ======================================================================== */

HttpResponse* http_response_ok(const char* body) {
    HttpResponse* response = (HttpResponse*)calloc(1, sizeof(HttpResponse));
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
