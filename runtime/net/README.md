# Casprix network runtime

The portable HTTP path is `socket` + `http` + `http_server`. It uses the shared
`runtime/async` scheduler and `runtime/io/event_loop`; networking does not own a
second thread pool or reactor. The listener is non-blocking and readiness-driven.
Accepted connections run on bounded workers with per-operation timeouts so
existing synchronous route handlers remain compatible and slow clients cannot
block the event-loop thread.

## Server defaults

- backlog: 1024
- I/O timeout: 5 seconds
- maximum request body: 1 MiB
- workers: detected CPU count, capped at 64
- maximum queued worker tasks: 65,536
- connection policy: one HTTP request per connection, then close

Use `http_server_create_with_config` to tune these values. Synchronous handlers
registered with `http_server_route_sync` avoid allocating a `Future` and native
synchronization objects for handlers that already have an immediate response.

The parser rejects unsupported methods, incomplete/oversized headers, duplicate
`Content-Length`, invalid lengths, unsupported HTTP versions and bodies above the
configured limit. Shutdown removes the listener watcher, rejects new tasks, drains accepted
connections and only then releases routes and application state.

## Load benchmark

`tests/runtime/bench_http_concurrency.c` drives the store's product endpoint with
new TCP connections and checks every response. Arguments are request count,
client concurrency and server worker count:

```text
bench_http_concurrency 50000 512 64
```

On the development Windows machine, loopback runs completed all 50,000 requests
with 512 concurrent clients and zero failures/rejections, with observed throughput
around 11,400 requests/second for the shared-runtime path (and roughly
9,900-13,800 across local compiler/build variations). This is a
local regression baseline, not an internet or
production capacity guarantee; TLS, persistence, database latency, kernel limits
and hardware materially change the result.
