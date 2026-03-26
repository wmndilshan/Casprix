// Casprix HTTP Client Library
// Modern async HTTP client with fluent API

#ifndef NUWAN_HTTP_H
#define NUWAN_HTTP_H

#include "async.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct nuwan_http_client nuwan_http_client_t;
typedef struct nuwan_http_response nuwan_http_response_t;
typedef struct nuwan_http_header nuwan_http_header_t;

// HTTP methods
typedef enum {
    HTTP_GET,
    HTTP_POST,
    HTTP_PUT,
    HTTP_DELETE,
    HTTP_PATCH,
    HTTP_HEAD,
    HTTP_OPTIONS
} nuwan_http_method_t;

// HTTP header
struct nuwan_http_header {
    char* key;
    char* value;
    struct nuwan_http_header* next;
};

// HTTP response
struct nuwan_http_response {
    int status_code;
    char* status_message;
    char* body;
    size_t body_length;
    nuwan_http_header_t* headers;
};

// HTTP client
struct nuwan_http_client {
    char* base_url;
    int timeout_ms;
    int max_retries;
    nuwan_http_header_t* default_headers;
    void* connection_pool;
};

// HTTP Client API
nuwan_http_client_t* nuwan_http_client_create(void);
void nuwan_http_client_set_base_url(nuwan_http_client_t* client, const char* url);
void nuwan_http_client_set_timeout(nuwan_http_client_t* client, int milliseconds);
void nuwan_http_client_set_retries(nuwan_http_client_t* client, int count);
void nuwan_http_client_add_header(nuwan_http_client_t* client, const char* key, const char* value);

// Async requests (return Promise)
nuwan_promise_t* nuwan_http_get_async(nuwan_http_client_t* client, const char* path);
nuwan_promise_t* nuwan_http_post_async(nuwan_http_client_t* client, const char* path, const char* body);
nuwan_promise_t* nuwan_http_put_async(nuwan_http_client_t* client, const char* path, const char* body);
nuwan_promise_t* nuwan_http_delete_async(nuwan_http_client_t* client, const char* path);

// Synchronous requests
nuwan_http_response_t* nuwan_http_get_sync(nuwan_http_client_t* client, const char* path);
nuwan_http_response_t* nuwan_http_post_sync(nuwan_http_client_t* client, const char* path, const char* body);

// Response API
int nuwan_http_response_get_status(nuwan_http_response_t* response);
const char* nuwan_http_response_get_body(nuwan_http_response_t* response);
const char* nuwan_http_response_get_header(nuwan_http_response_t* response, const char* key);
void nuwan_http_response_destroy(nuwan_http_response_t* response);

// Client cleanup
void nuwan_http_client_destroy(nuwan_http_client_t* client);

// WebSocket support
typedef struct nuwan_websocket nuwan_websocket_t;

typedef void (*nuwan_websocket_message_callback_t)(nuwan_websocket_t* ws, const char* message, size_t length);
typedef void (*nuwan_websocket_close_callback_t)(nuwan_websocket_t* ws, int code, const char* reason);
typedef void (*nuwan_websocket_error_callback_t)(nuwan_websocket_t* ws, const char* error);

struct nuwan_websocket {
    char* url;
    bool is_connected;
    void* socket_handle;
    nuwan_websocket_message_callback_t on_message;
    nuwan_websocket_close_callback_t on_close;
    nuwan_websocket_error_callback_t on_error;
    void* user_data;
};

// WebSocket API
nuwan_websocket_t* nuwan_websocket_create(const char* url);
nuwan_promise_t* nuwan_websocket_connect_async(nuwan_websocket_t* ws);
bool nuwan_websocket_send(nuwan_websocket_t* ws, const char* message);
void nuwan_websocket_set_on_message(nuwan_websocket_t* ws, nuwan_websocket_message_callback_t callback);
void nuwan_websocket_set_on_close(nuwan_websocket_t* ws, nuwan_websocket_close_callback_t callback);
void nuwan_websocket_set_on_error(nuwan_websocket_t* ws, nuwan_websocket_error_callback_t callback);
void nuwan_websocket_close(nuwan_websocket_t* ws);
bool nuwan_websocket_is_connected(nuwan_websocket_t* ws);
void nuwan_websocket_destroy(nuwan_websocket_t* ws);

#ifdef __cplusplus
}
#endif

#endif // NUWAN_HTTP_H
