#include "../runtime.h"
#include "../../include/casprix/stdlib.h"
#include "../../include/casprix/network.h"
#include "../../include/casprix/http.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>

// ============================================================================
// WebSocket Protocol Implementation (RFC 6455)
// ============================================================================

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

// WebSocket frame opcodes
typedef enum {
    WS_OPCODE_CONTINUATION = 0x0,
    WS_OPCODE_TEXT = 0x1,
    WS_OPCODE_BINARY = 0x2,
    WS_OPCODE_CLOSE = 0x8,
    WS_OPCODE_PING = 0x9,
    WS_OPCODE_PONG = 0xA
} ws_opcode_t;

// WebSocket frame header
typedef struct {
    uint8_t fin;
    uint8_t opcode;
    uint8_t mask;
    uint64_t payload_length;
    uint8_t masking_key[4];
} ws_frame_header_t;

// Internal WebSocket structure
typedef struct {
    nuwan_socket_t* socket;
    char* url;
    char host[256];
    int port;
    char path[1024];
    bool is_connected;
    bool is_secure;
    nuwan_websocket_message_callback_t on_message;
    nuwan_websocket_close_callback_t on_close;
    nuwan_websocket_error_callback_t on_error;
    void* user_data;
} nuwan_websocket_internal_t;

// ============================================================================
// Base64 encoding (for WebSocket handshake)
// ============================================================================

static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char* base64_encode(const unsigned char* data, size_t input_length) {
    size_t output_length = 4 * ((input_length + 2) / 3);
    char* encoded = (char*)malloc(output_length + 1);
    if (!encoded) return NULL;

    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? data[i++] : 0;
        uint32_t octet_b = i < input_length ? data[i++] : 0;
        uint32_t octet_c = i < input_length ? data[i++] : 0;
        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        encoded[j++] = base64_chars[(triple >> 3 * 6) & 0x3F];
        encoded[j++] = base64_chars[(triple >> 2 * 6) & 0x3F];
        encoded[j++] = base64_chars[(triple >> 1 * 6) & 0x3F];
        encoded[j++] = base64_chars[(triple >> 0 * 6) & 0x3F];
    }

    for (int i = 0; i < (3 - input_length % 3) % 3; i++) {
        encoded[output_length - 1 - i] = '=';
    }

    encoded[output_length] = '\0';
    return encoded;
}

// ============================================================================
// WebSocket Utilities
// ============================================================================

static void parse_ws_url(const char* url, char* host, int* port, char* path, bool* is_secure) {
    const char* proto_end = strstr(url, "://");
    const char* start = proto_end ? proto_end + 3 : url;

    *is_secure = false;
    if (proto_end && strncmp(url, "wss", 3) == 0) {
        *is_secure = true;
        *port = 443;
    } else {
        *port = 80;
    }

    const char* path_start = strchr(start, '/');
    const char* port_start = strchr(start, ':');

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

    if (path_start) {
        strcpy(path, path_start);
    } else {
        strcpy(path, "/");
    }
}

static char* generate_websocket_key() {
    unsigned char random_bytes[16];
    srand((unsigned int)time(NULL));
    for (int i = 0; i < 16; i++) {
        random_bytes[i] = (unsigned char)(rand() % 256);
    }
    return base64_encode(random_bytes, 16);
}

static bool websocket_send_frame(nuwan_websocket_internal_t* ws,
                                 uint8_t opcode,
                                 const uint8_t* payload,
                                 size_t payload_len) {
    if (!ws || !ws->is_connected || !ws->socket) return false;
    if (payload_len > (size_t)INT32_MAX) return false;

    uint8_t frame[14];
    int header_len = 0;

    frame[header_len++] = (uint8_t)(0x80 | (opcode & 0x0F));

    if (payload_len < 126) {
        frame[header_len++] = (uint8_t)(0x80 | (uint8_t)payload_len);
    } else if (payload_len <= 0xFFFFu) {
        frame[header_len++] = (uint8_t)(0x80 | 126);
        frame[header_len++] = (uint8_t)((payload_len >> 8) & 0xFF);
        frame[header_len++] = (uint8_t)(payload_len & 0xFF);
    } else {
        frame[header_len++] = (uint8_t)(0x80 | 127);
        for (int i = 7; i >= 0; i--) {
            frame[header_len++] = (uint8_t)((payload_len >> (i * 8)) & 0xFF);
        }
    }

    uint8_t mask[4];
    for (int i = 0; i < 4; i++) {
        mask[i] = (uint8_t)(rand() & 0xFF);
        frame[header_len++] = mask[i];
    }

    if (nuwan_socket_send_ex(ws->socket, (const char*)frame, header_len) <= 0) {
        return false;
    }

    if (payload_len == 0) return true;

    uint8_t* masked_payload = (uint8_t*)malloc(payload_len);
    if (!masked_payload) return false;

    for (size_t i = 0; i < payload_len; i++) {
        masked_payload[i] = payload[i] ^ mask[i % 4];
    }

    int sent = nuwan_socket_send_ex(ws->socket, (const char*)masked_payload, (int)payload_len);
    free(masked_payload);
    return sent == (int)payload_len;
}

