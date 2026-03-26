# Casprix ML Inference Backend Implementation Blueprint

## 1. Objective

This document translates the production inference architecture into an implementation plan aligned with the Casprix codebase and C runtime style. It defines modules, data structures, scheduler boundaries, memory ownership rules, protocol layering, and rollout order.

The target is a CPU-first, NUMA-aware, async inference server runtime with:

- event-driven networking
- bounded lock-free queues
- request-scoped arenas
- adaptive batching
- paged KV-cache
- structured observability
- clean integration with the `runtime/ml/` subsystem

This is an implementation blueprint, not a conceptual overview.

---

## 2. Runtime Composition

## 2.1 Process Topology

Per host:

- 1 supervisor/control process
- 1 worker process per NUMA node
- optional edge gateway process if public TLS/QUIC is terminated locally

Per worker process:

- listener shard(s)
- network reactor
- protocol executor
- request admission controller
- model router
- adaptive batchers
- compute scheduler
- tensor engine + model executors
- KV-cache manager
- telemetry exporters

## 2.2 Core Runtime Object

Add a top-level runtime object separate from `CpxRuntime` used by ML kernels.

Suggested type:

```c
typedef struct CpxServerRuntime CpxServerRuntime;
```

Opaque internals should contain:

- global config snapshot
- NUMA node id
- transport vtable
- listener set
- reactor
- coroutine scheduler
- compute scheduler
- model registry
- router table
- batch coordinators
- KV-cache manager
- metrics registry
- log sink
- tracing exporter state
- shutdown controller

This object owns the server lifecycle, while `CpxRuntime` remains the compute/backend runtime.

---

## 3. Suggested Module Layout

Add a dedicated subtree:

- `runtime/server/transport/`
- `runtime/server/proto/`
- `runtime/server/http/`
- `runtime/server/grpc/`
- `runtime/server/ws/`
- `runtime/server/router/`
- `runtime/server/middleware/`
- `runtime/server/admission/`
- `runtime/server/batch/`
- `runtime/server/model/`
- `runtime/server/session/`
- `runtime/server/obs/`
- `runtime/server/control/`

Suggested files:

- `runtime/server/server.h`
- `runtime/server/server.c`
- `runtime/server/config.h`
- `runtime/server/config.c`
- `runtime/server/reactor.h`
- `runtime/server/reactor_epoll.c`
- `runtime/server/reactor_iouring.c`
- `runtime/server/transport.h`
- `runtime/server/conn.h`
- `runtime/server/request.h`
- `runtime/server/response.h`
- `runtime/server/http/http1.c`
- `runtime/server/http/http2.c`
- `runtime/server/http/http3.c`
- `runtime/server/grpc/grpc.c`
- `runtime/server/router/router.c`
- `runtime/server/admission/admission.c`
- `runtime/server/batch/batcher.c`
- `runtime/server/model/registry.c`
- `runtime/server/model/loader.c`
- `runtime/server/model/executor.c`
- `runtime/server/session/kv_directory.c`
- `runtime/server/session/kv_cache_service.c`
- `runtime/server/obs/log_sink.c`
- `runtime/server/obs/metrics.c`
- `runtime/server/obs/trace.c`
- `runtime/server/control/admin.c`
- `runtime/server/control/health.c`

---

## 4. Key Type System

## 4.1 Server Config

```c
typedef struct {
    int listen_threads;
    int io_threads;
    int compute_threads;
    int numa_node;
    bool enable_http1;
    bool enable_http2;
    bool enable_http3;
    bool enable_grpc;
    bool enable_tls;
    bool enable_quic;
    bool enable_iouring;
    size_t max_connections;
    size_t max_inflight_requests;
    size_t conn_read_buffer_bytes;
    size_t conn_write_buffer_bytes;
    size_t request_arena_bytes;
    size_t batch_queue_depth;
    size_t kv_cache_bytes;
    uint32_t default_deadline_ms;
    uint32_t overload_queue_age_ms;
} CpxServerConfig;
```

## 4.2 Request Context

Hot and cold fields must be split.

