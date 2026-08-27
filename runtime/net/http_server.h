/**
 * Casperix HTTP Server Framework
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

// Middleware function
typedef bool (*Middleware)(HttpRequest* request, HttpResponse* response, void* user_data);

// HTTP Route
struct HttpRoute {
    HttpMethod method;
    char* path;
    RouteHandler handler;
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
    void* pool;
    bool running;
};

// Create HTTP server
HttpServer* http_server_create(const char* bind_addr, uint16_t port);

// Destroy server
void http_server_destroy(HttpServer* server);

// Add route
bool http_server_route(HttpServer* server, HttpMethod method, const char* path, RouteHandler handler, void* user_data);

// Add middleware
bool http_server_use(HttpServer* server, Middleware middleware, void* user_data);

// Start server (blocking)
bool http_server_listen(HttpServer* server);

// Stop server
void http_server_stop(HttpServer* server);

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
