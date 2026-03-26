/**
 * Casperix HTTP Server Example
 * Demonstrates routing and request handling
 */

#include "runtime/net/http_server.h"
#include "runtime/async/future.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Route handler for /
Future* handle_index(HttpRequest* request, void* user_data) {
    (void)request;
    (void)user_data;
    
    Future* future = future_create();
    HttpResponse* response = http_response_ok("Welcome to Casperix HTTP Server!");
    future_complete(future, response);
    return future;
}

// Route handler for /api/hello
Future* handle_hello(HttpRequest* request, void* user_data) {
    (void)request;
    (void)user_data;
    
    Future* future = future_create();
    HttpResponse* response = http_response_json(
        "{\"message\":\"Hello from Casperix!\",\"version\":\"1.0\"}"
    );
    future_complete(future, response);
    return future;
}

// Route handler for /api/echo
Future* handle_echo(HttpRequest* request, void* user_data) {
    (void)user_data;
    
    Future* future = future_create();
    
    char response_body[512];
    snprintf(response_body, sizeof(response_body),
            "{\"method\":\"%s\",\"path\":\"%s\"}",
            request->method == HTTP_GET ? "GET" :
            request->method == HTTP_POST ? "POST" : "OTHER",
            request->path);
    
    HttpResponse* response = http_response_json(response_body);
    future_complete(future, response);
    return future;
}

int main(void) {
    printf("╔════════════════════════════════════════╗\n");
    printf("║  Casperix HTTP Server Demo            ║\n");
    printf("╚════════════════════════════════════════╝\n\n");
    
    // Create server
    HttpServer* server = http_server_create("127.0.0.1", 8080);
    if (!server) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }
    
    // Register routes
    http_server_route(server, HTTP_GET, "/", handle_index, NULL);
    http_server_route(server, HTTP_GET, "/api/hello", handle_hello, NULL);
    http_server_route(server, HTTP_GET, "/api/echo", handle_echo, NULL);
    http_server_route(server, HTTP_POST, "/api/echo", handle_echo, NULL);
    
    printf("Registered routes:\n");
    printf("  GET  /\n");
    printf("  GET  /api/hello\n");
    printf("  GET  /api/echo\n");
    printf("  POST /api/echo\n\n");
    
    // Start server (blocking)
    http_server_listen(server);
    
    // Cleanup
    http_server_destroy(server);
    
    return 0;
}
