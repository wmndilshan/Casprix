/**
 * Casperix Runtime - Socket Implementation
 */

#include "socket.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

bool socket_system_init(void) {
#ifdef _WIN32
    WSADATA wsa_data;
    return WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0;
#else
    return true;  // No initialization needed on Unix
#endif
}

void socket_system_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

Socket* socket_create(SocketType type) {
    Socket* sock = (Socket*)calloc(1, sizeof(Socket));
    if (!sock) return NULL;
    
    int socket_type = (type == SOCKET_TYPE_TCP) ? SOCK_STREAM : SOCK_DGRAM;
    int protocol = (type == SOCKET_TYPE_TCP) ? IPPROTO_TCP : IPPROTO_UDP;
    
    sock->handle = socket(AF_INET, socket_type, protocol);
    if (sock->handle == INVALID_SOCKET_HANDLE) {
        free(sock);
        return NULL;
    }
    
    sock->type = type;
    sock->state = SOCKET_STATE_CLOSED;
    sock->non_blocking = false;
    sock->last_error = 0;
    
    return sock;
}

void socket_destroy(Socket* sock) {
    if (!sock) return;
    socket_close(sock);
    free(sock);
}

bool socket_set_nonblocking(Socket* sock, bool enabled) {
    if (!sock) return false;
    
#ifdef _WIN32
    u_long mode = enabled ? 1 : 0;
    if (ioctlsocket(sock->handle, FIONBIO, &mode) != 0) {
        sock->last_error = WSAGetLastError();
        return false;
    }
#else
    int flags = fcntl(sock->handle, F_GETFL, 0);
    if (flags == -1) {
        sock->last_error = errno;
        return false;
    }
    
    flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (fcntl(sock->handle, F_SETFL, flags) == -1) {
        sock->last_error = errno;
        return false;
    }
#endif
    
    sock->non_blocking = enabled;
    return true;
}

bool socket_set_option(Socket* sock, int level, int option, const void* value, int value_len) {
    if (!sock) return false;
    
    if (setsockopt(sock->handle, level, option, (const char*)value, value_len) != 0) {
#ifdef _WIN32
        sock->last_error = WSAGetLastError();
#else
        sock->last_error = errno;
#endif
        return false;
    }
    
    return true;
}

bool socket_set_timeout(Socket* sock, int milliseconds) {
    if (!sock) return false;

#ifdef _WIN32
    DWORD tv = (milliseconds > 0) ? (DWORD)milliseconds : 0;
    bool ok = setsockopt(sock->handle, SOL_SOCKET, SO_RCVTIMEO,
                         (const char*)&tv, sizeof(tv)) == 0
           && setsockopt(sock->handle, SOL_SOCKET, SO_SNDTIMEO,
                         (const char*)&tv, sizeof(tv)) == 0;
    if (!ok) sock->last_error = WSAGetLastError();
    return ok;
#else
    struct timeval tv;
    if (milliseconds > 0) {
        tv.tv_sec  = milliseconds / 1000;
        tv.tv_usec = (milliseconds % 1000) * 1000;
    } else {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
    }
    bool ok = setsockopt(sock->handle, SOL_SOCKET, SO_RCVTIMEO,
                         &tv, sizeof(tv)) == 0
           && setsockopt(sock->handle, SOL_SOCKET, SO_SNDTIMEO,
                         &tv, sizeof(tv)) == 0;
    if (!ok) sock->last_error = errno;
    return ok;
#endif
}

