> **STATUS: design proposal — not yet implemented.**
>
> This document describes a planned system. It is kept for the design
> thinking it records, not as a description of code that exists today.
> Parts of the runtime (`runtime/net/`, `runtime/ai/ml/`) contain early
> building blocks, but the architecture below is largely unbuilt. Do not
> treat any section here as current documentation.

# Casprix General-Purpose Backend Framework Architecture

## 1. Objective

Design a general-purpose backend framework and server runtime comparable in scope to Spring Boot, ASP.NET Core, or NestJS, but engineered for lower overhead, stronger control over memory and scheduling, and modern async execution.

The framework is not ML-specific. It is intended for HTTP APIs, gRPC services, streaming applications, background processing, and service-to-service backends.

Primary goals:

- extremely high throughput
- low median and tail latency
- massive connection concurrency
- bounded memory behavior
- modular extensibility
- production-grade observability
- strong typing with low runtime reflection cost

---

## 2. Core Framework Philosophy

## 2.1 Guiding Principles

### Convention over configuration

Defaults should make the common path trivial:

- convention-based module discovery
- opinionated bootstrapping
- standard middleware ordering
- standard config precedence
- standard health/metrics/logging endpoints

Configuration remains explicit but layered, with conventions minimizing boilerplate.

### Minimal runtime overhead

Framework code must not dominate request critical path.

Rules:

- fixed-layout hot structs
- bounded allocations in hot path
- no general reflection during request handling
- async state machines compiled or generated ahead of time where possible
- immutable route and DI graphs after boot

### Strong typing

Service definitions, configuration objects, request bindings, serializers, and DI registrations should be type-safe.

Type information is exploited at:

- compile-time for code generation and validation
- startup for graph freezing
- runtime only where dynamic extensibility is unavoidable

### Middleware-first architecture

The framework core is a request pipeline that composes:

- transport handlers
- middleware
- routing
- filters/interceptors
- endpoint invokers
- serializers

### Modular plugin system

Every major subsystem is replaceable behind stable interfaces:

- transport
- serializer
- config provider
- logger
- tracer
- database driver
- scheduler
- DI extensions

## 2.2 Developer Ergonomics vs Performance

The framework should deliberately trade some maximum dynamism for predictable performance.

Prefer:

- generated binding code over reflective field walking
- startup graph compilation over runtime lookup chains
- immutable route tables over late-bound handler chains
- explicit middleware ordering over implicit dynamic composition

Allow ergonomic APIs only if they compile down to stable execution plans.

## 2.3 Compile-Time vs Runtime Reflection

### Compile-time reflection / metadata generation

Use for:

- route descriptors
- DI graph registration tables
- validation schemas
- serializer metadata
- config binding metadata

Benefits:

- lower per-request overhead
- stronger static guarantees
- smaller hot-path branching surface

### Runtime reflection

Use sparingly for:

- admin tooling
- plugin inspection
- diagnostics
- dynamic config reload maps

Production position:

- compile-time metadata first
- runtime reflection confined to cold paths and tooling

---

## 3. Top-Level Architecture

## 3.1 Runtime Layers

1. **Host runtime**
   - process boot
   - config load
   - DI composition root
   - module lifecycle
   - signal handling

2. **Transport runtime**
   - listeners
   - reactor
   - protocol stacks
   - TLS
   - connection state

3. **Application runtime**
   - middleware pipeline
   - routing
   - endpoint activation
   - request context
   - response writing

4. **Service runtime**
   - DB clients
   - cache clients
   - event bus
   - job scheduler
   - service discovery integrations

5. **Observability runtime**
   - logging
   - tracing
   - metrics
   - profiling hooks

## 3.2 Execution Domains

Use distinct execution domains:

- **I/O domain** for socket and protocol progress
- **async task domain** for application continuations
- **blocking domain** for legacy drivers, file I/O, compression, crypto fallback
- **background domain** for timers, jobs, maintenance

This prevents untrusted application work from stalling transport progress.

---

## 4. Networking Stack Design

## 4.1 Server Model

Use a Reactor + coroutine executor architecture.

- reactor handles readiness/completion events
- coroutines represent connection, stream, and request state machines
- work handoff occurs only when necessary

Transport abstraction supports:

- `epoll`
- `kqueue`
- `io_uring`
- IOCP on Windows

Recommended policy:

- `epoll` / `kqueue` as baseline
- `io_uring` as optimized Linux path
- keep abstraction narrow to avoid lowest-common-denominator penalties

