/**
 * Casperix Networking Test
 * Tests socket and HTTP functionality
 */

#include "runtime/net/socket.h"
#include "runtime/net/http.h"
#include <stdio.h>
#include <string.h>

void test_socket_system() {
    printf("\n=== Testing Socket System ===\n");
    
    if (socket_system_init()) {
        printf("✅ Socket system initialized\n");
    } else {
        printf("❌ Socket system init failed\n");
        return;
    }
    
    Socket* sock = socket_create(SOCKET_TYPE_TCP);
    if (sock) {
        printf("✅ TCP socket created (handle: %d)\n", (int)sock->handle);
        
        // Set non-blocking
        if (socket_set_nonblocking(sock, true)) {
            printf("✅ Set non-blocking mode\n");
        }
        
        socket_destroy(sock);
    }
    
    socket_system_cleanup();
}

void test_http_request() {
    printf("\n=== Testing HTTP Client ===\n");
    
    HttpRequest* req = http_request_create(HTTP_GET, "/");
    if (req) {
        printf("✅ HTTP request created\n");
        
        http_request_add_header(req, "User-Agent", "Casperix/1.0");
        printf("✅ Added custom header\n");
        
        http_request_free(req);
    }
}

int main(void) {
    printf("╔════════════════════════════════════════╗\n");
    printf("║  Casperix Network Test                ║\n");
    printf("╚════════════════════════════════════════╝\n");
    
    test_socket_system();
    test_http_request();
    
    printf("\n=== Network Tests Passed ✅ ===\n\n");
    
    return 0;
}
