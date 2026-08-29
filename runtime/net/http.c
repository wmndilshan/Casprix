/**
 * Casperix Runtime - HTTP Implementation
 */

#include "http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
    HttpClient* client = (HttpClient*)calloc(1, sizeof(HttpClient));
    if (!client) return NULL;
    
    client->host = strdup(host);
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
    }
    
    // Build request string
    char buffer[8192];
    int offset = 0;
    
    // Request line
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                      "%s %s HTTP/1.1\r\n",
                      method_to_string(request->method),
                      request->path);
    
    // Host header
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                      "Host: %s\r\n", client->host);
    
    // User headers
    for (int i = 0; i < request->header_count; i++) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                          "%s\r\n", request->headers[i]);
    }
    
    // Content-Length if body exists
    if (request->body && request->body_length > 0) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                          "Content-Length: " FMT_ZU "\r\n", CAST_ZU(request->body_length));
    }
    
    // End headers
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\r\n");
    
    // Send headers
    if (!http_send_all(client->socket, buffer, (size_t)offset)) {
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

HttpResponse* http_client_recv_response(HttpClient* client) {
    if (!client || !client->socket) return NULL;
    
    char buffer[16384];
    int received = socket_recv(client->socket, buffer, sizeof(buffer) - 1);
    if (received <= 0) return NULL;
    
    buffer[received] = '\0';
    return http_parse_response(buffer, received);
}

HttpResponse* http_parse_response(const char* buffer, size_t length) {
    if (!buffer || length == 0) return NULL;
    
    HttpResponse* response = (HttpResponse*)calloc(1, sizeof(HttpResponse));
    if (!response) return NULL;
    
    // Parse status line
    if (sscanf(buffer, "HTTP/1.%*d %d", &response->status_code) != 1) {
        free(response);
        return NULL;
    }
    
    // Find body (after \r\n\r\n)
    const char* body_start = strstr(buffer, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        size_t body_len = length - (body_start - buffer);
        response->body = (char*)malloc(body_len + 1);
        memcpy(response->body, body_start, body_len);
        response->body[body_len] = '\0';
        response->body_length = body_len;
    }
    
    return response;
}

HttpResponse* http_get(const char* url) {
    // Simple URL parsing (http://host:port/path)
    char host[256] = {0};
    char path[1024] = "/";
    uint16_t port = 80;
    
    if (strncmp(url, "http://", 7) == 0) {
        const char* start = url + 7;
        const char* slash = strchr(start, '/');
        const char* colon = strchr(start, ':');
        
        if (colon && (!slash || colon < slash)) {
            size_t host_len = colon - start;
            strncpy(host, start, host_len);
            port = atoi(colon + 1);
            if (slash) strcpy(path, slash);
        } else if (slash) {
            size_t host_len = slash - start;
            strncpy(host, start, host_len);
            strcpy(path, slash);
        } else {
            strcpy(host, start);
        }
    }
    
    HttpClient* client = http_client_create(host, port);
    if (!client) return NULL;
    
    HttpRequest* request = http_request_create(HTTP_GET, path);
    bool success = http_client_send_request(client, request);
    
    HttpResponse* response = NULL;
    if (success) {
        response = http_client_recv_response(client);
    }
    
    http_request_free(request);
    http_client_destroy(client);
    
    return response;
}

HttpResponse* http_post(const char* url, const char* body, size_t body_length) {
    char host[256] = {0};
    char path[1024] = "/";
    uint16_t port = 80;

    if (strncmp(url, "http://", 7) == 0) {
        const char* start = url + 7;
        const char* slash = strchr(start, '/');
        const char* colon = strchr(start, ':');

        if (colon && (!slash || colon < slash)) {
            size_t host_len = (size_t)(colon - start);
            if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
            strncpy(host, start, host_len);
            port = (uint16_t)atoi(colon + 1);
            if (slash) snprintf(path, sizeof(path), "%s", slash);
        } else if (slash) {
            size_t host_len = (size_t)(slash - start);
            if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
            strncpy(host, start, host_len);
            snprintf(path, sizeof(path), "%s", slash);
        } else {
            snprintf(host, sizeof(host), "%s", start);
        }
    }

    HttpClient* client = http_client_create(host, port);
    if (!client) return NULL;

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
    if (!req) return;
    
    free(req->body);
    req->body = (char*)malloc(length);
    memcpy(req->body, body, length);
    req->body_length = length;
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
