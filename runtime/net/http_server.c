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

static bool parse_request(const char* raw, int raw_len, HttpRequest* req) {
    memset(req, 0, sizeof(*req));

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
        size_t body_len = (size_t)(raw_len - (body_start - raw));
        if (body_len > 0) {
            req->body = (char*)malloc(body_len + 1);
            memcpy(req->body, body_start, body_len);
            req->body[body_len] = '\0';
            req->body_length = body_len;
        }
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
 * Client handler (Coroutine Entry)
 * ======================================================================== */

typedef struct {
    HttpServer* server;
    Socket* client;
} ClientContext;

static void handle_client_coro(void* arg) {
    ClientContext* ctx = (ClientContext*)arg;
    HttpServer* server = ctx->server;
    Socket* client = ctx->client;
    free(ctx);

    char buffer[8192];
    int total = 0;

    /* Read request (may arrive in multiple recv calls) */
    while (total < (int)sizeof(buffer) - 1) {
        int received = socket_recv(client, buffer + total, (int)sizeof(buffer) - 1 - total);
        if (received <= 0) break;
        total += received;
        buffer[total] = '\0';
        /* Check if we've received the full headers */
        if (strstr(buffer, "\r\n\r\n")) break;
    }

    if (total <= 0) {
        socket_destroy(client);
        return;
    }

    /* Parse the request */
    HttpRequest request;
    if (!parse_request(buffer, total, &request)) {
        /* Malformed request — 400 */
        const char* bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 11\r\nConnection: close\r\n\r\nBad Request";
        socket_send(client, bad, (int)strlen(bad));
        socket_destroy(client);
        return;
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

        /* Send headers */
        socket_send(client, resp_buf, off);

        /* Send body (skip for HEAD) */
        if (request.method != HTTP_HEAD && response->body && response->body_length > 0) {
            socket_send(client, response->body, (int)response->body_length);
        }

        http_response_free(response);
    }

    free_parsed_request(&request);
    socket_destroy(client);
}

static void* handle_client_task(void* arg) {
    ClientContext* ctx = (ClientContext*)arg;
    
    // Create a coroutine for this client
    Coroutine* coro = coro_create(handle_client_coro, ctx, 0);
    if (coro) {
        // Run it immediately (in a real system, we'd add it to a run queue)
        coro_switch(coro_current(), coro);
        coro_destroy(coro);
    } else {
        socket_destroy(ctx->client);
        free(ctx);
    }
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
