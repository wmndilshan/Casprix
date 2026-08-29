/**
 * Casprix Runtime — networking hardening tests
 *
 * Covers the three runtime/net/ correctness fixes:
 *   (a) blocking recv()/send() now honour a socket timeout (Slowloris guard)
 *   (b) HTTP request bodies split across TCP segments are fully read
 *       (Content-Length aware), not truncated at \r\n\r\n
 *   (c) large responses are fully sent via a partial-write retry loop
 *
 * Tests (b) and (c) drive http_server_handle_connection() directly on one
 * end of a loopback TCP pair, while the test's own thread plays the client
 * and deliberately controls delivery timing / segmentation. This exercises
 * the real request-read + body-assembly + response-send code paths without
 * depending on the listener/threadpool.
 */

#include "runtime/net/socket.h"
#include "runtime/net/http.h"
#include "runtime/net/http_server.h"
#include "runtime/async/future.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); g_failures++; } \
} while (0)

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Create a connected loopback TCP pair via the socket.h API on one side and a
 * raw fd on the other. *out_server = the accepted Socket*, returns client fd. */
static int loopback_pair(Socket** out_server) {
    Socket* listener = socket_create(SOCKET_TYPE_TCP);
    int reuse = 1;
    socket_set_option(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (!socket_bind(listener, "127.0.0.1", 0) || !socket_listen(listener, 4)) {
        socket_destroy(listener);
        return -1;
    }
    struct sockaddr_in la; socklen_t ll = sizeof(la);
    getsockname(listener->handle, (struct sockaddr*)&la, &ll);

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(cfd, (struct sockaddr*)&la, sizeof(la)) != 0) {
        close(cfd); socket_destroy(listener); return -1;
    }
    Socket* accepted = socket_accept(listener);
    socket_destroy(listener);
    if (!accepted) { close(cfd); return -1; }
    *out_server = accepted;
    return cfd;
}

static int slurp(int fd, char* buf, size_t cap) {
    size_t off = 0;
    for (;;) {
        ssize_t n = recv(fd, buf + off, cap - 1 - off, 0);
        if (n <= 0) break;
        off += (size_t)n;
        if (off >= cap - 1) break;
    }
    buf[off] = '\0';
    return (int)off;
}

/* ---- route handlers -------------------------------------------------- */

static Future* make_ready_future(void* value) {
    Future* f = future_create();
    future_complete(f, value);
    return f;
}

/* Echo the received body length + a checksum, so the test can assert the
 * server saw the FULL and CORRECT body. */
static Future* echo_bodylen_handler(HttpRequest* req, void* ud) {
    (void)ud;
    unsigned long sum = 0;
    for (size_t i = 0; i < req->body_length; i++) sum += (unsigned char)req->body[i];
    char full[192];
    snprintf(full, sizeof(full), "GOT_BODY_LEN=%zu SUM=%lu", req->body_length, sum);
    return make_ready_future(http_response_ok(full));
}

#define BIG_BODY_LEN 200000
static Future* big_body_handler(HttpRequest* req, void* ud) {
    (void)req; (void)ud;
    char* body = (char*)malloc(BIG_BODY_LEN + 1);
    for (int i = 0; i < BIG_BODY_LEN; i++) body[i] = (char)('A' + (i % 26));
    body[BIG_BODY_LEN] = '\0';
    HttpResponse* r = http_response_ok(body);
    free(body);
    return make_ready_future(r);
}

/* The server side runs in a thread so the main thread can drive the client
 * with controlled timing while the handler blocks on recv(). */
typedef struct { HttpServer* server; Socket* client; } HandlerArg;
static void* run_handler(void* arg) {
    HandlerArg* h = (HandlerArg*)arg;
    http_server_handle_connection(h->server, h->client); /* closes h->client */
    return NULL;
}

/* ---- (a) socket timeout ------------------------------------------------- */

static void test_socket_timeout(void) {
    printf("\n=== (a) blocking recv() honours SO_RCVTIMEO ===\n");
    socket_system_init();

    Socket* server = NULL;
    int cfd = loopback_pair(&server);
    CHECK(cfd >= 0 && server != NULL, "loopback pair established");

    /* Without a timeout this recv() blocks forever (client sends nothing). */
    CHECK(socket_set_timeout(server, 500), "socket_set_timeout(500ms) succeeded");

    char buf[64];
    double t0 = now_sec();
    int n = socket_recv(server, buf, sizeof(buf));
    double elapsed = now_sec() - t0;

    CHECK(n <= 0, "recv() returned <=0 (timed out) instead of hanging");
    CHECK(elapsed >= 0.4 && elapsed < 5.0, "recv() unblocked near the 500ms deadline");
    printf("       (elapsed %.3fs, recv rc=%d, last_error=%d EAGAIN=%d)\n",
           elapsed, n, socket_get_error(server), EAGAIN);

    CHECK(socket_set_timeout(server, 0), "socket_set_timeout(0) clears timeout");
    send(cfd, "x", 1, 0);
    n = socket_recv(server, buf, sizeof(buf));
    CHECK(n == 1, "recv() after clearing timeout still works");

    socket_destroy(server);
    close(cfd);
    socket_system_cleanup();
}

/* ---- (b) body split across TCP segments ------------------------------- */

