#include "../runtime.h"
#include "../../include/casprix/stdlib.h"
#include "../../include/casprix/network.h"
#include "../../include/casprix/http.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

// ============================================================================
// HTTP Request/Response Structures
// ============================================================================

typedef struct {
    char method[16];
    char host[256];
    char path[1024];
    int port;
    char** headers;
    int header_count;
    char* body;
    int body_length;
    int timeout_ms;
} nuwan_http_request_t;

typedef struct {
    int status_code;
    char status_message[128];
    char** headers;
    int header_count;
    char* body;
    int body_length;
} nuwan_http_response_t;

// ============================================================================
// HTTP Utility Functions
// ============================================================================

static void parse_url(const char* url, char* host, int* port, char* path) {
    const char* proto_end = strstr(url, "://");
    const char* start = proto_end ? proto_end + 3 : url;

    // Default port
    *port = 80;
    if (proto_end && strncmp(url, "https", 5) == 0) {
        *port = 443;
    }

    // Find path separator
    const char* path_start = strchr(start, '/');
    const char* port_start = strchr(start, ':');

    // Extract host
    int host_len;
    if (port_start && (!path_start || port_start < path_start)) {
        host_len = port_start - start;
        *port = atoi(port_start + 1);
    } else if (path_start) {
        host_len = path_start - start;
    } else {
        host_len = strlen(start);
    }

    strncpy(host, start, host_len);
    host[host_len] = '\0';

    // Extract path
    if (path_start) {
        strcpy(path, path_start);
    } else {
        strcpy(path, "/");
    }
}

static char* build_http_request(const nuwan_http_request_t* req) {
    // Calculate total size needed
    int size = 0;
    size += strlen(req->method) + 1;  // "GET "
    size += strlen(req->path) + 1;    // "/path "
    size += 11;                        // "HTTP/1.1\r\n"
    size += 6 + strlen(req->host) + 2; // "Host: host\r\n"

    // Add headers
    for (int i = 0; i < req->header_count; i++) {
        size += strlen(req->headers[i]) + 2; // header\r\n
    }

    // Add default headers if not present
    size += 24; // "Connection: close\r\n"

    if (req->body && req->body_length > 0) {
        size += 40; // "Content-Length: XXXXX\r\n"
        size += 2;  // "\r\n"
        size += req->body_length;
    } else {
        size += 2;  // "\r\n"
    }

    size += 100; // Extra buffer

    char* request = (char*)malloc(size);
    if (!request) return NULL;

    // Build request line
    sprintf(request, "%s %s HTTP/1.1\r\n", req->method, req->path);

    // Add Host header
    sprintf(request + strlen(request), "Host: %s\r\n", req->host);

    // Add custom headers
    for (int i = 0; i < req->header_count; i++) {
        strcat(request, req->headers[i]);
        strcat(request, "\r\n");
    }

    // Add Connection header
    strcat(request, "Connection: close\r\n");

    // Add Content-Length if body exists
    if (req->body && req->body_length > 0) {
        sprintf(request + strlen(request), "Content-Length: %d\r\n", req->body_length);
        strcat(request, "\r\n");
        memcpy(request + strlen(request), req->body, req->body_length);
        request[strlen(request) + req->body_length] = '\0';
    } else {
        strcat(request, "\r\n");
    }

    return request;
}