```c
typedef struct {
    uint64_t request_id;
    uint64_t trace_id_hi;
    uint64_t trace_id_lo;
    uint64_t span_id;
    uint64_t deadline_ns;
    uint32_t tenant_id;
    uint16_t model_slot;
    uint8_t priority;
    uint8_t flags;
    atomic_uint cancel_flag;
} CpxReqHot;

typedef struct {
    CpxReqHot hot;
    CpxArena* arena;
    struct CpxConnection* conn;
    struct CpxModelInstance* model;
    struct CpxBatchQueue* batch_queue;
    struct CpxSessionRef* session;
    struct CpxTraceSpan* root_span;
    struct CpxRequestBody body;
    struct CpxResponseWriter writer;
    struct CpxInferenceEnvelope infer;
} CpxRequestCtx;
```

## 4.3 Connection Context

```c
typedef struct CpxConnection {
    uint64_t conn_id;
    int fd;
    uint8_t proto;
    uint8_t state;
    uint16_t numa_node;
    void* tls_ctx;
    CpxByteSpan read_buf;
    CpxByteSpan write_buf;
    struct CpxEventLoop* loop;
    struct CpxStreamTable* streams;
    uint32_t inflight_streams;
    uint32_t write_credits;
    uint64_t last_active_ns;
} CpxConnection;
```

## 4.4 Batch Queue Entry

```c
typedef struct {
    CpxRequestCtx* req;
    uint64_t enqueue_ns;
    uint32_t est_cost;
    uint16_t prompt_tokens;
    uint16_t max_output_tokens;
    uint8_t phase;
    uint8_t priority;
} CpxBatchEntry;
```

## 4.5 Model Instance

```c
typedef struct CpxModelInstance {
    uint32_t model_id;
    uint32_t version;
    uint16_t numa_node;
    uint16_t scheduler_lane;
    atomic_uint state;
    void* mmap_base;
    size_t mmap_len;
    CpxRuntime* runtime;
    CpxTensorEngine* engine;
    struct CpxKvCache* kv;
    struct CpxModelProfile profile;
} CpxModelInstance;
```

---

## 5. Networking Implementation

## 5.1 Reactor Interface

Use a transport-neutral reactor API:

```c
typedef enum {
    CPX_OP_ACCEPT,
    CPX_OP_READ,
    CPX_OP_WRITE,
    CPX_OP_CLOSE,
    CPX_OP_TIMER,
    CPX_OP_SENDMSG,
    CPX_OP_RECVMSG,
} CpxIoOp;

typedef struct {
    CpxIoOp op;
    int fd;
    void* user_data;
    void* buf;
    size_t len;
    uint64_t deadline_ns;
} CpxIoReq;

typedef struct {
    CpxIoOp op;
    int fd;
    void* user_data;
    ssize_t result;
    uint32_t flags;
} CpxIoCqe;
```

Transport vtable:

```c
typedef struct {
    bool (*init)(void* impl, const CpxServerConfig* cfg);
    bool (*submit)(void* impl, const CpxIoReq* req);
    int  (*poll)(void* impl, CpxIoCqe* out, int max_cqes, int timeout_ms);
    void (*wake)(void* impl);
    void (*destroy)(void* impl);
} CpxReactorVTable;
```

## 5.2 Implementation Order

1. `epoll` first
2. `io_uring` second
3. `kqueue` third
4. QUIC/HTTP3 after transport abstraction stabilizes

This reduces integration risk.

## 5.3 Protocol Stacks

### HTTP/1.1

- incremental parser over ring/slab buffer
- zero-copy header slices
- request body as `CpxByteSpan` or chained spans
- chunked encoding for token streaming fallback

### HTTP/2 / gRPC

- per-connection stream table
- HPACK decode using request arena-backed slices
- flow-control windows tied to response writer credits
- gRPC request metadata translated into internal request envelope

### HTTP/3

- isolated module due to QUIC complexity
- can share request/response abstractions above transport layer

---

## 6. Request Path in Casprix Terms

