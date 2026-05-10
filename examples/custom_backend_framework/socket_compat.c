#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static char g_last_error[256];

static void set_last_error_from_errno(void) {
    const char* msg = strerror(errno);
    if (!msg) {
        msg = "unknown socket error";
    }
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
}

int nuwan_socket_init(void) {
    g_last_error[0] = '\0';
    return 0;
}

void nuwan_socket_cleanup(void) {
}

int nuwan_socket_create(int domain, int type, int protocol) {
    int fd = socket(domain, type, protocol);
    if (fd < 0) {
        set_last_error_from_errno();
        return -1;
    }
    return fd;
}

int nuwan_socket_bind(int sockfd, const char* addr, int port) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);

    if (!addr || strcmp(addr, "") == 0 || strcmp(addr, "0.0.0.0") == 0) {
        sa.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        set_last_error_from_errno();
        return -1;
    }

    if (bind(sockfd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        set_last_error_from_errno();
        return -1;
    }
    return 0;
}

int nuwan_socket_listen(int sockfd, int backlog) {
    if (listen(sockfd, backlog) != 0) {
        set_last_error_from_errno();
        return -1;
    }
    return 0;
}

int nuwan_socket_accept(int sockfd, const char* client_addr, int addr_len) {
    (void)client_addr;
    (void)addr_len;
    int client = accept(sockfd, NULL, NULL);
    if (client < 0) {
        set_last_error_from_errno();
        return -1;
    }
    return client;
}

int nuwan_socket_connect(int sockfd, const char* addr, int port) {
    struct sockaddr_in sa;
    struct hostent* host;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);

    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        host = gethostbyname(addr);
        if (!host || !host->h_addr_list || !host->h_addr_list[0]) {
            set_last_error_from_errno();
            return -1;
        }
        memcpy(&sa.sin_addr, host->h_addr_list[0], (size_t)host->h_length);
    }

    if (connect(sockfd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        set_last_error_from_errno();
        return -1;
    }
    return 0;
}

int nuwan_socket_send(int sockfd, const char* buf, int len) {
    int sent = (int)send(sockfd, buf, (size_t)len, 0);
    if (sent < 0) {
        set_last_error_from_errno();
        return -1;
    }
    return sent;
}

int nuwan_socket_recv(int sockfd, const char* buf, int len) {
    int received = (int)recv(sockfd, (void*)buf, (size_t)(len - 1), 0);
    if (received < 0) {
        set_last_error_from_errno();
        return -1;
    }
    ((char*)buf)[received] = '\0';
    return received;
}

int cpx_socket_recv_ptr(int sockfd, void* buf, int len) {
    int received = (int)recv(sockfd, buf, (size_t)(len - 1), 0);
    if (received < 0) {
        set_last_error_from_errno();
        return -1;
    }
    ((char*)buf)[received] = '\0';
    return received;
}

int nuwan_socket_close(int sockfd) {
    if (close(sockfd) != 0) {
        set_last_error_from_errno();
        return -1;
    }
    return 0;
}

int nuwan_socket_set_blocking(int sockfd, int blocking) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0) {
        set_last_error_from_errno();
        return -1;
    }

    if (blocking) {
        flags &= ~O_NONBLOCK;
    } else {
        flags |= O_NONBLOCK;
    }

    if (fcntl(sockfd, F_SETFL, flags) != 0) {
        set_last_error_from_errno();
        return -1;
    }
    return 0;
}

int nuwan_socket_set_reuse_addr(int sockfd, int reuse) {
    int opt = reuse ? 1 : 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        set_last_error_from_errno();
        return -1;
    }
    return 0;
}

int nuwan_socket_set_nodelay(int sockfd, int nodelay) {
    int opt = nodelay ? 1 : 0;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) != 0) {
        set_last_error_from_errno();
        return -1;
    }
    return 0;
}

const char* nuwan_socket_get_last_error(void) {
    return g_last_error;
}

void* ptr_create_buf(int size) {
    return malloc((size_t)size);
}

const char* ptr_to_string(void* p) {
    return (const char*)p;
}

void ptr_free(void* p) {
    free(p);
}
