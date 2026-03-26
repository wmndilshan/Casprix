#include "../runtime.h"
#include "../../include/casprix/stdlib.h"
#include "../../include/casprix/network.h"
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
    static bool winsock_initialized = false;

    static void init_winsock() {
        if (!winsock_initialized) {
            WSADATA wsaData;
            WSAStartup(MAKEWORD(2, 2), &wsaData);
            winsock_initialized = true;
        }
    }
#endif

// ============================================================================
// Network Initialization
// ============================================================================

void nuwan_network_init() {
#ifdef _WIN32
    init_winsock();
#endif
}

void nuwan_network_cleanup() {
#ifdef _WIN32
    if (winsock_initialized) {
        WSACleanup();
        winsock_initialized = false;
    }
#endif
}

// ============================================================================
// Low-level Socket Functions (backward compatibility)
// ============================================================================

int nuwan_socket_create() {
#ifdef _WIN32
    init_winsock();
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    return sock != INVALID_SOCKET ? (int)sock : -1;
#else
    return socket(AF_INET, SOCK_STREAM, 0);
#endif
}

int nuwan_socket_bind(int socket, int port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((unsigned short)port);

#ifdef _WIN32
    return bind((SOCKET)socket, (struct sockaddr*)&addr, sizeof(addr)) == 0 ? 0 : -1;
#else
    return bind(socket, (struct sockaddr*)&addr, sizeof(addr));
#endif
}

int nuwan_socket_listen(int socket, int backlog) {
#ifdef _WIN32
    return listen((SOCKET)socket, backlog) == 0 ? 0 : -1;
#else
    return listen(socket, backlog);
#endif
}

int nuwan_socket_accept(int socket) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

#ifdef _WIN32
    SOCKET client = accept((SOCKET)socket, (struct sockaddr*)&addr, &addr_len);
    return client != INVALID_SOCKET ? (int)client : -1;
#else
    return accept(socket, (struct sockaddr*)&addr, &addr_len);
#endif
}

int nuwan_socket_connect(int socket, const char* host, int port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);

    // Resolve hostname
    struct hostent* host_entry = gethostbyname(host);
    if (!host_entry) {
        return -1;
    }

    memcpy(&addr.sin_addr, host_entry->h_addr_list[0], host_entry->h_length);

#ifdef _WIN32
    return connect((SOCKET)socket, (struct sockaddr*)&addr, sizeof(addr)) == 0 ? 0 : -1;
#else
    return connect(socket, (struct sockaddr*)&addr, sizeof(addr));
#endif
}

int nuwan_socket_send(int socket, const char* data, int length) {
    if (!data || length <= 0) return -1;

#ifdef _WIN32
    int sent = send((SOCKET)socket, data, length, 0);
    return sent != SOCKET_ERROR ? sent : -1;
#else
    return send(socket, data, length, 0);
#endif
}

int nuwan_socket_receive(int socket, char* buffer, int length) {
    if (!buffer || length <= 0) return -1;

#ifdef _WIN32
    int received = recv((SOCKET)socket, buffer, length - 1, 0);
    if (received != SOCKET_ERROR && received > 0) {
        buffer[received] = '\0';
    }
    return received != SOCKET_ERROR ? received : -1;
#else
    int received = recv(socket, buffer, length - 1, 0);
    if (received > 0) {
        buffer[received] = '\0';
    }
    return received;
#endif
}

int nuwan_socket_close(int socket) {
#ifdef _WIN32
    return closesocket((SOCKET)socket) == 0 ? 0 : -1;
#else
    return close(socket);
#endif
}

char* nuwan_get_host_by_name(const char* hostname) {
    struct hostent* host_entry = gethostbyname(hostname);
    if (!host_entry || !host_entry->h_addr_list[0]) {
        return NULL;
    }

    char* ip = (char*)malloc(INET_ADDRSTRLEN);
    if (!ip) return NULL;

    inet_ntop(AF_INET, host_entry->h_addr_list[0], ip, INET_ADDRSTRLEN);
    return ip;
}

// ============================================================================
// High-level Socket API (nuwan_socket_t structure)
// ============================================================================

nuwan_socket_t* nuwan_socket_new(nuwan_socket_type_t type) {
    nuwan_socket_t* sock = (nuwan_socket_t*)malloc(sizeof(nuwan_socket_t));
    if (!sock) return NULL;

    memset(sock, 0, sizeof(nuwan_socket_t));
    sock->type = type;
    sock->is_connected = false;
    sock->is_listening = false;

#ifdef _WIN32
    init_winsock();
    if (type == NUWAN_SOCKET_TCP) {
        sock->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    } else {
        sock->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    }
    sock->fd = (sock->fd != INVALID_SOCKET) ? sock->fd : INVALID_SOCKET_VALUE;
#else
    if (type == NUWAN_SOCKET_TCP) {
        sock->fd = socket(AF_INET, SOCK_STREAM, 0);
    } else {
        sock->fd = socket(AF_INET, SOCK_DGRAM, 0);
    }
#endif

    if (sock->fd == INVALID_SOCKET_VALUE) {
        free(sock);
        return NULL;
    }

    return sock;
}