static nuwan_http_response_t* parse_http_response(const char* response, int response_len) {
    nuwan_http_response_t* resp = (nuwan_http_response_t*)calloc(1, sizeof(nuwan_http_response_t));
    if (!resp) return NULL;

    // Parse status line
    const char* line_end = strstr(response, "\r\n");
    if (!line_end) {
        free(resp);
        return NULL;
    }

    // Extract status code (e.g., "HTTP/1.1 200 OK")
    sscanf(response, "HTTP/%*s %d", &resp->status_code);

    const char* status_msg_start = strchr(response, ' ');
    if (status_msg_start) {
        status_msg_start = strchr(status_msg_start + 1, ' ');
        if (status_msg_start) {
            status_msg_start++;
            int msg_len = line_end - status_msg_start;
            if (msg_len < sizeof(resp->status_message)) {
                strncpy(resp->status_message, status_msg_start, msg_len);
                resp->status_message[msg_len] = '\0';
            }
        }
    }

    // Find header/body separator
    const char* header_end = strstr(response, "\r\n\r\n");
    if (!header_end) {
        free(resp);
        return NULL;
    }

    // Parse headers (simplified - count them)
    const char* header_start = line_end + 2;
    resp->header_count = 0;
    const char* pos = header_start;
    while (pos < header_end) {
        const char* next_line = strstr(pos, "\r\n");
        if (next_line) {
            resp->header_count++;
            pos = next_line + 2;
        } else {
            break;
        }
    }

    // Allocate header array (we'll store them as strings)
    if (resp->header_count > 0) {
        resp->headers = (char**)calloc(resp->header_count, sizeof(char*));
        pos = header_start;
        int idx = 0;
        while (pos < header_end && idx < resp->header_count) {
            const char* next_line = strstr(pos, "\r\n");
            if (next_line) {
                int header_len = next_line - pos;
                resp->headers[idx] = (char*)malloc(header_len + 1);
                strncpy(resp->headers[idx], pos, header_len);
                resp->headers[idx][header_len] = '\0';
                pos = next_line + 2;
                idx++;
            } else {
                break;
            }
        }
    }

    // Extract body
    const char* body_start = header_end + 4;
    resp->body_length = response_len - (body_start - response);
    if (resp->body_length > 0) {
        resp->body = (char*)malloc(resp->body_length + 1);
        memcpy(resp->body, body_start, resp->body_length);
        resp->body[resp->body_length] = '\0';
    }

    return resp;
}

// ============================================================================
// HTTP Client API
// ============================================================================

nuwan_http_response_t* nuwan_http_request(const char* method, const char* url,
                                          const char* body, int body_len,
                                          char** headers, int header_count) {
    // Parse URL
    nuwan_http_request_t req;
    memset(&req, 0, sizeof(req));

    strncpy(req.method, method, sizeof(req.method) - 1);
    parse_url(url, req.host, &req.port, req.path);
    req.headers = headers;
    req.header_count = header_count;
    req.body = (char*)body;
    req.body_length = body_len;
    req.timeout_ms = 30000;

    // Create socket
    nuwan_socket_t* sock = nuwan_socket_new(NUWAN_SOCKET_TCP);
    if (!sock) {
        return NULL;
    }

    // Set timeout
    nuwan_socket_set_timeout(sock, req.timeout_ms);

    // Connect to server
    if (!nuwan_socket_connect_ex(sock, req.host, req.port)) {
        nuwan_socket_destroy(sock);
        return NULL;
    }

    // Build and send request
    char* request_str = build_http_request(&req);
    if (!request_str) {
        nuwan_socket_destroy(sock);
        return NULL;
    }

    int sent = nuwan_socket_send_ex(sock, request_str, strlen(request_str));
    free(request_str);

    if (sent <= 0) {
        nuwan_socket_destroy(sock);
        return NULL;
    }

    // Receive response (simplified - read until connection closes)
    int buffer_size = 8192;
    int total_received = 0;
    char* response_buffer = (char*)malloc(buffer_size);
    if (!response_buffer) {
        nuwan_socket_destroy(sock);
        return NULL;
    }

    while (1) {
        int received = nuwan_socket_recv_ex(sock, response_buffer + total_received,
                                           buffer_size - total_received - 1);
        if (received <= 0) {
            break;
        }

        total_received += received;

        // Expand buffer if needed
        if (total_received >= buffer_size - 1024) {
            buffer_size *= 2;
            char* new_buffer = (char*)realloc(response_buffer, buffer_size);
            if (!new_buffer) {
                free(response_buffer);
                nuwan_socket_destroy(sock);
                return NULL;
            }
            response_buffer = new_buffer;
        }
    }

    response_buffer[total_received] = '\0';

    // Parse response
    nuwan_http_response_t* response = parse_http_response(response_buffer, total_received);

    free(response_buffer);
    nuwan_socket_destroy(sock);

    return response;
}