## 4.2 Protocol Support

### HTTP/1.1

- keep-alive support
- bounded parser state
- chunked streaming
- pipelining constrained or disabled in default mode

### HTTP/2

- multiplexed streams
- per-stream flow control
- header compression
- ideal for internal RPC and gRPC

### HTTP/3

- QUIC-based transport
- optional edge-facing deployment
- connection migration and packet-loss resilience
- used where its operational complexity is justified

### WebSocket

- upgrade from HTTP/1.1 or HTTP/2-compatible fallback where applicable
- framed full-duplex streams
- backpressure-aware send queue
- suitable for low-latency event delivery

### gRPC

- unary
- client streaming
- server streaming
- bidirectional streaming
- shared request context and deadline propagation with HTTP stack

## 4.3 Zero-Copy Request Parsing

Parser design:

- input is an immutable or append-only buffer chain
- parser stores offsets/slices into transport buffer
- header keys and values are referenced, not copied, until application binding requires transformation
- request body remains in slab-backed chained spans

Copy only when:

- transcoding is required
- lifetime exceeds connection buffer scope
- content transformation or validation demands isolation

## 4.4 TLS Termination

Two modes:

### Edge termination

- preferred for public traffic
- simplifies certificate management
- offloads QUIC/TLS handshake cost
- allows internal HTTP/2 or gRPC over mTLS

### In-process termination

- useful for private clusters or simplified topologies
- lower hop count
- higher CPU competition with application workloads

Framework support should allow both without changing application code.

## 4.5 Connection Pooling

Client-side pooling for outbound RPC/DB/cache clients should include:

- connection warm pools
- per-endpoint max inflight limits
- health-aware leasing
- idle eviction
- HTTP/2 multiplexed channel preference

## 4.6 Backpressure Control

Backpressure boundaries:

- socket receive buffer
- protocol decode queue
- middleware/application queue
- outbound serializer/write queue
- downstream client pool queue

Policy:

- stop reading when application queue exceeds threshold
- reduce stream windows under sustained pressure
- reject or defer low-priority work
- propagate deadlines to downstream calls

## 4.7 Kernel-Bypass Option

Kernel bypass is optional and isolated behind a transport plugin.

Use only for specialized deployments with:

- dedicated NIC queues
- controlled workloads
- no strong dependency on service mesh and generic TLS stacks

Default production path remains kernel sockets due to operational simplicity.

## 4.8 Request Lifecycle

1. socket accepted on local event loop
2. transport fills read buffer
3. parser builds request metadata slices
4. middleware pipeline executes
5. router resolves endpoint
6. request binder builds typed handler inputs
7. handler or interceptor chain runs
8. response serialized
9. write queue flushes with flow-control awareness
10. context finalized and memory reset/reused

---

## 5. Concurrency Runtime Design

## 5.1 Coroutine-Based Async Execution

Requests are represented by coroutines or generated async state machines.

Coroutines should support:

- suspend on I/O
- suspend on timers/deadlines
- await outbound dependencies
- inherit request context and cancellation

Rules:

- no blocking operations on reactor threads
- CPU-heavy work must move to worker pool or blocking domain
- user middleware can be async without owning scheduling details

## 5.2 Work-Stealing Scheduler

Application tasks and background jobs run on a work-stealing executor:

- per-worker local deque
- LIFO local pop for cache locality
- FIFO steal for balancing
- bounded task allocation pools

This scheduler is separate from transport reactor threads.

## 5.3 Lock-Free Queues

Use bounded lock-free queues for:

- reactor -> application executor
- outbound completion notifications
- logger event ingestion
- metrics event aggregation
- job dispatch mailboxes

Guidelines:

- ring buffer over linked-list queue in hot paths
- cache-line padded counters
- avoid unbounded queue growth

## 5.4 Thread Pinning Strategy

Thread roles:

- listener/reactor threads pinned to dedicated cores when latency-sensitive
- async worker threads pinned in balanced spread across cores
- blocking pool unpinned or lightly pinned
- background maintenance isolated from critical cores

## 5.5 NUMA-Aware Scheduling

Per NUMA node:

- local worker pool
- local allocator arenas
- local socket accept sharding
- local connection buffers

Affinity rules:

- keep connection and request processing on same node whenever possible
- keep outbound client pools NUMA-local
- cross-node handoff only for overload or resource affinity constraints

## 5.6 Structured Concurrency

Every request defines a scope containing:

- handler task
- child middleware continuations
- downstream client calls
- stream writers
- timers

