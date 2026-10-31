#include "backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION EcommerceMutex;
static void mutex_init(EcommerceMutex* mutex) { InitializeCriticalSection(mutex); }
static void mutex_lock(EcommerceMutex* mutex) { EnterCriticalSection(mutex); }
static void mutex_unlock(EcommerceMutex* mutex) { LeaveCriticalSection(mutex); }
static void mutex_destroy(EcommerceMutex* mutex) { DeleteCriticalSection(mutex); }
#else
#include <pthread.h>
typedef pthread_mutex_t EcommerceMutex;
static void mutex_init(EcommerceMutex* mutex) { pthread_mutex_init(mutex, NULL); }
static void mutex_lock(EcommerceMutex* mutex) { pthread_mutex_lock(mutex); }
static void mutex_unlock(EcommerceMutex* mutex) { pthread_mutex_unlock(mutex); }
static void mutex_destroy(EcommerceMutex* mutex) { pthread_mutex_destroy(mutex); }
#endif

typedef struct Product {
    int id;
    const char* name;
    int price_cents;
    int inventory;
} Product;

typedef struct EcommerceState {
    Product products[3];
    int cart_product;
    int cart_quantity;
    unsigned long long next_order;
    EcommerceMutex mutex;
} EcommerceState;

struct EcommerceBackend {
    HttpServer* server;
    EcommerceState state;
};

static HttpResponse* json_status(int status, const char* status_text,
                                 const char* json) {
    HttpResponse* response = http_response_json(json);
    if (!response) return NULL;
    response->status_code = status;
    free(response->status_text);
    response->status_text = strdup(status_text);
    return response;
}

static int query_integer(const char* path, const char* key, int fallback) {
    const char* query = strchr(path, '?');
    if (!query) return fallback;
    size_t key_length = strlen(key);
    query++;
    while (*query) {
        if (strncmp(query, key, key_length) == 0 && query[key_length] == '=') {
            char* end = NULL;
            long value = strtol(query + key_length + 1, &end, 10);
            return end == query + key_length + 1 ? fallback : (int)value;
        }
        query = strchr(query, '&');
        if (!query) break;
        query++;
    }
    return fallback;
}

static HttpResponse* health_handler(HttpRequest* request, void* user_data) {
    (void)request;
    (void)user_data;
    return http_response_json("{\"status\":\"ok\",\"service\":\"casprix-store\"}");
}

static HttpResponse* products_handler(HttpRequest* request, void* user_data) {
    (void)request;
    EcommerceState* state = (EcommerceState*)user_data;
    char json[768];
    mutex_lock(&state->mutex);
    snprintf(json, sizeof(json),
             "{\"products\":["
             "{\"id\":1,\"name\":\"Keyboard\",\"price_cents\":%d,\"inventory\":%d},"
             "{\"id\":2,\"name\":\"Mouse\",\"price_cents\":%d,\"inventory\":%d},"
             "{\"id\":3,\"name\":\"Monitor\",\"price_cents\":%d,\"inventory\":%d}]}",
             state->products[0].price_cents, state->products[0].inventory,
             state->products[1].price_cents, state->products[1].inventory,
             state->products[2].price_cents, state->products[2].inventory);
    mutex_unlock(&state->mutex);
    return http_response_json(json);
}

static HttpResponse* cart_get_handler(HttpRequest* request, void* user_data) {
    (void)request;
    EcommerceState* state = (EcommerceState*)user_data;
    char json[256];
    mutex_lock(&state->mutex);
    snprintf(json, sizeof(json), "{\"product_id\":%d,\"quantity\":%d}",
             state->cart_product, state->cart_quantity);
    mutex_unlock(&state->mutex);
    return http_response_json(json);
}

static HttpResponse* cart_add_handler(HttpRequest* request, void* user_data) {
    EcommerceState* state = (EcommerceState*)user_data;
    int product_id = query_integer(request->path, "product", 0);
    int quantity = query_integer(request->path, "quantity", 1);
    if (product_id < 1 || product_id > 3 || quantity < 1 || quantity > 20) {
        return json_status(400, "Bad Request", "{\"error\":\"invalid product or quantity\"}");
    }

    mutex_lock(&state->mutex);
    Product* product = &state->products[product_id - 1];
    if (quantity > product->inventory) {
        mutex_unlock(&state->mutex);
        return json_status(409, "Conflict", "{\"error\":\"insufficient inventory\"}");
    }
    state->cart_product = product_id;
    state->cart_quantity += quantity;
    int cart_quantity = state->cart_quantity;
    mutex_unlock(&state->mutex);

    char json[256];
    snprintf(json, sizeof(json), "{\"product_id\":%d,\"quantity\":%d}",
             product_id, cart_quantity);
    return json_status(201, "Created", json);
}