1. connection accepted by reactor shard
2. parser produces `CpxRequestCtx`
3. middleware extracts model, deadline, session, trace
4. admission controller assigns NUMA-local worker lane
5. request body tokenized or decoded into request arena
6. request pushed into `CpxBatchQueue`
7. batcher forms `CpxBatchPlan`
8. executor invokes `CpxRuntime` + `CpxTensorEngine`
9. response writer streams partial or final output
10. telemetry finalized, arena reset, request returned to pool

---

## 7. Middleware and Routing

## 7.1 Pipeline Shape

For server runtime, use fixed-order middleware stages to minimize dynamic dispatch:

- stage 0: connection metadata
- stage 1: auth / tenant resolution
- stage 2: trace extraction
- stage 3: deadline and priority resolution
- stage 4: model routing
- stage 5: admission control
- stage 6: request-specific transforms
- stage 7: handler dispatch

Representation:

```c
typedef bool (*CpxMiddlewareFn)(CpxRequestCtx* req);

typedef struct {
    CpxMiddlewareFn fns[16];
    int count;
} CpxMiddlewareChain;
```

Avoid per-request heap-built linked middleware chains.

## 7.2 Routing

Routing tables should be immutable snapshots replaced atomically on config or model updates.

Router key:

- endpoint path
- protocol method
- model alias
- tenant override
- session affinity token

---

## 8. Admission, Batching, and Scheduling

## 8.1 Admission Controller

Implement `runtime/server/admission/` with a compact policy engine.

Inputs:

- per-model queue depth
- queue oldest age
- local CPU utilization
- local allocator pressure
- KV-cache free pages
- predicted request cost
- deadline slack

Output:

- admit local
- admit remote
- degrade features
- reject with overload

## 8.2 Batch Queues

Separate queues by:

- model id
- version
- phase: prefill/decode
- priority band
- context bucket

Queue implementation:

- bounded ring buffer
- MPSC for ingress, single-consumer batcher per queue
- cache-line padded counters

## 8.3 Batch Plan

```c
typedef struct {
    uint32_t batch_id;
    uint16_t model_slot;
    uint16_t batch_size;
    uint8_t phase;
    uint8_t priority;
    uint32_t total_prompt_tokens;
    uint32_t max_output_tokens;
    CpxRequestCtx* reqs[128];
} CpxBatchPlan;
```

## 8.4 Compute Scheduler Integration

Do not run inference kernels on reactor threads.

Reactor path submits batch execution tasks to compute scheduler via mailbox:

- batch assembly on batcher thread or coroutine
- execution on compute workers
- completion callback back into response/stream writer queue

Use the existing `runtime/ml/cpx_scheduler.*` as inspiration, but keep server-side scheduler interfaces separate until HPC and server contracts stabilize.

---

## 9. Memory Model

## 9.1 Request Arena

Build server request arena on top of `CpxArena` semantics:

- fixed capacity per request class
- fallback overflow arena only on cold path
- O(1) reset
- spans and descriptors stored in request arena

## 9.2 Connection Buffers

Per-loop slab pools:

- small read buffers for headers
- large chained buffers for bodies
- write-side gather buffer descriptors

## 9.3 Tensor Scratch

Continue using `runtime/ml/cpx_mem_arena.*` for tensor scratch and activations, but do not expose it directly to protocol code.

Bridge object:

```c
typedef struct {
    CpxArena* req_arena;
    CpxMemArena* ml_pool;
    CpxTensorView input_views[16];
    CpxTensorView output_views[16];
} CpxExecMemoryCtx;
```

## 9.4 KV-Cache

Use `runtime/ml/cpx_kvcache.*` as the model-local KV manager.

Server-side session directory maps:

- session id -> model instance
- session id -> KV sequence id
- session id -> owner shard
- session id -> last access timestamp

---

## 10. Model Lifecycle

## 10.1 Registry

A `CpxModelRegistry` should manage immutable snapshots.

```c
typedef struct {
    _Atomic(void*) active_snapshot;
} CpxModelRegistry;
```

Snapshot contains:

- route aliases
- version table
- instance placements
- warmup status
- capacity profiles