When the scope completes or cancels:

- children are cancelled
- context finalizers run
- pooled resources return deterministically

## 5.7 Cancellation Propagation

Cancellation sources:

- client disconnect
- deadline exceeded
- server shutdown
- parent scope cancellation

Propagation rules:

- request scope owns cancellation token
- all awaited downstream calls inherit token
- streaming writers and job submissions observe token
- cleanup is idempotent and non-blocking

## 5.8 Deadline-Aware Execution

Request context carries an absolute deadline.

Scheduler decisions may incorporate:

- remaining slack
- service class
- queue age
- downstream call budget

Expired work should be aborted before expensive serialization or downstream retries.

## 5.9 Async vs Thread-per-Request

### Thread-per-request

Pros:

- easy mental model
- easy compatibility with blocking code

Cons:

- high stack memory cost
- poor scaling under many idle connections
- scheduler and context-switch overhead
- weak control over tail latency

### Async + coroutine model

Pros:

- scales to large numbers of concurrent streams
- efficient for mixed I/O-heavy services
- better cancellation and timeout semantics
- better composability for distributed systems

Cons:

- more complex runtime
- requires strict blocking discipline
- debugging requires better tracing

Production choice:

- async default
- bounded blocking pool for legacy adapters

## 5.10 Tail Latency Optimization

- isolate reactor threads from blocking and CPU-heavy tasks
- bound every queue
- prefer queue age over queue length for overload decisions
- precompute route and DI graphs
- reuse memory aggressively
- avoid global locks in hot path
- reserve capacity for priority traffic
- sample p99 traces continuously

---

## 6. Request Pipeline Architecture

## 6.1 Pipeline Stages

1. connection setup
2. protocol parse
3. request context creation
4. middleware chain
5. routing
6. filters/interceptors
7. DI resolution for endpoint scope
8. handler invocation
9. serialization
10. response filters
11. write flush
12. completion/finalization

## 6.2 Routing System

Routing tables should be immutable snapshots.

Support:

- static path routes
- parameterized segments
- wildcard segments
- method-based dispatch
- host-based dispatch
- versioned APIs
- gRPC service/method descriptors

Implementation preference:

- radix tree or compressed trie for HTTP paths
- direct descriptor tables for gRPC methods
- route binding metadata generated at build/startup time

## 6.3 Middleware Chaining

Middleware categories:

- transport-aware middleware
- authn/authz
- request logging
- tracing
- rate limiting
- caching
- compression
- exception mapping
- custom application middleware

Representation should avoid heap-building dynamic chains per request.

Compile pipeline once at startup into an execution plan.

## 6.4 Filters and Interceptors

Filters wrap endpoint execution and can operate at endpoint, controller, module, or global scope.

Use for:

- validation
- auth policies
- transaction boundaries
- response shaping
- retries for outbound dependencies

## 6.5 DI Resolution

Request-scoped DI is resolved from a precomputed graph:

- singleton services resolved from root container
- scoped services allocated in request scope arena or pooled object graph
- transient services minimized and optionally pooled/generated

Resolution should be mostly pointer chasing through compiled provider tables, not map lookups.

## 6.6 Request Context Propagation

Context carries:

- trace ids
- auth principal
- deadline/cancellation
- locale/tenant metadata
- request-scoped services
- log tags

Context should be explicitly passed or attached to coroutine/task scope, not hidden in process-wide globals.

## 6.7 Streaming Response Support

Framework must support:

- chunked HTTP responses
- SSE
- WebSocket streams
- gRPC server and bidi streams

Streaming writer requirements:

- backpressure-aware
- cancel-safe
- flush policy control
- partial serialization support

---

## 7. Memory Management Model

## 7.1 Request-Scoped Arenas

Per-request arenas store:

- decoded route values
- validation state
- serializer temp state
- scoped service objects
- response metadata

Benefits:

- O(1) reset
- low fragmentation
- predictable lifetimes

## 7.2 Object Pooling

Pool reusable objects with expensive construction or stable shapes:

- parser states
- header maps
- serializer buffers
- outbound client call objects
- DB command wrappers
- job descriptors

Avoid pooling tiny trivial objects unless profiling shows value.

## 7.3 Zero-Copy Buffers

Use slab-backed or page-backed buffers with ref-counted spans for:

- request bodies
- response bodies
- websocket frames
- gRPC payload chunks

## 7.4 Cache-Line Alignment Strategy

Hot structs should be aligned and padded to reduce false sharing:

