#ifndef SOCKET_RUNTIME_H
#define SOCKET_RUNTIME_H

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef SOCKET socket_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    typedef int socket_t;
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
#endif

// Socket domain
#define NUWAN_AF_INET  2     // IPv4
#define NUWAN_AF_INET6 23    // IPv6

// Socket type
#define NUWAN_SOCK_STREAM 1  // TCP
#define NUWAN_SOCK_DGRAM  2  // UDP

// Socket options
#define NUWAN_SO_REUSEADDR   2
#define NUWAN_SO_KEEPALIVE   8
#define NUWAN_TCP_NODELAY    1

// Initialize/cleanup
int nuwan_socket_init(void);
void nuwan_socket_cleanup(void);

// Socket operations
socket_t nuwan_socket_create(int domain, int type, int protocol);
int nuwan_socket_bind(socket_t sockfd, const char* addr, int port);
int nuwan_socket_listen(socket_t sockfd, int backlog);
socket_t nuwan_socket_accept(socket_t sockfd, char* client_addr, int addr_len);
int nuwan_socket_connect(socket_t sockfd, const char* addr, int port);
int nuwan_socket_send(socket_t sockfd, const char* buf, int len);
int nuwan_socket_recv(socket_t sockfd, char* buf, int len);
int nuwan_socket_close(socket_t sockfd);

// Socket configuration
int nuwan_socket_set_blocking(socket_t sockfd, int blocking);
int nuwan_socket_set_reuse_addr(socket_t sockfd, int reuse);
int nuwan_socket_set_nodelay(socket_t sockfd, int nodelay);
int nuwan_socket_set_keepalive(socket_t sockfd, int keepalive);

// Utility
const char* nuwan_socket_get_last_error(void);

#endif // SOCKET_RUNTIME_H
