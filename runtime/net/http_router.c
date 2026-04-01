/**
 * Synchronous HTTP route table (CpxRouter)
 */

#include "http_server.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#if !defined(_WIN32)
#include <strings.h>
#endif

static int cpx_stricmp_local(const char* a, const char* b) {
#if defined(_WIN32)
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}

static const char* http_method_name(HttpMethod m) {
    switch (m) {
        case HTTP_GET:    return "GET";
        case HTTP_POST:   return "POST";
        case HTTP_PUT:    return "PUT";
        case HTTP_DELETE: return "DELETE";
        case HTTP_HEAD:   return "HEAD";
        default:          return "GET";
    }
}

void cpx_router_init(CpxRouter* r) {
    if (!r) return;
    memset(r, 0, sizeof(*r));
}

void cpx_router_add(CpxRouter* r, const char* method, const char* path,
                    CpxSyncHttpHandler handler, void* userdata) {
    if (!r || !method || !path || !handler || r->route_count >= 64) return;

    CpxRoute* rt = &r->routes[r->route_count++];
    memset(rt, 0, sizeof(*rt));
    strncpy(rt->method, method, sizeof(rt->method) - 1);
    strncpy(rt->path, path, sizeof(rt->path) - 1);
    rt->handler  = handler;
    rt->userdata = userdata;
}

static void cpx_fill_404(HttpResponse* res) {
    if (!res) return;
    res->status_code = 404;
    free(res->status_text);
    res->status_text = strdup("Not Found");
    free(res->body);
    res->body = strdup("Not Found");
    res->body_length = res->body ? strlen(res->body) : 0;
}

void cpx_router_dispatch(CpxRouter* r, HttpRequest* req, HttpResponse* res) {
    if (!r || !req || !res) return;

    const char* m = http_method_name(req->method);
    const char* p = req->path ? req->path : "/";

    for (int i = 0; i < r->route_count; i++) {
        CpxRoute* rt = &r->routes[i];
        if (cpx_stricmp_local(rt->method, m) != 0) continue;
        if (strcmp(rt->path, p) != 0) continue;
        rt->handler(req, res, rt->userdata);
        return;
    }

    cpx_fill_404(res);
}