static void test_split_body(void) {
    printf("\n=== (b) Content-Length body arriving in later TCP packets ===\n");
    socket_system_init();

    HttpServer* server = http_server_create("127.0.0.1", 0);
    http_server_route(server, HTTP_POST, "/echo", echo_bodylen_handler, NULL);

    Socket* srv_sock = NULL;
    int cfd = loopback_pair(&srv_sock);
    CHECK(cfd >= 0, "loopback pair established");

    const char* body = "This-is-a-request-body-delivered-in-slow-dribbles-0123456789-abcdef";
    size_t body_len = strlen(body);

    char headers[256];
    snprintf(headers, sizeof(headers),
             "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n", body_len);

    HandlerArg ha = { server, srv_sock };
    pthread_t tid;
    pthread_create(&tid, NULL, run_handler, &ha);

    /* Headers in their own segment, then a pause: handler must NOT finalize
     * the body from the header read alone. */
    send(cfd, headers, strlen(headers), 0);
    usleep(150000);

    int nodelay = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    for (size_t i = 0; i < body_len; i += 7) {
        size_t chunk = (body_len - i < 7) ? (body_len - i) : 7;
        send(cfd, body + i, chunk, 0);
        usleep(15000);
    }

    char resp[1024];
    slurp(cfd, resp, sizeof(resp));
    close(cfd);
    pthread_join(tid, NULL);

    unsigned long sum = 0;
    for (size_t i = 0; i < body_len; i++) sum += (unsigned char)body[i];
    char expect[128];
    snprintf(expect, sizeof(expect), "GOT_BODY_LEN=%zu SUM=%lu", body_len, sum);

    CHECK(strstr(resp, " 200 ") != NULL, "response is 200 OK");
    CHECK(strstr(resp, expect) != NULL, "server read the FULL and CORRECT body");
    printf("       (expected marker: '%s')\n", expect);
    if (!strstr(resp, expect)) printf("       full response:\n%s\n", resp);

    http_server_destroy(server);
    socket_system_cleanup();
}

/* ---- (b2) body fully present in the first read (no regression) -------- */

static void test_body_single_read(void) {
    printf("\n=== (b2) body fully in first packet still works ===\n");
    socket_system_init();

    HttpServer* server = http_server_create("127.0.0.1", 0);
    http_server_route(server, HTTP_POST, "/echo", echo_bodylen_handler, NULL);

    Socket* srv_sock = NULL;
    int cfd = loopback_pair(&srv_sock);

    const char* body = "compact-body-1234567890";
    size_t body_len = strlen(body);
    char req[512];
    snprintf(req, sizeof(req),
             "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n%s",
             body_len, body);

    HandlerArg ha = { server, srv_sock };
    pthread_t tid;
    pthread_create(&tid, NULL, run_handler, &ha);
    send(cfd, req, strlen(req), 0);   /* headers + body in one write */

    char resp[1024];
    slurp(cfd, resp, sizeof(resp));
    close(cfd);
    pthread_join(tid, NULL);

    unsigned long sum = 0;
    for (size_t i = 0; i < body_len; i++) sum += (unsigned char)body[i];
    char expect[128];
    snprintf(expect, sizeof(expect), "GOT_BODY_LEN=%zu SUM=%lu", body_len, sum);
    CHECK(strstr(resp, expect) != NULL, "single-read body parsed correctly");

    http_server_destroy(server);
    socket_system_cleanup();
}

/* ---- (c) large response fully sent ----------------------------------- */

static void test_large_response(void) {
    printf("\n=== (c) large response body fully delivered (partial-send retry) ===\n");
    socket_system_init();

    HttpServer* server = http_server_create("127.0.0.1", 0);
    http_server_route(server, HTTP_GET, "/big", big_body_handler, NULL);

    Socket* srv_sock = NULL;
    int cfd = loopback_pair(&srv_sock);
    CHECK(cfd >= 0, "loopback pair established");

    /* Shrink the client's receive buffer so the server's send() socket buffer
     * fills quickly and send() is forced to return partial counts. */
    int rcvbuf = 4096;
    setsockopt(cfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    HandlerArg ha = { server, srv_sock };
    pthread_t tid;
    pthread_create(&tid, NULL, run_handler, &ha);

    const char* req = "GET /big HTTP/1.1\r\nHost: x\r\n\r\n";
    send(cfd, req, strlen(req), 0);

    /* Drain slowly with sleeps to keep back-pressure on the sender. */
    char* resp = malloc(BIG_BODY_LEN + 8192);
    size_t off = 0;
    for (;;) {
        ssize_t n = recv(cfd, resp + off, 2048, 0);
        if (n <= 0) break;
        off += (size_t)n;
        usleep(500);
        if (off >= (size_t)BIG_BODY_LEN + 8000) break;
    }
    resp[off] = '\0';
    close(cfd);
    pthread_join(tid, NULL);

    char* hdr_end = strstr(resp, "\r\n\r\n");
    size_t got_body = hdr_end ? (off - (size_t)(hdr_end + 4 - resp)) : 0;

    CHECK(strstr(resp, " 200 ") != NULL, "response is 200 OK");
    CHECK(strstr(resp, "Content-Length: 200000") != NULL, "Content-Length header is 200000");
    CHECK(got_body == BIG_BODY_LEN, "received body is exactly 200000 bytes (no truncation)");
    printf("       (received %zu body bytes of %d)\n", got_body, BIG_BODY_LEN);

    int tail_ok = (hdr_end && got_body == BIG_BODY_LEN);
    if (tail_ok) {
        const char* b = hdr_end + 4;
        for (int i = BIG_BODY_LEN - 200; i < BIG_BODY_LEN; i++) {
            if (b[i] != (char)('A' + (i % 26))) { tail_ok = 0; break; }
        }
    }
    CHECK(tail_ok, "final 200 bytes of body are byte-exact");

    free(resp);
    http_server_destroy(server);
    socket_system_cleanup();
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=====================================================\n");
    printf("  Casprix networking hardening tests (runtime/net/)\n");
    printf("=====================================================\n");

    test_socket_timeout();
    test_split_body();
    test_body_single_read();
    test_large_response();

    printf("\n-----------------------------------------------------\n");
    if (g_failures == 0) {
        printf("ALL NETWORKING HARDENING TESTS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
