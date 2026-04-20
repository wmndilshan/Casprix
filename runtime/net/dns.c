/**
 * Minimal UDP DNS A-record resolver (recursive resolver via 8.8.8.8 fallback)
 */

#include "dns.h"
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET cpx_sock_t;
#define CPX_INVALID_SOCKET INVALID_SOCKET
#define cpx_closesocket closesocket
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
typedef int cpx_sock_t;
#define CPX_INVALID_SOCKET (-1)
#define cpx_closesocket close
#endif

static void dns_build_query(const char* host, uint8_t* buf, size_t* len_out, uint16_t txid) {
    size_t pos = 0;
    buf[pos++] = (uint8_t)(txid >> 8);
    buf[pos++] = (uint8_t)(txid & 0xFF);
    buf[pos++] = 0x01; /* flags: standard query */
    buf[pos++] = 0x00;
    buf[pos++] = 0x00; buf[pos++] = 0x01; /* QDCOUNT */
    buf[pos++] = 0x00; buf[pos++] = 0x00;
    buf[pos++] = 0x00; buf[pos++] = 0x00;
    buf[pos++] = 0x00; buf[pos++] = 0x00;

    const char* p = host;
    while (*p) {
        const char* dot = strchr(p, '.');
        size_t lab = dot ? (size_t)(dot - p) : strlen(p);
        if (lab > 63) lab = 63;
        buf[pos++] = (uint8_t)lab;
        memcpy(buf + pos, p, lab);
        pos += lab;
        if (!dot) break;
        p = dot + 1;
    }
    buf[pos++] = 0;
    buf[pos++] = 0; buf[pos++] = 1; /* A */
    buf[pos++] = 0; buf[pos++] = 1; /* IN */
    *len_out = pos;
}

int cpx_dns_resolve(const char* hostname, uint8_t out_ip[4], uint32_t timeout_ms) {
    if (!hostname || !out_ip) return -1;

#ifdef _WIN32
    static int wsa_once = 0;
    if (!wsa_once) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return -1;
        wsa_once = 1;
    }
#endif

    uint8_t qbuf[512];
    size_t qlen = 0;
    uint16_t txid = (uint16_t)(rand() & 0xFFFF);
    dns_build_query(hostname, qbuf, &qlen, txid);

    cpx_sock_t fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == CPX_INVALID_SOCKET) return -1;

#ifdef _WIN32
    DWORD tv = timeout_ms;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec  = (int)(timeout_ms / 1000u);
    tv.tv_usec = (int)((timeout_ms % 1000u) * 1000);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(53);
    dst.sin_addr.s_addr = inet_addr("8.8.8.8");

    if (sendto(fd, (const char*)qbuf, (int)qlen, 0,
               (struct sockaddr*)&dst, (int)sizeof(dst)) < 0) {
        cpx_closesocket(fd);
        return -1;
    }

    uint8_t rbuf[1024];
    int rlen;

#ifdef _WIN32
    rlen = recvfrom(fd, (char*)rbuf, sizeof(rbuf), 0, NULL, NULL);
    if (rlen == SOCKET_ERROR || rlen < 12) {
        cpx_closesocket(fd);
        return -1;
    }
#else
    {
        struct pollfd pfd;
        pfd.fd      = fd;
        pfd.events  = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, (int)timeout_ms) <= 0) {
            cpx_closesocket(fd);
            return -1;
        }
    }
    {
        ssize_t n = recvfrom(fd, rbuf, sizeof(rbuf), 0, NULL, NULL);
        if (n < 12) {
            cpx_closesocket(fd);
            return -1;
        }
        rlen = (int)n;
    }
#endif

    cpx_closesocket(fd);

    if (rbuf[0] != (uint8_t)(txid >> 8) || rbuf[1] != (uint8_t)(txid & 0xFF)) return -1;
    if ((rbuf[3] & 0x0F) != 0) return -1; /* RCODE */

    /* Skip header + question section */
    size_t i = 12;
    while (i < (size_t)rlen && rbuf[i] != 0) {
        size_t lab = rbuf[i];
        i += 1 + lab;
    }
    if (i + 4 > (size_t)rlen) return -1;
    i += 5; /* null + QTYPE + QCLASS */

    /* Parse answers */
    uint16_t ancount = (uint16_t)((rbuf[6] << 8) | rbuf[7]);
    for (uint16_t a = 0; a < ancount && i < (size_t)rlen; a++) {
        if (i + 12 > (size_t)rlen) return -1;
        if ((rbuf[i] & 0xC0) == 0xC0)
            i += 2;
        else {
            while (i < (size_t)rlen && rbuf[i]) {
                size_t lab = rbuf[i];
                i += 1 + lab;
            }
            i++;
        }
        uint16_t typ = (uint16_t)((rbuf[i] << 8) | rbuf[i + 1]);
        i += 8;
        uint16_t rdlen = (uint16_t)((rbuf[i] << 8) | rbuf[i + 1]);
        i += 2;
        if (typ == 1 && rdlen == 4 && i + 4 <= (size_t)rlen) {
            out_ip[0] = rbuf[i];
            out_ip[1] = rbuf[i + 1];
            out_ip[2] = rbuf[i + 2];
            out_ip[3] = rbuf[i + 3];
            return 0;
        }
        i += rdlen;
    }
    return -1;
}
