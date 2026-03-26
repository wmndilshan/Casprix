/**
 * Casperix Runtime - HTTP Client/Server
 * Simple HTTP implementation for async I/O
 */

#ifndef CASPERIX_HTTP_H
#define CASPERIX_HTTP_H

#include "socket.h"
#include <stdint.h>
#include <stdbool.h>

// HTTP Methods
typedef enum {
    HTTP_GET,
    HTTP_POST,
    HTTP_PUT,
    HTTP_DELETE,
    HTTP_HEAD
} HttpMethod;

// HTTP Request
typedef struct {
    HttpMethod method;
    char* url;
    char* path;
    char* headers[32];
    int header_count;
    char* body;
    size_t body_length;
} HttpRequest;

// HTTP Response
typedef struct {
    int status_code;
    char* status_text;
    char* headers[32];
    int header_count;
    char* body;
    size_t body_length;
} HttpResponse;

// HTTP Client
typedef struct {
    char* host;
    uint16_t port;
    Socket* socket;
    bool keep_alive;
} HttpClient;

// Create HTTP client
HttpClient* http_client_create(const char* host, uint16_t port);

// Destroy HTTP client
void http_client_destroy(HttpClient* client);

// Send HTTP request
bool http_client_send_request(HttpClient* client, const HttpRequest* request);

// Receive HTTP response
HttpResponse* http_client_recv_response(HttpClient* client);

// Simple GET request
HttpResponse* http_get(const char* url);

// Simple POST request
HttpResponse* http_post(const char* url, const char* body, size_t body_length);

// Free response
void http_response_free(HttpResponse* response);

// HTTP Request helpers
HttpRequest* http_request_create(HttpMethod method, const char* path);
void http_request_add_header(HttpRequest* req, const char* key, const char* value);
void http_request_set_body(HttpRequest* req, const char* body, size_t length);
void http_request_free(HttpRequest* req);

// Parse HTTP response from buffer
HttpResponse* http_parse_response(const char* buffer, size_t length);

#endif // CASPERIX_HTTP_H
