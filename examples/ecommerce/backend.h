#ifndef CPX_ECOMMERCE_BACKEND_H
#define CPX_ECOMMERCE_BACKEND_H

#include "runtime/net/http_server.h"

typedef struct EcommerceBackend EcommerceBackend;

EcommerceBackend* ecommerce_backend_create(const char* address, uint16_t port,
                                            size_t worker_count);
bool ecommerce_backend_listen(EcommerceBackend* backend);
void ecommerce_backend_stop(EcommerceBackend* backend);
bool ecommerce_backend_is_running(const EcommerceBackend* backend);
void ecommerce_backend_destroy(EcommerceBackend* backend);
HttpServerStats ecommerce_backend_stats(const EcommerceBackend* backend);

/* Small FFI entry point used by backend.cpx. */
int ecommerce_backend_run(int port, int worker_count);

#endif