bool nuwan_socket_connect_ex(nuwan_socket_t* sock, const char* host, int port) {
    if (!sock || !host) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);

    // Resolve hostname
    struct hostent* host_entry = gethostbyname(host);
    if (!host_entry) {
        return false;
    }

    memcpy(&addr.sin_addr, host_entry->h_addr_list[0], host_entry->h_length);

    int result;
#ifdef _WIN32
    result = connect(sock->fd, (struct sockaddr*)&addr, sizeof(addr));
    sock->is_connected = (result == 0);
#else
    result = connect(sock->fd, (struct sockaddr*)&addr, sizeof(addr));
    sock->is_connected = (result == 0);
#endif

    if (sock->is_connected) {
        strncpy(sock->remote_addr, host, sizeof(sock->remote_addr) - 1);
        sock->remote_port = port;

        // Get local address
        struct sockaddr_in local_addr;
        socklen_t local_len = sizeof(local_addr);
        getsockname(sock->fd, (struct sockaddr*)&local_addr, &local_len);
        inet_ntop(AF_INET, &local_addr.sin_addr, sock->local_addr, sizeof(sock->local_addr));
        sock->local_port = ntohs(local_addr.sin_port);
    }

    return sock->is_connected;
}

bool nuwan_socket_bind_ex(nuwan_socket_t* sock, const char* addr, int port) {
    if (!sock) return false;

    struct sockaddr_in sockaddr;
    memset(&sockaddr, 0, sizeof(sockaddr));
    sockaddr.sin_family = AF_INET;
    sockaddr.sin_port = htons((unsigned short)port);

    if (addr && strlen(addr) > 0) {
        inet_pton(AF_INET, addr, &sockaddr.sin_addr);
    } else {
        sockaddr.sin_addr.s_addr = INADDR_ANY;
    }

    int result = bind(sock->fd, (struct sockaddr*)&sockaddr, sizeof(sockaddr));

    if (result == 0) {
        strncpy(sock->local_addr, addr ? addr : "0.0.0.0", sizeof(sock->local_addr) - 1);
        sock->local_port = port;
        return true;
    }

    return false;
}

bool nuwan_socket_listen_ex(nuwan_socket_t* sock, int backlog) {
    if (!sock || sock->type != NUWAN_SOCKET_TCP) return false;

    int result = listen(sock->fd, backlog);
    sock->is_listening = (result == 0);
    return sock->is_listening;
}

nuwan_socket_t* nuwan_socket_accept_ex(nuwan_socket_t* sock) {
    if (!sock || !sock->is_listening) return NULL;

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    socket_t client_fd = accept(sock->fd, (struct sockaddr*)&addr, &addr_len);

#ifdef _WIN32
    if (client_fd == INVALID_SOCKET) return NULL;
#else
    if (client_fd < 0) return NULL;
#endif

    nuwan_socket_t* client = (nuwan_socket_t*)malloc(sizeof(nuwan_socket_t));
    if (!client) {
#ifdef _WIN32
        closesocket(client_fd);
#else
        close(client_fd);
#endif
        return NULL;
    }

    memset(client, 0, sizeof(nuwan_socket_t));
    client->fd = client_fd;
    client->type = NUWAN_SOCKET_TCP;
    client->is_connected = true;
    client->is_listening = false;

    // Get remote address
    inet_ntop(AF_INET, &addr.sin_addr, client->remote_addr, sizeof(client->remote_addr));
    client->remote_port = ntohs(addr.sin_port);

    // Get local address
    struct sockaddr_in local_addr;
    socklen_t local_len = sizeof(local_addr);
    getsockname(client_fd, (struct sockaddr*)&local_addr, &local_len);
    inet_ntop(AF_INET, &local_addr.sin_addr, client->local_addr, sizeof(client->local_addr));
    client->local_port = ntohs(local_addr.sin_port);

    return client;
}

int nuwan_socket_send_ex(nuwan_socket_t* sock, const char* data, int len) {
    if (!sock || !data || len <= 0) return -1;

#ifdef _WIN32
    int sent = send(sock->fd, data, len, 0);
    return sent != SOCKET_ERROR ? sent : -1;
#else
    return send(sock->fd, data, len, 0);
#endif
}

int nuwan_socket_recv_ex(nuwan_socket_t* sock, char* buffer, int len) {
    if (!sock || !buffer || len <= 0) return -1;

#ifdef _WIN32
    int received = recv(sock->fd, buffer, len, 0);
    return received != SOCKET_ERROR ? received : -1;
#else
    return recv(sock->fd, buffer, len, 0);
#endif
}

