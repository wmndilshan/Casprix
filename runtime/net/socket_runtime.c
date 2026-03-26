#include "socket_runtime.h"
#include <stdio.h>
#include <string.h>

static char last_error[256] = {0};

// Initialize sockets (required on Windows)
int nuwan_socket_init(void) {
#ifdef _WIN32
    WSADATA wsa_data;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) {
        snprintf(last_error, sizeof(last_error), "WSAStartup failed: %d", result);
        return -1;
    }
#endif
    return 0;
}

// Cleanup sockets
void nuwan_socket_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

// Create socket
socket_t nuwan_socket_create(int domain, int type, int protocol) {
    socket_t sockfd = socket(domain, type, protocol);
    if (sockfd == INVALID_SOCKET) {
#ifdef _WIN32
        snprintf(last_error, sizeof(last_error), 
                "socket() failed: %d", WSAGetLastError());
#else
        snprintf(last_error, sizeof(last_error), "socket() failed");
#endif
        return INVALID_SOCKET;
    }
    return sockfd;
}

// Bind socket to address
int nuwan_socket_bind(socket_t sockfd, const char* addr, int port) {
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    // Parse address
    if (addr == NULL || strlen(addr) == 0 || strcmp(addr, "0.0.0.0") == 0) {
        server_addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        server_addr.sin_addr.s_addr = inet_addr(addr);
    }
    
    int result = bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (result == SOCKET_ERROR) {
#ifdef _WIN32
        snprintf(last_error, sizeof(last_error), 
                "bind() failed: %d", WSAGetLastError());
#else
        snprintf(last_error, sizeof(last_error), "bind() failed");
#endif
        return -1;
    }
    
    return 0;
}

// Listen for connections
int nuwan_socket_listen(socket_t sockfd, int backlog) {
    int result = listen(sockfd, backlog);
    if (result == SOCKET_ERROR) {
#ifdef _WIN32
        snprintf(last_error, sizeof(last_error), 
                "listen() failed: %d", WSAGetLastError());
#else
        snprintf(last_error, sizeof(last_error), "listen() failed");
#endif
        return -1;
    }
    return 0;
}

// Accept connection
socket_t nuwan_socket_accept(socket_t sockfd, char* client_addr, int addr_len) {
    struct sockaddr_in client;
    int client_len = sizeof(client);
    
    socket_t client_sock = accept(sockfd, (struct sockaddr*)&client, 
#ifdef _WIN32
                                   &client_len
#else
                                   (socklen_t*)&client_len
#endif
    );
    
    if (client_sock == INVALID_SOCKET) {
#ifdef _WIN32
        snprintf(last_error, sizeof(last_error), 
                "accept() failed: %d", WSAGetLastError());
#else
        snprintf(last_error, sizeof(last_error), "accept() failed");
#endif
        return INVALID_SOCKET;
    }
    
    // Copy client address if buffer provided
    if (client_addr && addr_len > 0) {
        const char* addr_str = inet_ntoa(client.sin_addr);
        if (addr_str) {
            strncpy(client_addr, addr_str, addr_len - 1);
            client_addr[addr_len - 1] = '\0';
        }
    }
    
    return client_sock;
}

// Connect to remote host
int nuwan_socket_connect(socket_t sockfd, const char* addr, int port) {
    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr(addr);
    
    int result = connect(sockfd, (struct sockaddr*)&server, sizeof(server));
    if (result == SOCKET_ERROR) {
#ifdef _WIN32
        snprintf(last_error, sizeof(last_error), 
                "connect() failed: %d", WSAGetLastError());
#else
        snprintf(last_error, sizeof(last_error), "connect() failed");
#endif
        return -1;
    }
    
    return 0;
}

// Send data
int nuwan_socket_send(socket_t sockfd, const char* buf, int len) {
    int sent = send(sockfd, buf, len, 0);
    if (sent == SOCKET_ERROR) {
#ifdef _WIN32
        snprintf(last_error, sizeof(last_error), 
                "send() failed: %d", WSAGetLastError());
#else
        snprintf(last_error, sizeof(last_error), "send() failed");
#endif
        return -1;
    }
    return sent;
}

// Receive data
int nuwan_socket_recv(socket_t sockfd, char* buf, int len) {
    int received = recv(sockfd, buf, len, 0);
    if (received == SOCKET_ERROR) {
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return 0; // Non-blocking, no data available
        }
        snprintf(last_error, sizeof(last_error), 
                "recv() failed: %d", err);
#else
        snprintf(last_error, sizeof(last_error), "recv() failed");
#endif
        return -1;
    }
    return received;
}

// Close socket
int nuwan_socket_close(socket_t sockfd) {
#ifdef _WIN32
    return closesocket(sockfd);
#else
    return close(sockfd);
#endif
}

// Set blocking mode
int nuwan_socket_set_blocking(socket_t sockfd, int blocking) {
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    return ioctlsocket(sockfd, FIONBIO, &mode);
#else
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) return -1;
    
    flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return fcntl(sockfd, F_SETFL, flags);
#endif
}

// Set SO_REUSEADDR
int nuwan_socket_set_reuse_addr(socket_t sockfd, int reuse) {
    int opt = reuse ? 1 : 0;
    return setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
                     (const char*)&opt, sizeof(opt));
}

// Set TCP_NODELAY (disable Nagle's algorithm)
int nuwan_socket_set_nodelay(socket_t sockfd, int nodelay) {
    int opt = nodelay ? 1 : 0;
    return setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, 
                     (const char*)&opt, sizeof(opt));
}

// Set SO_KEEPALIVE
int nuwan_socket_set_keepalive(socket_t sockfd, int keepalive) {
    int opt = keepalive ? 1 : 0;
    return setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, 
                     (const char*)&opt, sizeof(opt));
}

// Get last error message
const char* nuwan_socket_get_last_error(void) {
    return last_error;
}
