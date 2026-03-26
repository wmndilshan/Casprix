// Casprix Network Library
// Platform-agnostic socket networking

#ifndef NUWAN_NETWORK_H
#define NUWAN_NETWORK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef SOCKET socket_t;
    #define INVALID_SOCKET_VALUE INVALID_SOCKET
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    typedef int socket_t;
    #define INVALID_SOCKET_VALUE -1
#endif

#define NUWAN_MAX_CONNECTIONS 128
#define NUWAN_BUFFER_SIZE 4096

// Socket types
typedef enum {
    NUWAN_SOCKET_TCP,
    NUWAN_SOCKET_UDP
} nuwan_socket_type_t;

// Socket structure (maps to Casprix Socket class)
typedef struct {
    socket_t fd;
    nuwan_socket_type_t type;
    bool is_connected;
    bool is_listening;
    char remote_addr[64];
    int remote_port;
    char local_addr[64];
    int local_port;
} nuwan_socket_t;

// Server structure (maps to Casprix Server class)
typedef struct {
    nuwan_socket_t* socket;
    int max_connections;
    int active_connections;
} nuwan_server_t;

// Network API
void nuwan_network_init();
void nuwan_network_cleanup();

// Socket API (high-level, uses nuwan_socket_t structure)
nuwan_socket_t* nuwan_socket_new(nuwan_socket_type_t type);
bool nuwan_socket_connect_ex(nuwan_socket_t* sock, const char* host, int port);
bool nuwan_socket_bind_ex(nuwan_socket_t* sock, const char* addr, int port);
bool nuwan_socket_listen_ex(nuwan_socket_t* sock, int backlog);
nuwan_socket_t* nuwan_socket_accept_ex(nuwan_socket_t* sock);
int nuwan_socket_send_ex(nuwan_socket_t* sock, const char* data, int len);
int nuwan_socket_recv_ex(nuwan_socket_t* sock, char* buffer, int len);
void nuwan_socket_close_ex(nuwan_socket_t* sock);
void nuwan_socket_destroy(nuwan_socket_t* sock);

// UDP-specific
int nuwan_socket_sendto(nuwan_socket_t* sock, const char* data, int len, const char* host, int port);
int nuwan_socket_recvfrom(nuwan_socket_t* sock, char* buffer, int len, char* from_addr, int* from_port);

// Socket options
bool nuwan_socket_set_nonblocking(nuwan_socket_t* sock, bool enabled);
bool nuwan_socket_set_reuse_addr(nuwan_socket_t* sock, bool enabled);
bool nuwan_socket_set_timeout(nuwan_socket_t* sock, int milliseconds);

// Server API
nuwan_server_t* nuwan_server_create(int port, int max_connections);
bool nuwan_server_start(nuwan_server_t* server);
nuwan_socket_t* nuwan_server_accept_client(nuwan_server_t* server);
void nuwan_server_stop(nuwan_server_t* server);
void nuwan_server_destroy(nuwan_server_t* server);

#endif // NUWAN_NETWORK_H