// ============================================================================
// WebSocket API Implementation
// ============================================================================

nuwan_websocket_t* nuwan_websocket_create(const char* url) {
    if (!url) return NULL;

    nuwan_websocket_internal_t* ws = (nuwan_websocket_internal_t*)calloc(1, sizeof(nuwan_websocket_internal_t));
    if (!ws) return NULL;

    ws->url = strdup(url);
    parse_ws_url(url, ws->host, &ws->port, ws->path, &ws->is_secure);
    ws->is_connected = false;

    return (nuwan_websocket_t*)ws;
}

static bool websocket_handshake(nuwan_websocket_internal_t* ws) {
    // Generate WebSocket key
    char* ws_key = generate_websocket_key();
    if (!ws_key) return false;

    // Build handshake request
    char request[2048];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "\r\n",
             ws->path, ws->host, ws_key);

    free(ws_key);

    // Send handshake
    int sent = nuwan_socket_send_ex(ws->socket, request, strlen(request));
    if (sent <= 0) {
        return false;
    }

    // Receive handshake response
    char response[4096];
    int received = nuwan_socket_recv_ex(ws->socket, response, sizeof(response) - 1);
    if (received <= 0) {
        return false;
    }
    response[received] = '\0';

    // Check for "101 Switching Protocols"
    if (strstr(response, "101") && strstr(response, "Switching Protocols")) {
        return true;
    }

    return false;
}

nuwan_promise_t* nuwan_websocket_connect_async(nuwan_websocket_t* ws_public) {
    nuwan_websocket_internal_t* ws = (nuwan_websocket_internal_t*)ws_public;
    if (!ws) return NULL;

    // Note: This is a simplified synchronous implementation
    // In a real implementation, this would use async I/O

    ws->socket = nuwan_socket_new(NUWAN_SOCKET_TCP);
    if (!ws->socket) {
        if (ws->on_error) {
            ws->on_error(ws_public, "Failed to create socket");
        }
        return NULL;
    }

    // Connect to server
    if (!nuwan_socket_connect_ex(ws->socket, ws->host, ws->port)) {
        if (ws->on_error) {
            ws->on_error(ws_public, "Failed to connect to server");
        }
        nuwan_socket_destroy(ws->socket);
        ws->socket = NULL;
        return NULL;
    }

    // Perform WebSocket handshake
    if (!websocket_handshake(ws)) {
        if (ws->on_error) {
            ws->on_error(ws_public, "WebSocket handshake failed");
        }
        nuwan_socket_destroy(ws->socket);
        ws->socket = NULL;
        return NULL;
    }

    ws->is_connected = true;

    // For now, return NULL (would return a promise in full async implementation)
    return NULL;
}

bool nuwan_websocket_send(nuwan_websocket_t* ws_public, const char* message) {
    nuwan_websocket_internal_t* ws = (nuwan_websocket_internal_t*)ws_public;
    if (!ws || !ws->is_connected || !message) return false;

    size_t msg_len = strlen(message);
    return websocket_send_frame(ws, WS_OPCODE_TEXT, (const uint8_t*)message, msg_len);
}

void nuwan_websocket_set_on_message(nuwan_websocket_t* ws_public, nuwan_websocket_message_callback_t callback) {
    nuwan_websocket_internal_t* ws = (nuwan_websocket_internal_t*)ws_public;
    if (ws) ws->on_message = callback;
}

void nuwan_websocket_set_on_close(nuwan_websocket_t* ws_public, nuwan_websocket_close_callback_t callback) {
    nuwan_websocket_internal_t* ws = (nuwan_websocket_internal_t*)ws_public;
    if (ws) ws->on_close = callback;
}

void nuwan_websocket_set_on_error(nuwan_websocket_t* ws_public, nuwan_websocket_error_callback_t callback) {
    nuwan_websocket_internal_t* ws = (nuwan_websocket_internal_t*)ws_public;
    if (ws) ws->on_error = callback;
}

