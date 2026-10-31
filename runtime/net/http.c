/**
 * Casprix Runtime - HTTP Implementation
 */

#include "http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* MinGW printf doesn't support %zu */
#ifdef __MINGW32__
#define FMT_ZU "%llu"
#define CAST_ZU(x) ((unsigned long long)(x))
#else
#define FMT_ZU "%zu"
#define CAST_ZU(x) (x)
#endif

/*
 * send(2) may write fewer bytes than requested. The old `socket_send(...) < 0`
 * check let a short write through as success, truncating the request on the
 * wire. Loop until the whole buffer is sent or the connection fails.
 */
static bool http_send_all(Socket* sock, const void* buf, size_t len) {
    const char* p = (const char*)buf;
    size_t sent = 0;
    while (sent < len) {
        int n = socket_send(sock, p + sent, len - sent);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static bool http_append(char* buffer, size_t capacity, size_t* used,
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

static const char* method_to_string(HttpMethod method) {
    switch (method) {
        case HTTP_GET: return "GET";
        case HTTP_POST: return "POST";
        case HTTP_PUT: return "PUT";
        case HTTP_DELETE: return "DELETE";
        case HTTP_HEAD: return "HEAD";
        default: return "GET";
    }
}

HttpClient* http_client_create(const char* host, uint16_t port) {
    if (!host || !*host) return NULL;
    HttpClient* client = (HttpClient*)calloc(1, sizeof(HttpClient));
    if (!client) return NULL;
    
    client->host = strdup(host);
    if (!client->host) {
        free(client);
        return NULL;
    }
    client->port = port;
    client->keep_alive = true;
    client->socket = NULL;
    
    return client;
}

void http_client_destroy(HttpClient* client) {
    if (!client) return;
    
    free(client->host);
    if (client->socket) {
        socket_destroy(client->socket);
    }
    free(client);
}

bool http_client_send_request(HttpClient* client, const HttpRequest* request) {
    if (!client || !request) return false;
    
    // Create socket if not connected
    if (!client->socket) {
        client->socket = socket_create(SOCKET_TYPE_TCP);
        if (!client->socket) return false;
        
        if (!socket_connect(client->socket, client->host, client->port)) {
            socket_destroy(client->socket);
            client->socket = NULL;
            return false;
        }
        socket_set_timeout(client->socket, SOCKET_DEFAULT_IO_TIMEOUT_MS);
    }
    
    // Build request string
    char buffer[8192];
    size_t offset = 0;
    bool valid = true;
    
    // Request line
    valid = http_append(buffer, sizeof(buffer), &offset, "%s %s HTTP/1.1\r\n",
                        method_to_string(request->method),
                        request->path ? request->path : "/");
    
    // Host header
    if (valid) valid = http_append(buffer, sizeof(buffer), &offset,
                                   "Host: %s\r\n", client->host);
    
    // User headers
    for (int i = 0; i < request->header_count; i++) {
        if (valid && request->headers[i]) {
            valid = http_append(buffer, sizeof(buffer), &offset,
                                "%s\r\n", request->headers[i]);
        }
    }
    
    // Content-Length if body exists
    if (request->body && request->body_length > 0) {
        if (valid) valid = http_append(buffer, sizeof(buffer), &offset,
                                       "Content-Length: " FMT_ZU "\r\n",
                                       CAST_ZU(request->body_length));
    }
    
    // End headers
    if (valid) valid = http_append(buffer, sizeof(buffer), &offset,
                                   "Connection: close\r\n\r\n");
    if (!valid) return false;
    
    // Send headers
    if (!http_send_all(client->socket, buffer, offset)) {
        return false;
    }

    // Send body if exists
    if (request->body && request->body_length > 0) {
        if (!http_send_all(client->socket, request->body, request->body_length)) {
            return false;
        }
    }
    
    return true;
}

static bool parse_http_url(const char* url, char* host, size_t host_capacity,
                           uint16_t* port, char* path, size_t path_capacity) {
    if (!url || strncmp(url, "http://", 7) != 0) return false;
    const char* authority = url + 7;
    const char* slash = strchr(authority, '/');
    const char* authority_end = slash ? slash : authority + strlen(authority);
    const char* colon = NULL;
    for (const char* cursor = authority; cursor < authority_end; cursor++) {
        if (*cursor == ':') colon = cursor;
    }
    const char* host_end = colon ? colon : authority_end;
    size_t host_length = (size_t)(host_end - authority);
    if (host_length == 0 || host_length >= host_capacity) return false;
    memcpy(host, authority, host_length);
    host[host_length] = '\0';
    *port = 80;
    if (colon) {
        char* end = NULL;
        long parsed = strtol(colon + 1, &end, 10);
        if (end != authority_end || parsed < 1 || parsed > 65535) return false;
        *port = (uint16_t)parsed;
    }
    const char* source_path = slash ? slash : "/";
    size_t path_length = strlen(source_path);
    if (path_length >= path_capacity) return false;
    memcpy(path, source_path, path_length + 1);
    return true;
}

HttpResponse* http_client_recv_response(HttpClient* client) {
    if (!client || !client->socket) return NULL;

    const size_t maximum = 16u * 1024u * 1024u;
    size_t capacity = 16384;
    size_t used = 0;
    char* buffer = (char*)malloc(capacity + 1);
    if (!buffer) return NULL;
    for (;;) {
        if (used == capacity) {
            if (capacity >= maximum) {
                free(buffer);
                return NULL;
            }
            size_t next_capacity = capacity * 2;
            if (next_capacity > maximum) next_capacity = maximum;
            char* next = (char*)realloc(buffer, next_capacity + 1);
            if (!next) {
                free(buffer);
                return NULL;
            }
            buffer = next;
            capacity = next_capacity;
        }
        int received = socket_recv(client->socket, buffer + used, capacity - used);
        if (received < 0) {
            free(buffer);
            return NULL;
        }
        if (received == 0) break;
        used += (size_t)received;
    }
    buffer[used] = '\0';
    HttpResponse* response = http_parse_response(buffer, used);
    free(buffer);
    return response;
}

HttpResponse* http_parse_response(const char* buffer, size_t length) {
    if (!buffer || length == 0) return NULL;
    char* owned = (char*)malloc(length + 1);
    if (!owned) return NULL;
    memcpy(owned, buffer, length);
    owned[length] = '\0';
    
    HttpResponse* response = (HttpResponse*)calloc(1, sizeof(HttpResponse));
    if (!response) {
        free(owned);
        return NULL;
    }
    
    // Parse status line
    if (sscanf(owned, "HTTP/1.%*d %d", &response->status_code) != 1) {
        free(owned);
        free(response);
        return NULL;
    }
    
    // Find body (after \r\n\r\n)
    const char* body_start = strstr(owned, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        size_t body_len = length - (size_t)(body_start - owned);
        response->body = (char*)malloc(body_len + 1);
        if (!response->body) {
            free(owned);
            free(response);
            return NULL;
        }
        memcpy(response->body, body_start, body_len);
        response->body[body_len] = '\0';
        response->body_length = body_len;
    }
    free(owned);
    return response;
}

HttpResponse* http_get(const char* url) {
    char host[256] = {0};
    char path[1024] = "/";
    uint16_t port = 80;
    if (!parse_http_url(url, host, sizeof(host), &port, path, sizeof(path))) return NULL;
    if (!socket_system_init()) return NULL;
    
    HttpClient* client = http_client_create(host, port);
    if (!client) {
        socket_system_cleanup();
        return NULL;
    }
    
    HttpRequest* request = http_request_create(HTTP_GET, path);
    bool success = http_client_send_request(client, request);
    
    HttpResponse* response = NULL;
    if (success) {
        response = http_client_recv_response(client);
    }
    
    http_request_free(request);
    http_client_destroy(client);
    socket_system_cleanup();
    return response;
}

HttpResponse* http_post(const char* url, const char* body, size_t body_length) {
    char host[256] = {0};
    char path[1024] = "/";
    uint16_t port = 80;

    if (!parse_http_url(url, host, sizeof(host), &port, path, sizeof(path))) return NULL;
    if (!socket_system_init()) return NULL;

    HttpClient* client = http_client_create(host, port);
    if (!client) {
        socket_system_cleanup();
        return NULL;
    }

    HttpRequest* request = http_request_create(HTTP_POST, path);
    http_request_add_header(request, "Content-Type", "application/x-www-form-urlencoded");
    if (body && body_length > 0) {
        http_request_set_body(request, body, body_length);
    }

    bool success = http_client_send_request(client, request);
    HttpResponse* response = NULL;
    if (success) {
        response = http_client_recv_response(client);
    }

    http_request_free(request);
    http_client_destroy(client);
    socket_system_cleanup();
    return response;
}

HttpRequest* http_request_create(HttpMethod method, const char* path) {
    HttpRequest* req = (HttpRequest*)calloc(1, sizeof(HttpRequest));
    if (!req) return NULL;
    
    req->method = method;
    req->path = strdup(path ? path : "/");
    req->header_count = 0;
    req->body = NULL;
    req->body_length = 0;
    
    return req;
}

void http_request_add_header(HttpRequest* req, const char* key, const char* value) {
    if (!req || req->header_count >= 32) return;
    
    char header[512];
    snprintf(header, sizeof(header), "%s: %s", key, value);
    req->headers[req->header_count++] = strdup(header);
}

void http_request_set_body(HttpRequest* req, const char* body, size_t length) {
    if (!req || (!body && length > 0)) return;
    
    free(req->body);
    req->body = (char*)malloc(length);
    if (length == 0 || req->body) {
        if (length > 0) memcpy(req->body, body, length);
        req->body_length = length;
    }
}

void http_request_free(HttpRequest* req) {
    if (!req) return;
    
    free(req->path);
    free(req->body);
    for (int i = 0; i < req->header_count; i++) {
        free(req->headers[i]);
    }
    free(req);
}

void http_response_free(HttpResponse* response) {
    if (!response) return;
    
    free(response->status_text);
    free(response->body);
    for (int i = 0; i < response->header_count; i++) {
        free(response->headers[i]);
    }
    free(response);
}