// UDP-specific functions
int nuwan_socket_sendto(nuwan_socket_t* sock, const char* data, int len,
                        const char* host, int port) {
    if (!sock || sock->type != NUWAN_SOCKET_UDP || !data || len <= 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);

    struct hostent* host_entry = gethostbyname(host);
    if (!host_entry) {
        return -1;
    }

    memcpy(&addr.sin_addr, host_entry->h_addr_list[0], host_entry->h_length);

#ifdef _WIN32
    int sent = sendto(sock->fd, data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
    return sent != SOCKET_ERROR ? sent : -1;
#else
    return sendto(sock->fd, data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
#endif
}

int nuwan_socket_recvfrom(nuwan_socket_t* sock, char* buffer, int len,
                          char* from_addr, int* from_port) {
    if (!sock || sock->type != NUWAN_SOCKET_UDP || !buffer || len <= 0) return -1;

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

#ifdef _WIN32
    int received = recvfrom(sock->fd, buffer, len, 0, (struct sockaddr*)&addr, &addr_len);
    if (received != SOCKET_ERROR && received > 0) {
        if (from_addr) {
            inet_ntop(AF_INET, &addr.sin_addr, from_addr, INET_ADDRSTRLEN);
        }
        if (from_port) {
            *from_port = ntohs(addr.sin_port);
        }
    }
    return received != SOCKET_ERROR ? received : -1;
#else
    int received = recvfrom(sock->fd, buffer, len, 0, (struct sockaddr*)&addr, &addr_len);
    if (received > 0) {
        if (from_addr) {
            inet_ntop(AF_INET, &addr.sin_addr, from_addr, INET_ADDRSTRLEN);
        }
        if (from_port) {
            *from_port = ntohs(addr.sin_port);
        }
    }
    return received;
#endif
}

void nuwan_socket_close_ex(nuwan_socket_t* sock) {
    if (!sock) return;

    if (sock->fd != INVALID_SOCKET_VALUE) {
#ifdef _WIN32
        closesocket(sock->fd);
#else
        close(sock->fd);
#endif
        sock->fd = INVALID_SOCKET_VALUE;
    }

    sock->is_connected = false;
    sock->is_listening = false;
}

void nuwan_socket_destroy(nuwan_socket_t* sock) {
    if (!sock) return;
    nuwan_socket_close_ex(sock);
    free(sock);
}

// ============================================================================
// Server API
// ============================================================================

nuwan_server_t* nuwan_server_create(int port, int max_connections) {
    nuwan_server_t* server = (nuwan_server_t*)malloc(sizeof(nuwan_server_t));
    if (!server) return NULL;

    memset(server, 0, sizeof(nuwan_server_t));
    server->max_connections = max_connections;
    server->active_connections = 0;

    // Create socket
    server->socket = nuwan_socket_new(NUWAN_SOCKET_TCP);
    if (!server->socket) {
        free(server);
        return NULL;
    }

    // Bind to port
    if (!nuwan_socket_bind_ex(server->socket, "0.0.0.0", port)) {
        nuwan_socket_destroy(server->socket);
        free(server);
        return NULL;
    }

    return server;
}

bool nuwan_server_start(nuwan_server_t* server) {
    if (!server || !server->socket) return false;
    return nuwan_socket_listen_ex(server->socket, server->max_connections);
}

nuwan_socket_t* nuwan_server_accept_client(nuwan_server_t* server) {
    if (!server || !server->socket) return NULL;

    nuwan_socket_t* client = nuwan_socket_accept_ex(server->socket);
    if (client) {
        server->active_connections++;
    }

    return client;
}

void nuwan_server_stop(nuwan_server_t* server) {
    if (!server || !server->socket) return;
    nuwan_socket_close_ex(server->socket);
}

void nuwan_server_destroy(nuwan_server_t* server) {
    if (!server) return;

    if (server->socket) {
        nuwan_socket_destroy(server->socket);
    }

    free(server);
}

// ============================================================================
// Socket Options
// ============================================================================

bool nuwan_socket_set_nonblocking(nuwan_socket_t* sock, bool enabled) {
    if (!sock) return false;

#ifdef _WIN32
    u_long mode = enabled ? 1 : 0;
    return ioctlsocket(sock->fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(sock->fd, F_GETFL, 0);
    if (flags == -1) return false;

    if (enabled) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }

    return fcntl(sock->fd, F_SETFL, flags) == 0;
#endif
}

bool nuwan_socket_set_reuse_addr(nuwan_socket_t* sock, bool enabled) {
    if (!sock) return false;

    int opt = enabled ? 1 : 0;
    return setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR,
                     (const char*)&opt, sizeof(opt)) == 0;
}

bool nuwan_socket_set_timeout(nuwan_socket_t* sock, int milliseconds) {
    if (!sock) return false;

#ifdef _WIN32
    DWORD timeout = milliseconds;
    return setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO,
                     (const char*)&timeout, sizeof(timeout)) == 0 &&
           setsockopt(sock->fd, SOL_SOCKET, SO_SNDTIMEO,
                     (const char*)&timeout, sizeof(timeout)) == 0;
#else
    struct timeval tv;
    tv.tv_sec = milliseconds / 1000;
    tv.tv_usec = (milliseconds % 1000) * 1000;
    return setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO,
                     (const char*)&tv, sizeof(tv)) == 0 &&
           setsockopt(sock->fd, SOL_SOCKET, SO_SNDTIMEO,
                     (const char*)&tv, sizeof(tv)) == 0;
#endif
}