- queue heads/tails
- scheduler worker state
- per-thread counters
- connection hot path metadata

Keep hot and cold fields separate.

## 7.5 Fragmentation Mitigation

Memory domains:

- connection lifetime pools
- request arenas
- singleton graph allocations
- pooled reusable buffers
- background scheduler pools

Do not mix long-lived container state with short-lived request allocations.

## 7.6 Long-Lived vs Short-Lived Objects

### Long-lived

- route tables
- DI provider graph
- config snapshots
- client pool structures
- logger sinks

Allocate once, often read-only.

### Short-lived

- request state
- serializer temporaries
- validation errors
- response builders

Allocate from request arena and reset after completion.

---

## 8. Framework Module Design

## 8.1 Routing Module

Responsibilities:

- build route graph
- bind descriptors to handlers
- method and content negotiation
- path param extraction

## 8.2 DI Container

Container design:

- root container for singletons
- scoped containers as compact activation frames
- precompiled provider tables
- lifecycle hooks: init, start, stop, dispose

Support:

- singleton
- scoped
- transient
- keyed/named service resolution where required
- optional/lazy services

## 8.3 Configuration System

Config sources:

- files
- environment
- command line
- secrets providers
- remote config plugin

Precedence is fixed and explicit.

Config binding should be typed and validated at startup.

Hot reload supported via immutable config snapshots and subscription callbacks.

## 8.4 Validation System

Validation should support:

- generated schema metadata
- fast-path primitive checks
- structured error reporting
- request binding integration
- optional compile-time validator generation

## 8.5 Serialization Framework

Requirements:

- JSON
- protobuf
- plain text / binary codecs
- streaming serialization
- zero-copy body access where possible

Prefer generated serializers for performance-critical paths.

## 8.6 Database Abstraction

Abstraction should be thin and non-leaky.

Provide:

- connection pools
- async query APIs
- transaction scopes
- retries/timeouts/circuit integration
- instrumentation hooks

Do not impose heavy ORM overhead in core path. ORM can exist as plugin layer.

## 8.7 Background Job System

Provide an in-process scheduler for:

- timers
- periodic jobs
- delayed tasks
- retry queues

For durable jobs, framework integrates with external queue systems instead of pretending in-process memory is durable.

## 8.8 Event System

Two event classes:

- in-process synchronous events for low-latency module coordination
- async event bus adapters for external brokers

Avoid using a generic event bus for request-critical control flow.

---

## 9. Logging and Observability System

## 9.1 Structured Logging

Logger features:

- JSON and human-readable modes
- async sink
- lock-free per-thread ingestion
- request-context enrichment
- log sampling and rate limiting

Fields:

- timestamp
- level
- service
- module
- request_id
- trace_id
- span_id
- route
- method
- status
- duration_us
- remote_addr
- tenant
- error_code

## 9.2 Request Tracing

Distributed tracing support via OpenTelemetry-compatible spans.

Span examples:

- request root
- middleware span
- handler span
- DB call span
- cache call span
- serializer span
- response flush span

## 9.3 Metrics System

Prometheus-style metrics:

- request rate
- latency histograms
- active connections
- active streams
- queue depth
- error counts
- GC/allocator stats if applicable
- scheduler stats
- downstream dependency stats

Per-thread counters aggregated periodically to reduce contention.

## 9.4 Profiling Hooks

Provide hooks for:

- CPU profiling
- allocation sampling
- event loop stall detection
- slow request logging
- scheduler queue latency measurement

## 9.5 Log Levels

- error
- warn
- info
- debug
- trace

Production defaults:

- `info` global
- targeted `debug` or `trace` by module/request/tenant

---

## 10. Fault Tolerance Strategy

## 10.1 Circuit Breaker

Apply to outbound dependencies:

- DB
- cache
- service RPC
- messaging brokers

States:

- closed
- open
- half-open

Trip on:

- timeout rate
- connection failure rate
- saturation signals

## 10.2 Retry Policies

Retries must be:

- bounded
- jittered
- deadline-aware
- idempotency-aware

No blanket retries from middleware for arbitrary handlers.

## 10.3 Timeout Handling

Every request has:

- connection idle timeout
- header/body read timeout
- application deadline
- downstream call deadlines
- graceful shutdown deadline

Use absolute deadlines instead of stacked relative timeouts where possible.

## 10.4 Graceful Shutdown

Sequence:

1. stop accepting new connections
2. fail readiness
3. drain keep-alive and active streams
4. cancel lingering low-priority work
5. flush telemetry
6. stop modules in dependency order

