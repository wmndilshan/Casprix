/**
 * Casprix HTTP Server Framework
 * Simple routing and middleware support
 */

#ifndef CASPERIX_HTTP_SERVER_H
#define CASPERIX_HTTP_SERVER_H

#include "socket.h"
#include "http.h"
#include "../async/future.h"

// Forward declarations
typedef struct HttpServer HttpServer;
typedef struct HttpRoute HttpRoute;

// Route handler function
typedef Future* (*RouteHandler)(HttpRequest* request, void* user_data);
typedef HttpResponse* (*SyncRouteHandler)(HttpRequest* request, void* user_data);

typedef struct HttpServerConfig {
    size_t worker_count;       /* 0 selects a CPU-derived default. */
    int backlog;               /* Pending kernel accept queue. */
    int io_timeout_ms;         /* Per-connection send/receive timeout. */
    size_t max_request_body;   /* Reject larger Content-Length values. */
} HttpServerConfig;

typedef struct HttpServerStats {
    uint64_t accepted_connections;
    uint64_t completed_connections;
    uint64_t rejected_connections;
    uint64_t malformed_requests;
    uint64_t active_connections;
} HttpServerStats;

// Middleware function
typedef bool (*Middleware)(HttpRequest* request, HttpResponse* response, void* user_data);

// HTTP Route
struct HttpRoute {
    HttpMethod method;
    char* path;
    RouteHandler handler;
    SyncRouteHandler sync_handler;
    void* user_data;
    HttpRoute* next;
};

// HTTP Server
struct HttpServer {
    Socket* listener;
    char* bind_addr;
    uint16_t port;
    HttpRoute* routes;
    Middleware* middlewares;
    int middleware_count;
    void* async_runtime;      /* Shared scheduler + event-loop integration. */
    volatile long running;
    bool socket_initialized;
    HttpServerConfig config;
    volatile uint64_t accepted_connections;
    volatile uint64_t completed_connections;
    volatile uint64_t rejected_connections;
    volatile uint64_t malformed_requests;
    volatile uint64_t active_connections;
};

// Create HTTP server
HttpServer* http_server_create(const char* bind_addr, uint16_t port);
HttpServer* http_server_create_with_config(const char* bind_addr, uint16_t port,
                                           const HttpServerConfig* config);
HttpServerConfig http_server_default_config(void);

// Destroy server
void http_server_destroy(HttpServer* server);

// Add route
bool http_server_route(HttpServer* server, HttpMethod method, const char* path, RouteHandler handler, void* user_data);
bool http_server_route_sync(HttpServer* server, HttpMethod method, const char* path,
                            SyncRouteHandler handler, void* user_data);

// Add middleware
bool http_server_use(HttpServer* server, Middleware middleware, void* user_data);

// Start server (blocking)
bool http_server_listen(HttpServer* server);

// Stop server
void http_server_stop(HttpServer* server);
bool http_server_is_running(const HttpServer* server);
HttpServerStats http_server_get_stats(const HttpServer* server);

// Handle a single already-accepted connection to completion (read request,
// route, send response, close). Synchronous; normally called by the listen
// loop's worker threads, but exposed for tests that drive a loopback socket.
void http_server_handle_connection(HttpServer* server, Socket* client);

// Helper functions for route handlers
HttpResponse* http_response_ok(const char* body);
HttpResponse* http_response_json(const char* json);
HttpResponse* http_response_error(int status_code, const char* message);

/* -------------------------------------------------------------------------
 * Synchronous route table (method + path → handler), for tests and thin apps
 * ------------------------------------------------------------------------- */

typedef void (*CpxSyncHttpHandler)(HttpRequest* req, HttpResponse* res, void* userdata);

typedef struct CpxRoute {
    char               method[8];
    char               path[256];
    CpxSyncHttpHandler handler;
    void*              userdata;
} CpxRoute;

typedef struct CpxRouter {
    CpxRoute routes[64];
    int      route_count;
} CpxRouter;

void cpx_router_init(CpxRouter* r);
void cpx_router_add(CpxRouter* r, const char* method, const char* path,
                    CpxSyncHttpHandler handler, void* userdata);
void cpx_router_dispatch(CpxRouter* r, HttpRequest* req, HttpResponse* res);

#endif // CASPERIX_HTTP_SERVER_H