bool socket_bind(Socket* sock, const char* ip, uint16_t port) {
    if (!sock) return false;
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (ip && strlen(ip) > 0) {
        addr.sin_addr.s_addr = inet_addr(ip);
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }
    
    if (bind(sock->handle, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
#ifdef _WIN32
        sock->last_error = WSAGetLastError();
#else
        sock->last_error = errno;
#endif
        return false;
    }
    
    snprintf(sock->local_addr.ip, sizeof(sock->local_addr.ip), "%s", ip ? ip : "0.0.0.0");
    sock->local_addr.port = port;
    
    return true;
}

bool socket_listen(Socket* sock, int backlog) {
    if (!sock || sock->type != SOCKET_TYPE_TCP) return false;
    
    if (listen(sock->handle, backlog) != 0) {
#ifdef _WIN32
        sock->last_error = WSAGetLastError();
#else
        sock->last_error = errno;
#endif
        return false;
    }
    
    sock->state = SOCKET_STATE_LISTENING;
    return true;
}

Socket* socket_accept(Socket* sock) {
    if (!sock || sock->type != SOCKET_TYPE_TCP) return NULL;
    
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    SocketHandle client_handle = accept(sock->handle, (struct sockaddr*)&client_addr, &addr_len);
    if (client_handle == INVALID_SOCKET_HANDLE) {
#ifdef _WIN32
        sock->last_error = WSAGetLastError();
#else
        sock->last_error = errno;
#endif
        return NULL;
    }
    
    Socket* client = (Socket*)calloc(1, sizeof(Socket));
    if (!client) {
#ifdef _WIN32
        closesocket(client_handle);
#else
        close(client_handle);
#endif
        return NULL;
    }
    
    client->handle = client_handle;
    client->type = SOCKET_TYPE_TCP;
    client->state = SOCKET_STATE_CONNECTED;
    client->non_blocking = sock->non_blocking;
    
    // Store remote address
    {
        char* addr_str = inet_ntoa(client_addr.sin_addr);
        if (addr_str) {
            snprintf(client->remote_addr.ip, sizeof(client->remote_addr.ip), "%s", addr_str);
        }
    }
    client->remote_addr.port = ntohs(client_addr.sin_port);
    
    return client;
}

bool socket_connect(Socket* sock, const char* ip, uint16_t port) {
    if (!sock || sock->type != SOCKET_TYPE_TCP) return false;
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);
    
    sock->state = SOCKET_STATE_CONNECTING;
    
    if (connect(sock->handle, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            sock->last_error = err;
            sock->state = SOCKET_STATE_ERROR;
            return false;
        }
#else
        if (errno != EINPROGRESS) {
            sock->last_error = errno;
            sock->state = SOCKET_STATE_ERROR;
            return false;
        }
#endif
    }
    
    snprintf(sock->remote_addr.ip, sizeof(sock->remote_addr.ip), "%s", ip);
    sock->remote_addr.port = port;
    sock->state = SOCKET_STATE_CONNECTED;
    
    return true;
}

int socket_send(Socket* sock, const void* data, size_t length) {
    if (!sock || !data) return -1;
    
    int sent = send(sock->handle, (const char*)data, (int)length, 0);
    if (sent < 0) {
#ifdef _WIN32
        sock->last_error = WSAGetLastError();
#else
        sock->last_error = errno;
#endif
    }
    
    return sent;
}

int socket_recv(Socket* sock, void* buffer, size_t length) {
    if (!sock || !buffer) return -1;
    
    int received = recv(sock->handle, (char*)buffer, (int)length, 0);
    if (received < 0) {
#ifdef _WIN32
        sock->last_error = WSAGetLastError();
#else
        sock->last_error = errno;
#endif
    } else if (received == 0) {
        sock->state = SOCKET_STATE_CLOSED;
    }
    
    return received;
}

int socket_sendto(Socket* sock, const void* data, size_t length, const char* ip, uint16_t port) {
    if (!sock || !data || !ip) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);

    int sent = sendto(sock->handle, (const char*)data, (int)length, 0,
                      (struct sockaddr*)&addr, sizeof(addr));
    if (sent < 0) {
#ifdef _WIN32
        sock->last_error = WSAGetLastError();
#else
        sock->last_error = errno;
#endif
    }
    return sent;
}

int socket_recvfrom(Socket* sock, void* buffer, size_t length, Address* from) {
    if (!sock || !buffer) return -1;

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    int received = recvfrom(sock->handle, (char*)buffer, (int)length, 0,
                            (struct sockaddr*)&addr, &addr_len);
    if (received < 0) {
#ifdef _WIN32
        sock->last_error = WSAGetLastError();
#else
        sock->last_error = errno;
#endif
    } else if (from) {
        char* addr_str = inet_ntoa(addr.sin_addr);
        if (addr_str) {
            snprintf(from->ip, sizeof(from->ip), "%s", addr_str);
        }
        from->port = ntohs(addr.sin_port);
        from->is_ipv6 = false;
    }
    return received;
}

void socket_close(Socket* sock) {
    if (!sock || sock->handle == INVALID_SOCKET_HANDLE) return;
    
#ifdef _WIN32
    closesocket(sock->handle);
#else
    close(sock->handle);
#endif
    
    sock->handle = INVALID_SOCKET_HANDLE;
    sock->state = SOCKET_STATE_CLOSED;
}

int socket_get_error(const Socket* sock) {
    return sock ? sock->last_error : 0;
}

bool address_parse(Address* addr, const char* ip, uint16_t port) {
    if (!addr || !ip) return false;
    
    snprintf(addr->ip, sizeof(addr->ip), "%s", ip);
    addr->port = port;
    addr->is_ipv6 = (strchr(ip, ':') != NULL);  // Simple IPv6 detection
    
    return true;
}
