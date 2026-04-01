#ifndef CASPRIX_DNS_H
#define CASPRIX_DNS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve hostname to IPv4 (A record). out_ip[4] network byte order. Returns 0 on success. */
int cpx_dns_resolve(const char* hostname, uint8_t out_ip[4], uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