## 10.2 Loader

Loader state machine:

- discovered
- mmap_open
- metadata_validated
- runtime_created
- warmed
- serving
- draining
- retired

## 10.3 Hot Reload

Perform RCU-style swap:

- build new snapshot fully
- atomically publish
- old snapshot remains until inflight count drops to zero

---

## 11. Observability Implementation

## 11.1 Logging Integration

Reuse the diagnostic/logging principles already introduced in `src/core/diagnostic.*`, but add a dedicated server log event model.

Suggested log event type:

```c
typedef struct {
    uint64_t ts_ns;
    uint64_t request_id;
    uint64_t trace_id_hi;
    uint64_t trace_id_lo;
    uint32_t model_id;
    uint16_t component;
    uint8_t severity;
    uint8_t stage;
    int32_t status_code;
    uint32_t queue_wait_us;
    uint32_t exec_us;
    const char* message;
} CpxServerLogEvent;
```

Writer path:

- per-thread lock-free log ring
- single async sink thread per process
- JSON or human formatting at sink, not callsite

## 11.2 Metrics

Provide fast-path counters and histograms:

- per-core counters with periodic aggregation
- fixed-bucket histograms for latency
- gauges for queue depth, inflight, KV pages
- exporter thread for Prometheus/OpenTelemetry conversion

## 11.3 Tracing

Tracing should be optional but cheap to propagate:

- trace identifiers in `CpxReqHot`
- span start/stop macros compiled to near-noop when disabled
- sampled detailed decode spans only for selected requests

---

## 12. Fault Tolerance Hooks

Implement server control hooks:

- drain mode flag in `CpxServerRuntime`
- circuit state in router destination records
- timeout wheel or timer heap in reactor
- overload trigger in admission controller
- graceful shutdown sequence:
  1. stop accepts
  2. fail readiness
  3. drain streams
  4. flush logs/metrics
  5. retire model instances

---

## 13. Concrete Phased Build Plan

## Phase A - Foundation

Implement:

- `runtime/server/server.h|c`
- `reactor.h`
- `reactor_epoll.c`
- `conn.h`
- `request.h`
- `response.h`
- `admission.c`
- `router.c`

Outcome:

- basic HTTP/1.1 ingress, request context lifecycle, request arenas, structured logging

## Phase B - Protocol and Streaming

Implement:

- HTTP/2
- gRPC
- stream writer
- cancellation/deadline propagation
- per-connection flow control

Outcome:

- unary and server-streaming inference endpoints

## Phase C - ML Integration

Implement:

- `batcher.c`
- `executor.c`
- `model/registry.c`
- `session/kv_directory.c`
- bridge to `runtime/ml/`

Outcome:

- full request -> batch -> execution -> streaming path

## Phase D - High Performance

Implement:

- `io_uring`
- NUMA-aware routing
- decode/prefill lane separation
- adaptive batching controller
- paged KV-cache ownership and rollback

Outcome:

- low tail latency and high throughput under mixed conversational workloads

## Phase E - Distributed Serving

Implement:

- remote KV shard support
- canary and hot reload control APIs
- trace export
- overload-aware routing across worker pool

---

## 14. Immediate Next Coding Targets

If implemented in the current repository, the first practical coding targets should be:

1. `runtime/server/server.h`
2. `runtime/server/config.h`
3. `runtime/server/reactor.h`
4. `runtime/server/request.h`
5. `runtime/server/response.h`
6. `runtime/server/router/router.h`
7. `runtime/server/admission/admission.h`
8. `runtime/server/batch/batcher.h`

These define the contracts before protocol and backend code diverge.

---

## 15. Design Position

For Casprix, the correct implementation path is:

- keep the ML compute runtime specialized and separate
- add a server runtime layer with independent transport, routing, admission, and observability concerns
- bridge server execution into `runtime/ml/` through explicit batch and execution contracts
- avoid prematurely fusing network/runtime concerns into the ML kernel layer

This preserves mechanical sympathy, keeps APIs clean, and allows the inference backend to scale from single-node workers to distributed conversational serving.