## 10.5 Overload Protection

Mechanisms:

- bounded queues
- per-tenant quotas
- adaptive connection acceptance
- backpressure-driven read suspension
- low-priority shedding
- degraded-mode features disabled under pressure

## 10.6 Health Checks

Expose:

- liveness
- readiness
- startup completion
- dependency health summary
- overload/shed state
- queue depth and saturation summary

---

## 11. Scalability Model

## 11.1 Horizontal Scaling

Framework is designed for stateless application instances.

Scale by adding instances behind:

- L4 load balancer
- L7 ingress
- service mesh sidecar or ambient mesh

## 11.2 Stateless Server Design

Session state should live in:

- databases
- distributed caches
- external session stores
- signed tokens where appropriate

In-memory affinity is optional optimization, not the correctness model.

## 11.3 Load Balancer Compatibility

Support:

- HTTP/1.1 keep-alive
- HTTP/2/gRPC multiplexing
- WebSocket upgrades
- health probes
- proxy headers
- mTLS where required

## 11.4 Service Mesh Compatibility

Be mesh-compatible, not mesh-dependent.

Rules:

- no hidden assumptions about raw sockets only
- preserve trace and auth headers cleanly
- avoid redundant retries if mesh already retries
- expose backpressure and timeout semantics clearly

## 11.5 Clustering Strategy

For clustered runtime features:

- external service registry for discovery
- leader election only for optional control-plane modules
- no mandatory in-process clustering model for regular stateless services

---

## 12. Performance Engineering

## 12.1 Critical Path Latency Analysis

Typical critical path:

1. accept/read
2. parse
3. route
4. auth/validation
5. handler logic
6. outbound dependency latency
7. serialization
8. write flush

Framework overhead must be minimized especially in stages 2-5 and 7.

## 12.2 Memory Bandwidth Considerations

High-throughput backends often become memory and cache limited before compute limited.

Mitigations:

- compact request structs
- immutable route/config snapshots
- per-thread pools and counters
- avoid pointer-heavy object graphs in hot path

## 12.3 CPU Cache Efficiency

- split hot/cold fields
- use contiguous arrays for route and provider metadata
- prefer table-driven dispatch
- minimize virtual calls in request path
- pad queue counters and worker metadata

## 12.4 Scheduler Tuning

Tune:

- reactor thread count
- worker thread count
- blocking pool limits
- queue capacities
- timer granularity
- batching policy for log/metric export

## 12.5 Benchmarking Methodology

Measure separately:

- raw transport throughput
- parser throughput
- middleware overhead
- DI activation overhead
- serialization throughput
- end-to-end request latency
- p50/p95/p99 under mixed traffic
- behavior under overload and client disconnect storms

Benchmark classes:

- hello world route
- JSON API
- gRPC unary
- streaming endpoint
- DB-backed CRUD path
- WebSocket fanout

Do not trust single-route microbenchmarks alone.

---

## 13. Implementation Roadmap

## Phase 1 - Host and Transport Core

Implement:

- host bootstrap
- config system
- reactor abstraction
- HTTP/1.1 transport
- request arena
- structured logger
- metrics exporter

## Phase 2 - Application Pipeline

Implement:

- routing engine
- middleware pipeline compiler
- DI root/scoped container
- request binding and validation
- JSON serializer

## Phase 3 - Advanced Protocols

Implement:

- HTTP/2
- gRPC
- WebSocket
- streaming response writer
- cancellation/deadline propagation

## Phase 4 - Service Modules

Implement:

- DB abstraction
- cache client layer
- background jobs
- event system
- circuit breaker and retry policies

## Phase 5 - High-Performance Enhancements

Implement:

- `io_uring` path
- generated serializers/validators/binders
- route and DI graph freezing optimizations
- NUMA-aware scheduler options
- advanced profiling hooks

## Phase 6 - Platform Hardening

Implement:

- graceful rolling restart support
- config hot reload
- service mesh tuning guidance
- production dashboards and SLO templates

---

## 14. Final Design Position

The correct architecture for a next-generation general-purpose backend framework is:

- async event-driven transport
- compiled middleware and routing pipeline
- strongly typed DI and config systems
- bounded lock-free queues and structured concurrency
- request arenas and pooled reusable objects
- observability built into the runtime, not bolted on later
- extensible modules behind narrow contracts

This yields a framework with the ergonomics of modern backend platforms while preserving predictable low-level performance characteristics suitable for high-scale production services.