void nuwan_websocket_close(nuwan_websocket_t* ws_public) {
    nuwan_websocket_internal_t* ws = (nuwan_websocket_internal_t*)ws_public;
    if (!ws) return;

    if (ws->is_connected && ws->socket) {
        (void)websocket_send_frame(ws, WS_OPCODE_CLOSE, NULL, 0);

        nuwan_socket_close_ex(ws->socket);
        ws->is_connected = false;
    }

    if (ws->on_close) {
        ws->on_close(ws_public, 1000, "Normal closure");
    }
}

bool nuwan_websocket_is_connected(nuwan_websocket_t* ws_public) {
    nuwan_websocket_internal_t* ws = (nuwan_websocket_internal_t*)ws_public;
    return ws ? ws->is_connected : false;
}

void nuwan_websocket_destroy(nuwan_websocket_t* ws_public) {
    nuwan_websocket_internal_t* ws = (nuwan_websocket_internal_t*)ws_public;
    if (!ws) return;

    if (ws->is_connected) {
        nuwan_websocket_close(ws_public);
    }

    if (ws->socket) {
        nuwan_socket_destroy(ws->socket);
    }

    if (ws->url) {
        free(ws->url);
    }

    free(ws);
}

// ============================================================================
// WebSocket receive (blocking - for polling or event loop)
// ============================================================================

bool nuwan_websocket_poll_message(nuwan_websocket_t* ws_public, int timeout_ms) {
    nuwan_websocket_internal_t* ws = (nuwan_websocket_internal_t*)ws_public;
    if (!ws || !ws->is_connected) return false;

    // Set socket timeout
    nuwan_socket_set_timeout(ws->socket, timeout_ms);

    // Read frame header (minimum 2 bytes)
    uint8_t header[2];
    int received = nuwan_socket_recv_ex(ws->socket, (char*)header, 2);
    if (received <= 0) {
        return false;
    }

    uint8_t fin = (header[0] & 0x80) != 0;
    uint8_t opcode = header[0] & 0x0F;
    uint8_t masked = (header[1] & 0x80) != 0;
    uint64_t payload_len = header[1] & 0x7F;

    // Extended payload length
    if (payload_len == 126) {
        uint8_t len_bytes[2];
        if (nuwan_socket_recv_ex(ws->socket, (char*)len_bytes, 2) != 2) {
            return false;
        }
        payload_len = (len_bytes[0] << 8) | len_bytes[1];
    } else if (payload_len == 127) {
        uint8_t len_bytes[8];
        if (nuwan_socket_recv_ex(ws->socket, (char*)len_bytes, 8) != 8) {
            return false;
        }
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | len_bytes[i];
        }
    }

    if (payload_len > (64ull * 1024ull * 1024ull)) {
        if (ws->on_error) {
            ws->on_error(ws_public, "WebSocket payload exceeds safety limit");
        }
        return false;
    }

    // Read masking key (if present)
    uint8_t mask[4] = {0};
    if (masked) {
        if (nuwan_socket_recv_ex(ws->socket, (char*)mask, 4) != 4) {
            return false;
        }
    }

    // Read payload
    if (payload_len > 0) {
        uint8_t* payload = (uint8_t*)malloc(payload_len + 1);
        if (!payload) return false;

        int read_total = 0;
        while (read_total < payload_len) {
            int read_now = nuwan_socket_recv_ex(ws->socket, (char*)(payload + read_total),
                                               payload_len - read_total);
            if (read_now <= 0) {
                free(payload);
                return false;
            }
            read_total += read_now;
        }

        // Unmask if needed
        if (masked) {
            for (uint64_t i = 0; i < payload_len; i++) {
                payload[i] ^= mask[i % 4];
            }
        }

        payload[payload_len] = '\0';

        // Handle different opcodes
        switch (opcode) {
            case WS_OPCODE_TEXT:
            case WS_OPCODE_BINARY:
                if (ws->on_message) {
                    ws->on_message(ws_public, (const char*)payload, payload_len);
                }
                break;

            case WS_OPCODE_CLOSE:
                if (ws->on_close) {
                    ws->on_close(ws_public, 1000, "Peer closed connection");
                }
                ws->is_connected = false;
                break;

            case WS_OPCODE_PING:
                (void)websocket_send_frame(ws, WS_OPCODE_PONG, payload, (size_t)payload_len);
                break;

            default:
                break;
        }

        free(payload);
    }

    return true;
}
