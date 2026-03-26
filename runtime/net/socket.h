/**
 * Casperix Runtime - Socket Abstraction
 * Cross-platform non-blocking socket API
 */

#ifndef CASPERIX_SOCKET_H
#define CASPERIX_SOCKET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET SocketHandle;
#define INVALID_SOCKET_HANDLE INVALID_SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
typedef int SocketHandle;
#define INVALID_SOCKET_HANDLE -1
#endif

// Socket types
typedef enum {
    SOCKET_TYPE_TCP,
    SOCKET_TYPE_UDP
} SocketType;

// Socket state
typedef enum {
    SOCKET_STATE_CLOSED,
    SOCKET_STATE_LISTENING,
    SOCKET_STATE_CONNECTING,
    SOCKET_STATE_CONNECTED,
    SOCKET_STATE_ERROR
} SocketState;

// Address structure
typedef struct {
    char ip[46];  // IPv4 or IPv6
    uint16_t port;
    bool is_ipv6;
} Address;

// Socket structure
typedef struct Socket {
    SocketHandle handle;
    SocketType type;
    SocketState state;
    Address local_addr;
    Address remote_addr;
    int last_error;
    bool non_blocking;
} Socket;

// Initialize socket system (call once at startup)
bool socket_system_init(void);

// Cleanup socket system
void socket_system_cleanup(void);

// Create a socket
Socket* socket_create(SocketType type);

// Destroy a socket
void socket_destroy(Socket* sock);

// Set socket to non-blocking mode
bool socket_set_nonblocking(Socket* sock, bool enabled);

// Set socket options
bool socket_set_option(Socket* sock, int level, int option, const void* value, int value_len);

// Bind socket to address
bool socket_bind(Socket* sock, const char* ip, uint16_t port);

// Listen for connections (TCP only)
bool socket_listen(Socket* sock, int backlog);

// Accept a connection (TCP only)
Socket* socket_accept(Socket* sock);

// Connect to remote address (TCP)
bool socket_connect(Socket* sock, const char* ip, uint16_t port);

// Send data
int socket_send(Socket* sock, const void* data, size_t length);

// Receive data
int socket_recv(Socket* sock, void* buffer, size_t length);

// Send to address (UDP)
int socket_sendto(Socket* sock, const void* data, size_t length, const char* ip, uint16_t port);

// Receive from address (UDP)
int socket_recvfrom(Socket* sock, void* buffer, size_t length, Address* from);

// Close socket
void socket_close(Socket* sock);

// Get last error
int socket_get_error(const Socket* sock);

// Parse address from string
bool address_parse(Address* addr, const char* ip, uint16_t port);

#endif // CASPERIX_SOCKET_H