static HttpResponse* order_handler(HttpRequest* request, void* user_data) {
    (void)request;
    EcommerceState* state = (EcommerceState*)user_data;
    char json[256];
    mutex_lock(&state->mutex);
    if (state->cart_product == 0 || state->cart_quantity == 0) {
        mutex_unlock(&state->mutex);
        return json_status(400, "Bad Request", "{\"error\":\"cart is empty\"}");
    }
    Product* product = &state->products[state->cart_product - 1];
    if (state->cart_quantity > product->inventory) {
        mutex_unlock(&state->mutex);
        return json_status(409, "Conflict", "{\"error\":\"insufficient inventory\"}");
    }
    product->inventory -= state->cart_quantity;
    unsigned long long order_id = state->next_order++;
    int total = product->price_cents * state->cart_quantity;
    state->cart_product = 0;
    state->cart_quantity = 0;
    mutex_unlock(&state->mutex);
    snprintf(json, sizeof(json),
             "{\"order_id\":%llu,\"status\":\"confirmed\",\"total_cents\":%d}",
             order_id, total);
    return json_status(201, "Created", json);
}

EcommerceBackend* ecommerce_backend_create(const char* address, uint16_t port,
                                            size_t worker_count) {
    EcommerceBackend* backend = (EcommerceBackend*)calloc(1, sizeof(*backend));
    if (!backend) return NULL;
    backend->state.products[0] = (Product){1, "Keyboard", 7900, 10000};
    backend->state.products[1] = (Product){2, "Mouse", 3900, 10000};
    backend->state.products[2] = (Product){3, "Monitor", 24900, 10000};
    backend->state.next_order = 1000;
    mutex_init(&backend->state.mutex);

    HttpServerConfig config = http_server_default_config();
    config.worker_count = worker_count;
    config.backlog = 2048;
    config.io_timeout_ms = 3000;
    config.max_request_body = 64u * 1024u;
    backend->server = http_server_create_with_config(address, port, &config);
    if (!backend->server ||
        !http_server_route_sync(backend->server, HTTP_GET, "/health", health_handler, &backend->state) ||
        !http_server_route_sync(backend->server, HTTP_GET, "/api/products", products_handler, &backend->state) ||
        !http_server_route_sync(backend->server, HTTP_GET, "/api/cart", cart_get_handler, &backend->state) ||
        !http_server_route_sync(backend->server, HTTP_POST, "/api/cart", cart_add_handler, &backend->state) ||
        !http_server_route_sync(backend->server, HTTP_POST, "/api/orders", order_handler, &backend->state)) {
        ecommerce_backend_destroy(backend);
        return NULL;
    }
    return backend;
}

bool ecommerce_backend_listen(EcommerceBackend* backend) {
    return backend && http_server_listen(backend->server);
}

void ecommerce_backend_stop(EcommerceBackend* backend) {
    if (backend) http_server_stop(backend->server);
}

bool ecommerce_backend_is_running(const EcommerceBackend* backend) {
    return backend && http_server_is_running(backend->server);
}

HttpServerStats ecommerce_backend_stats(const EcommerceBackend* backend) {
    return backend ? http_server_get_stats(backend->server) : (HttpServerStats){0};
}

void ecommerce_backend_destroy(EcommerceBackend* backend) {
    if (!backend) return;
    http_server_destroy(backend->server);
    mutex_destroy(&backend->state.mutex);
    free(backend);
}

int ecommerce_backend_run(int port, int worker_count) {
    EcommerceBackend* backend = ecommerce_backend_create("0.0.0.0", (uint16_t)port,
                                                          (size_t)worker_count);
    if (!backend) return 1;
    printf("Casprix store listening on http://0.0.0.0:%d\n", port);
    bool ok = ecommerce_backend_listen(backend);
    ecommerce_backend_destroy(backend);
    return ok ? 0 : 1;
}

#ifdef CPX_ECOMMERCE_STANDALONE
int main(int argc, char** argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    int workers = argc > 2 ? atoi(argv[2]) : 0;
    return ecommerce_backend_run(port, workers);
}
#endif