char* nuwan_http_get(const char* url) {
    nuwan_http_response_t* response = nuwan_http_request("GET", url, NULL, 0, NULL, 0);
    if (!response) {
        return NULL;
    }

    char* body = response->body ? strdup(response->body) : NULL;

    // Free response
    nuwan_http_response_free(response);

    return body;
}

char* nuwan_http_post(const char* url, const char* body) {
    int body_len = body ? strlen(body) : 0;
    nuwan_http_response_t* response = nuwan_http_request("POST", url, body, body_len, NULL, 0);
    if (!response) {
        return NULL;
    }

    char* resp_body = response->body ? strdup(response->body) : NULL;

    // Free response
    nuwan_http_response_free(response);

    return resp_body;
}

int nuwan_http_response_get_status(nuwan_http_response_t* response) {
    return response ? response->status_code : -1;
}

const char* nuwan_http_response_get_body(nuwan_http_response_t* response) {
    return response ? response->body : NULL;
}

const char* nuwan_http_response_get_header(nuwan_http_response_t* response, const char* name) {
    if (!response || !name) return NULL;

    int name_len = strlen(name);
    for (int i = 0; i < response->header_count; i++) {
        if (strncmp(response->headers[i], name, name_len) == 0 &&
            response->headers[i][name_len] == ':') {
            // Skip colon and spaces
            const char* value = response->headers[i] + name_len + 1;
            while (*value == ' ') value++;
            return value;
        }
    }

    return NULL;
}

void nuwan_http_response_free(nuwan_http_response_t* response) {
    if (!response) return;

    if (response->headers) {
        for (int i = 0; i < response->header_count; i++) {
            free(response->headers[i]);
        }
        free(response->headers);
    }

    if (response->body) {
        free(response->body);
    }

    free(response);
}

// ============================================================================
// HTTP Client Class Support (for OOP interface)
// ============================================================================

typedef struct {
    char base_url[512];
    int timeout_ms;
    int max_retries;
    char** headers;
    int header_count;
} nuwan_http_client_t;

nuwan_http_client_t* nuwan_http_client_new() {
    nuwan_http_client_t* client = (nuwan_http_client_t*)calloc(1, sizeof(nuwan_http_client_t));
    if (!client) return NULL;

    client->timeout_ms = 30000;
    client->max_retries = 3;

    return client;
}

void nuwan_http_client_set_base_url(nuwan_http_client_t* client, const char* url) {
    if (!client || !url) return;
    strncpy(client->base_url, url, sizeof(client->base_url) - 1);
}

void nuwan_http_client_set_timeout(nuwan_http_client_t* client, int milliseconds) {
    if (!client) return;
    client->timeout_ms = milliseconds;
}

void nuwan_http_client_add_header(nuwan_http_client_t* client, const char* header) {
    if (!client || !header) return;

    // Reallocate header array
    char** new_headers = (char**)realloc(client->headers,
                                         (client->header_count + 1) * sizeof(char*));
    if (!new_headers) return;

    client->headers = new_headers;
    client->headers[client->header_count] = strdup(header);
    client->header_count++;
}

char* nuwan_http_client_get(nuwan_http_client_t* client, const char* path) {
    if (!client || !path) return NULL;

    char full_url[1024];
    if (strlen(client->base_url) > 0) {
        snprintf(full_url, sizeof(full_url), "%s%s", client->base_url, path);
    } else {
        strncpy(full_url, path, sizeof(full_url) - 1);
    }

    return nuwan_http_get(full_url);
}

char* nuwan_http_client_post(nuwan_http_client_t* client, const char* path, const char* body) {
    if (!client || !path) return NULL;

    char full_url[1024];
    if (strlen(client->base_url) > 0) {
        snprintf(full_url, sizeof(full_url), "%s%s", client->base_url, path);
    } else {
        strncpy(full_url, path, sizeof(full_url) - 1);
    }

    return nuwan_http_post(full_url, body);
}

void nuwan_http_client_free(nuwan_http_client_t* client) {
    if (!client) return;

    if (client->headers) {
        for (int i = 0; i < client->header_count; i++) {
            free(client->headers[i]);
        }
        free(client->headers);
    }

    free(client);
}
