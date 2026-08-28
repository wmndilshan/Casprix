> **STATUS: design proposal — not yet implemented.**
>
> This document describes a planned system. It is kept for the design
> thinking it records, not as a description of code that exists today.
> Parts of the runtime (`runtime/net/`, `runtime/ai/ml/`) contain early
> building blocks, but the architecture below is largely unbuilt. Do not
> treat any section here as current documentation.

# Casprix CPU Inference Backend Architecture

## 1. System Objective

Design a production-grade CPU-first inference backend for classical ML, encoder/decoder models, and autoregressive LLM serving. The system is optimized for:

- high ingress concurrency
- low median and tail latency
- adaptive high-efficiency batching
- NUMA-aware CPU execution
- streaming token output
- production observability
- horizontal scale-out

The architecture is not a generic web server. It is a specialized inference dataplane with a control plane for model lifecycle, admission control, and fleet orchestration.

---

## 2. Top-Level Architecture

### 2.1 Planes

The system is split into four planes:

1. **Ingress dataplane**
   - socket accept, TLS, HTTP/1.1, HTTP/2, HTTP/3, gRPC
   - request decode, auth, rate limit, routing, cancellation, deadlines

2. **Inference dataplane**
   - request normalization
   - batching and scheduling
   - tensor prep
   - model execution
   - streaming / final response encode

3. **State plane**
   - model registry and version state
   - KV-cache directory and placement
   - warm pools
   - hot reload metadata

4. **Observability and control plane**
   - structured logs
   - metrics and traces
   - admin API, health endpoints, drain mode, circuit state, rollout state

### 2.2 Node Roles

A production deployment uses three roles:

- **Edge Gateway**
  - terminates public TLS and QUIC
  - enforces auth, quotas, coarse routing
  - can be separate if public internet exposure is required

- **Stateless Frontend Router**
  - protocol termination for internal HTTP/2/gRPC
  - request normalization
  - model/version lookup
  - consistent routing to inference workers or KV-cache shards

- **Inference Worker**
  - owns CPU cores, NUMA-local memory pools, batching queues, model executors
  - may also host local KV-cache for short sessions

Optional:

- **KV-Cache Node** for long-running conversational sessions and disaggregated decode
- **Model Loader / Control Agent** per host for warmup, mmap validation, and hot-swap orchestration

### 2.3 Process Model

Per physical host:

- 1 control process
- N inference worker processes, usually 1 process per NUMA node
- each process runs one event loop group and one compute scheduler

This avoids cross-NUMA thrash and reduces global allocator contention.

---

## 3. Networking Stack Design

## 3.1 Event-Driven Runtime

The networking runtime follows a Reactor + Executor model:

- **Reactor**: readiness/completion driven I/O multiplexer
- **Executor**: coroutine scheduler for protocol and application tasks
- **Compute scheduler**: separate CPU-bound executor for tensor work and batching

### 3.1.1 OS Abstraction Layer

Provide a transport abstraction:

- Linux default: `io_uring` if kernel/runtime maturity checks pass
- Linux fallback: `epoll`
- BSD/macOS: `kqueue`
- Windows: IOCP

Interface shape:

- submit accept/read/write/sendfile/timeout ops
- completion callback or coroutine wakeup
- registered buffers support
- registered file descriptors where supported

`io_uring` is preferred for:

- reduced syscall overhead
- batched submissions/completions
- fixed buffer registration
- lower per-request scheduling cost under high concurrency

`epoll` remains necessary as a mature fallback for:

- heterogeneous kernel versions
- operational simplicity
- certain TLS stacks and proxy chains

## 3.2 Protocol Support

### HTTP/1.1

Used for:

- compatibility clients
- health/admin endpoints
- simple synchronous inference

Requirements:

- keep-alive by default
- pipelining disabled or tightly bounded
- header/body bounds enforced pre-parse

### HTTP/2

Primary transport for low-latency RPC over TCP:

- multiplexed streams per connection
- HPACK header compression
- stream-level deadlines and cancellation
- preferred for gRPC

### HTTP/3

Preferred internet-facing transport when QUIC is beneficial:

- connection migration support
- reduced HOL blocking vs TCP
- better behavior under packet loss
- useful for token streaming across unstable networks

### gRPC

Primary service interface internally and for model-serving clients:

- unary inference RPCs
- server-streaming token emission
- bidirectional streaming for agentic or incremental prompting
- explicit metadata for deadlines, priority, tenant, trace, session routing

## 3.3 Zero-Copy Parsing and Buffering

Zero-copy is applied where safe and practical:

- NIC/kernel buffer -> transport read buffer
- parser stores slices into buffer, not copied strings
- protobuf/JSON decode uses arena-backed field references
- body payloads remain in immutable ref-counted slabs until tensorization

Buffer strategy:

- per-thread slab allocator for I/O buffers
- fixed-size classes: 1 KB, 4 KB, 16 KB, 64 KB, 256 KB
- large payload path uses page-aligned chained buffers
- scatter/gather writes for headers + body + stream chunks

Request parser requirements:

- incremental parsing
- bounds-checked slices
- no heap allocation in hot path for common headers
- canonicalized metadata written into request arena only once

## 3.4 TLS Termination Strategy

Two supported modes:

### Edge TLS Termination

Best for public entry:

- terminate TLS/QUIC at edge gateway
- re-encrypt internally with mTLS for east-west traffic
- offload certificate rotation and ciphersuite policy
- preserve trace headers and client identity metadata

### In-Worker TLS Termination

Best for private clusters or minimal hop count:

- terminate TLS directly in inference workers
- useful for lower internal latency and simpler topology
- requires tight CPU budgeting because TLS competes with inference cores

Policy:

- QUIC/TLS 1.3 at edge
- HTTP/2 over TLS 1.3 internally for RPC
- mTLS between router and worker

## 3.5 Connection Management

Per-worker connection architecture:

- acceptor shard per listener socket with `SO_REUSEPORT` on Linux
- connection assigned to local event loop and stays pinned there
- HTTP/2 stream scheduling isolated from tensor scheduler
- bounded write buffers per connection and per stream

Pooling:

- client-side gRPC channel pools with warm HTTP/2 connections
- backend connection pools keyed by worker shard and model family
- per-destination inflight limits to prevent convoying

## 3.6 Backpressure and Load Shedding

Backpressure is explicit at every queue boundary:

- socket read disable when request decode queue exceeds threshold
- HTTP/2 stream window reduction for overloaded workers
- request admission gate before tensorization
- batch queue depth limits per model/version/priority class
- response streaming throttled by downstream write credits

Load shedding hierarchy:

1. reject low-priority work before decode
2. cap queue wait time using request deadline
3. downgrade expensive features (logprobs, explanations, debug traces)
4. shrink batch target size under saturation
5. stop accepting new long-context requests
6. fail fast with structured overload response

Admission decision uses:

- queue length
- oldest queued age
- current service time estimate
- deadline slack
- model-specific memory pressure
- CPU run queue and LLC miss rate signal

## 3.7 Kernel Bypass Considerations

DPDK or similar kernel-bypass path is optional, not default.

Use only when:

- requests are extremely small and uniform
- deployment is single-tenant or tightly controlled
- dedicated NIC queues and CPU isolation are available
- operational complexity is acceptable

Default production choice remains kernel sockets with `io_uring`/`epoll` because:

- easier TLS and protocol integration
- lower operational complexity
- better compatibility with service meshes and containerized environments

Kernel bypass becomes attractive for private low-latency inference fabrics, not for general multi-tenant serving.

---

## 4. Request Lifecycle: Socket -> Inference -> Response

1. **Accept**
   - listener shard accepts connection on local NUMA node
   - connection context allocated from per-loop connection pool

2. **Handshake / Protocol detect**
   - TLS/QUIC negotiation
   - ALPN selects HTTP/1.1, HTTP/2, or HTTP/3

3. **Parse**
   - headers parsed zero-copy into slices
   - deadline, priority, tenant, model id, session id extracted
   - malformed/oversized requests rejected before application allocation

4. **Admission control**
   - overload gate checks queue depth, latency budget, memory pressure
   - request either admitted, downgraded, or rejected

5. **Normalization**
   - request canonicalized into internal inference envelope
   - tokenization or feature decode scheduled
   - request-scoped arena instantiated

6. **Routing**
   - model/version selected
   - if session-bound decode, route to KV-cache owner shard
   - otherwise route to local or remote batch coordinator

7. **Preprocessing**
   - prompt/token decode
   - tensor views built into request arena
   - large immutable inputs remain referenced, not copied

8. **Batching**
   - request inserted into per-model adaptive batch queue
   - batcher decides immediate dispatch vs delay-for-fill based on SLO slack

9. **Execution**
   - execution planner maps batch to NUMA-local workers
   - prefill path and decode path scheduled separately
   - KV-cache pages reserved or reused

10. **Streaming**
   - partial tokens flushed via gRPC stream or HTTP chunk/HTTP/3 stream frames
   - flush cadence obeys write credits and latency target

11. **Completion**
   - final metadata, usage counters, timing, and trace emitted
   - request arena reset in O(1)
   - connection reused

12. **Post-completion**
   - success/error metrics updated
   - adaptive controllers update batching and admission thresholds

---

## 5. Request Concurrency Model

## 5.1 Hybrid Concurrency Architecture

Do not use a single scheduler for both network and inference.

Use three cooperating schedulers:

1. **I/O coroutine scheduler**
   - event-loop driven
   - handles protocol state machines, stream control, deadlines, cancellation, writes

2. **Control/task scheduler**
   - lightweight async tasks for tokenization, routing, metadata fetches, tracing

3. **Compute scheduler**
   - work-stealing, CPU-pinned, NUMA-local
   - runs tensor prep, batching decisions, model kernels, post-processing

This avoids head-of-line blocking where expensive compute starves network progress.

## 5.2 Coroutine Execution

Coroutines are used for:

- connection state machines
- per-stream request handling
- timeout/deadline suspension
- awaitable queue admission
- streaming response emission

Coroutines must be stackless or segmented-stack to avoid large per-request memory footprints.

Rules:

- no blocking syscalls in coroutine paths
- any operation exceeding a few microseconds or touching large memory regions moves to compute scheduler
- cancellation tokens propagate through every await point

## 5.3 Thread-per-Core vs Async Tradeoff

### Thread-per-Core

Pros:

- strong cache locality
- simpler reasoning for CPU-owned resources
- minimal context-switch overhead
- ideal for compute-heavy execution

Cons:

- poor fit for tens of thousands of mostly idle connections
- harder stream multiplexing
- difficult timeout/cancellation handling for protocol state

### Pure Async

Pros:

- excellent at massive connection fan-in
- efficient timers, cancellations, stream multiplexing
- low idle resource cost

Cons:

- poor isolation for CPU-heavy tasks
- compute tasks can monopolize reactor threads
- harder NUMA ownership and deterministic locality

### Production Choice

Use **hybrid async + thread-per-core compute**:

- async for networking and request orchestration
- thread-pinned compute workers for inference

## 5.4 Work-Stealing Scheduler

Compute workers:

- one worker thread per physical core assigned to the NUMA node process
- each worker has a local Chase-Lev deque
- owner pops LIFO for locality
- thieves steal FIFO for balance

Task classes:

- request preprocessing
n- prefill execution
- decode step
- post-processing
- stream formatting
- KV-cache maintenance

Cross-node stealing is disabled by default. Stealing is local to a NUMA node.

## 5.5 Lock-Free Queues

Use lock-free MPSC or MPMC queues at specific boundaries:

- ingress -> per-model admission queue
- router -> batcher mailbox
- batcher -> executor dispatch queue
- completion -> streaming writer queue

Guidelines:

- bounded queues only
- cache-line padded head/tail counters
- sequence-based ring buffers for predictable memory layout
- avoid fully general unbounded lock-free structures in hot paths

## 5.6 NUMA-Aware Request Routing

Routing rule:

- request is assigned to a NUMA node based on model residency, session affinity, and queue pressure
- once assigned, all mutable per-request state stays local to that node

Preferred affinity order:

1. existing KV-cache owner
2. model weights resident on node
3. lowest predicted wait among local nodes
4. remote node only if local SLO violation would be worse

Cross-NUMA handoff only passes compact metadata and buffer references, not materialized tensors.

## 5.7 Adaptive Batching

Maintain separate queues per:

- model
- version
- phase: prefill vs decode
- priority class
- context-length bucket

Batching policy:

- prefill batches favor throughput
- decode batches favor cadence and tail latency
- maximum batching delay derived from deadline slack and token cadence target

Heuristic inputs:

- current queue length
- observed service time distribution
- context length
- token generation rate
- per-model memory bandwidth pressure
- KV-cache availability

## 5.8 Priority and Deadline Scheduling

Every request carries:

- hard deadline timestamp
- service class
- tenant priority
- cancellation token

Scheduler chooses next work by composite score:

- earliest deadline first within class
- weighted fairness across tenants
- aging boost for long-waiting requests
- anti-starvation floor for low-priority traffic

## 5.9 Cancellation Support

Cancellation points:

- pre-admission
- in queue
- between prefill and decode steps
- between emitted tokens
- before expensive post-processing

Cancellation does not interrupt an inner micro-kernel. It is checked at safe boundaries:

- batch assembly
- layer boundary
- token boundary
- stream flush boundary

## 5.10 Tail Latency Mitigation

Use a combination of:

- bounded queues and deadline admission
- separate prefill and decode lanes
- small-batch fast lane for urgent requests
- anti-convoy batching buckets by context length
- CPU isolation for networking threads
- NUMA-local memory ownership
- straggler-aware speculative second dispatch for rare high-value requests
- background cache maintenance only when spare capacity exists

---

## 6. Inference Execution Pipeline

## 6.1 Pipeline Stages

1. request decode
2. tokenizer / feature extraction
3. input tensorization
4. scheduling and batch assignment
5. prefill execution
6. decode loop / iterative generation
7. post-processing
8. response serialization / streaming

Each stage emits spans and queue metrics.

## 6.2 Request Decoding

Input forms:

- JSON REST
- protobuf/gRPC
- binary tensor RPC for trusted internal callers

Decoding writes into request arena:

- request envelope
- string slices
- token arrays
- inference options
- trace metadata

Avoid copying prompt bytes unless transformation is required.

## 6.3 Tensor Preparation

Build tensor views backed by:

- request arena for small temporary tensors
- shared immutable vocab/token buffers
- per-thread scratch pools for padded or packed layouts

Tensor preparation includes:

- token ids
- masks and position ids
- packed sequence descriptors
- rope/scaling parameters
- batch layout descriptors

## 6.4 Dynamic Batching Engine

The batcher is the central throughput/latency controller.

Responsibilities:

- bucket compatible requests
- maximize useful FLOPs per cache-resident working set
- avoid mixing pathological long and short contexts in same batch
- decide dispatch time using slack-aware heuristics

Batch compatibility keys:

- model id/version
- execution phase
- precision/quantization mode
- sampling configuration family
- max sequence length bucket
- streaming/non-streaming mode

Batch formation rules:

- decode micro-batches are cadence-driven
- prefill batches are fill-driven within bounded delay
- high-priority traffic may bypass batching thresholds

## 6.5 Model Execution Scheduling

Execution planner decomposes work into:

- prefill jobs
- decode jobs
- post-processing jobs
- stream emission jobs

Each model instance exposes:

- preferred batch sizes
- preferred token cadence
- KV-cache footprint per token
- memory bandwidth estimate
- saturation curves per core count

Scheduler uses this to decide:

- which instance to run
- how many compute workers to allocate
- whether to co-locate with other models on same NUMA node

## 6.6 Token Streaming

Streaming path must not wait for final completion.

Design:

- per-request stream state object
- token chunks written into ring buffer
- writer coroutine flushes on one of:
  - token available
  - flush timer
  - punctuation/end-of-sentence heuristic
  - backpressure window threshold

Supports:

- gRPC server streaming
- SSE for browser clients
- chunked HTTP/1.1 fallback
- HTTP/3 unidirectional or bidirectional stream mapping

## 6.7 Partial Result Flushing

Flush policy is dynamic:

- default flush every token for low-latency interactive traffic
- coalesce 2-8 tokens for throughput traffic
- coalesce more aggressively when client RTT is high or socket buffers are constrained

## 6.8 Speculative Decoding Pipeline

Production speculative path:

- lightweight draft model generates K speculative tokens
- verifier model validates prefix in larger batch
- accepted prefix emitted immediately
- rejected suffix rolled back via KV-cache markers

Requirements:

- branchable KV-cache markers
- per-request speculative windows
- rollback-safe token accounting
- acceptance-rate feedback loop

Scheduling note:

- draft model runs on otherwise underutilized cores or lower-priority pool
- verifier stays on main latency-critical pool

---

## 7. Memory Management Architecture

## 7.1 Allocation Domains

Use specialized allocators, not a global heap:

1. **connection pool memory**
2. **request-scoped arenas**
3. **tensor scratch pools**
4. **batch descriptor pools**
5. **KV-cache pools**
6. **model-weight mapped regions**
7. **telemetry buffers**

## 7.2 Request-Scoped Arenas

Each request gets an arena reset on completion.

Contents:

- decoded metadata
- token arrays
- temporary tensor descriptors
- post-processing strings
- tracing annotations

Properties:

- O(1) reset
- no free-list churn
- no fragmentation in hot path
- alignment guarantees for tensor views and SIMD loads

## 7.3 Tensor Memory Pools

Per-thread / per-NUMA pools for:

- packed GEMM panels
- temporary activations
- dequantization scratch
- attention score tiles
- stream formatting buffers

Pool design:

- size-classed slabs
- 64-byte alignment minimum
- hugepage option for large recurrent buffers
- cache coloring or page spread when LLC conflicts are visible

## 7.4 Zero-Copy Buffer Passing

Pass references across stages via descriptors:

- base pointer
- length
- ownership domain
- lifetime token
- NUMA home

Avoid copying for:

- raw request bodies
- token buffers after decode
- immutable prompt prefix blocks
- response stream fragments

Copy only when:

- layout transformation is necessary for compute efficiency
- security sanitization requires isolation
- cross-process transport demands serialization

## 7.5 Fragmentation Avoidance

Rules:

- arenas for short-lived state
- pools for reusable fixed-size blocks
- page-based allocator for KV-cache
- never interleave long-lived weights with short-lived scratch allocations
- no general-purpose allocator on hot path except during cold start

## 7.6 KV-Cache Management

Use paged KV-cache rather than monolithic contiguous growth.

Features:

- per-model page pools
- per-sequence page tables
- append-only token writes
- prefix sharing and reuse where safe
- speculative decode mark/rollback
- eviction by TTL, LRU-with-cost, or session policy

Topology options:

- local KV-cache on worker for lowest latency
- remote KV-cache shard for sticky long sessions
- hybrid with local hot prefix and remote cold suffix

## 7.7 NUMA-Aware Allocation

Each process is pinned to one NUMA node.

Allocator policy:

- first-touch or explicit NUMA binding for pools
- request arena allocated from node-local pages
- model weights replicated per node when memory budget allows
- otherwise interleaved read-mostly mapping for shared weights
- remote memory access counters fed into admission controller

---

## 8. Model Management

## 8.1 Versioned Model Serving

Each model is addressed by:

- logical name
- semantic version
- deployment generation
- quantization/runtime profile

Frontend routes with explicit version when provided, otherwise via traffic policy.

## 8.2 Hot Reload

Hot reload is two-phase:

1. **Prepare**
   - mmap weights
   - validate metadata and checksum
   - instantiate tokenizer/runtime artifacts
   - run warmup and baseline checks

2. **Commit**
   - atomically switch routing table to new generation
   - drain old generation
   - retain old mmap until all inflight requests complete

No in-place mutation of live model objects.

## 8.3 Lazy Loading

Lazy load on first request only for low-traffic models.

Guardrails:

- separate cold path capacity budget
- max concurrent cold loads per host
- queue requests in pending state with stricter timeout
- optionally redirect to warm replica

## 8.4 Memory-Mapped Weights

Default strategy for large models:

- mmap read-only weight files
- page-aligned sections by layer group
- optional hugepage remap for hot layers
- startup prefetch of hot pages

Benefits:

- reduced startup copy cost
- easier hot reload
- shared page cache where appropriate

## 8.5 Warmup Strategy

Warmup must exercise real critical paths:

- tokenizer warmup
- protocol serialization warmup
- representative prefill batch
- representative decode stream
- KV-cache allocation/rollback path
- branch predictor and instruction cache warmup for hot kernels

Warmup results feed readiness state.

## 8.6 Multi-Model Scheduling

Do not co-schedule blindly.

Place models based on:

- LLC footprint
- memory bandwidth demand
- typical batch sizes
- decode cadence
- tenant isolation policy

Models with conflicting bandwidth behavior should not share a NUMA node under load.

---

## 9. Logging and Observability

## 9.1 Structured Logging

Logger must support JSON and human formats.

Core fields:

- timestamp
- severity
- stage
- component
- request_id
- trace_id
- span_id
- model
- version
- tenant
- connection_id
- stream_id
- deadline_ms_remaining
- queue_wait_us
- exec_us
- bytes_in
- bytes_out
- outcome

Stages:

- ingress
- parse
- admit
- route
- batch
- execute
- decode
- stream
- scheduler
- network_write
- complete
- fault

## 9.2 Tracing

Use OpenTelemetry-compatible spans.

Span hierarchy:

- request root span
- parse span
- admission span
- routing span
- batch wait span
- model execute span
- per-token decode spans sampled selectively
- response flush spans

Sampling:

- low base rate globally
- elevated for errors, tail events, canaries, and debug tenants

## 9.3 Metrics

Prometheus/OpenTelemetry metrics include:

### Traffic

- requests/sec by model/version/tenant/status
- active connections
- active streams
- bytes in/out

### Latency

- end-to-end latency histogram
- queue wait histogram
- batching delay histogram
- prefill latency histogram
- decode token latency histogram
- flush latency histogram

### Capacity

- queue depth by stage
- inflight requests by class
- active batches
- CPU utilization by core and NUMA node
- memory bandwidth estimate
- LLC miss rate
- remote NUMA access rate

### Reliability

- shed request count
- deadline miss count
- cancellations
- circuit breaker open count
- health degradation state

## 9.4 Profiling

Span-based profiling and sampled continuous profiling should capture:

- per-stage wall time
- per-stage CPU time
- allocator hot spots
- queue residence time
- token cadence variance
- tail event attribution

## 9.5 Logging Modes

### Production Mode

- JSON structured logs
- bounded field cardinality
- sampled debug spans
- no per-token logging by default
- asynchronous batched log writer

### Debug Mode

- human-readable logs
- request-stage transitions
- scheduler decisions
- batch composition diagnostics
- optional token-level trace for a single request id

---

## 10. Fault Tolerance and Overload Control

## 10.1 Circuit Breakers

Circuit breakers exist at:

- upstream routing destination
- model instance
- remote KV-cache shard
- external tokenizer/feature service if any

States:

- closed
- open
- half-open with bounded probes

Open conditions use:

- error rate
- timeout rate
- queue overflow rate
- health probe failure

## 10.2 Retry Strategy

Retries are conservative.

Allowed only for:

- idempotent pre-execution failures
- connection establishment failures
- routing failures before model execution begins

Never blindly retry after prefill or token emission started.

Use:

- jittered bounded retry
- alternate shard retry if affinity permits
- deadline-aware retry budget

## 10.3 Graceful Degradation

Degradation ladder:

- disable verbose traces
- cap max output tokens
- disable logprobs/top-k extras
- route only high-priority traffic
- disable speculative decoding if verifier saturated
- reject long-context requests
- serve cached or approximate responses where allowed

## 10.4 Health Checking

Health endpoints expose:

- liveness
- readiness
- model readiness per version
- queue saturation state
- memory pressure
- KV-cache pressure
- circuit state summary

Readiness must fail on sustained SLO violation, not just process aliveness.

## 10.5 Backpressure Propagation

Backpressure propagates upward:

- batcher to router
- router to transport stream windows
- transport to socket reads
- frontend to edge via service status and retry hints

This prevents hidden queue buildup.

## 10.6 Overload Protection

Core mechanisms:

- bounded queues everywhere
- queue timeout over queue length preference
- strict deadline admission
- per-tenant quotas
- request cost estimation based on prompt len + max output len
- reserve capacity for high-priority traffic

---

## 11. Scalability Model

## 11.1 Horizontal Scaling

Use scale-out as the primary growth mode.

Within a region:

- stateless frontend routers scale independently
- inference worker pools scale by model family and size class
- KV-cache shards scale separately for conversational workloads

## 11.2 Sharded Inference Clusters

Shard by:

- model family
- version
- tenant isolation class
- memory footprint / context class

Avoid a universal worker pool if models have very different cache and bandwidth behavior.

## 11.3 Stateless Frontend + Stateful KV Nodes

For LLM chat workloads:

- frontend remains stateless
- request carries session id and conversation routing token
- KV directory maps session -> KV shard
- decode requests stick to KV owner or its failover partner

This keeps frontend elastic while preserving low-latency session state.

## 11.4 Service Mesh Compatibility

System should be mesh-compatible but not mesh-dependent.

Recommendations:

- disable expensive L7 mesh features on hot inference path if they add tail latency
- prefer mTLS and simple routing at mesh layer
- keep retries and deadlines owned by application layer to avoid duplicate retry storms

## 11.5 Load Balancer Strategy

Two-level balancing:

1. **Edge/global load balancer**
   - geo and zone aware
   - weighted by regional health and capacity

2. **Internal router**
   - model-aware
   - session-aware for KV affinity
   - queue-aware with power-of-two choices or EWMA latency routing

Request cost estimation should influence balancing, not just request count.

---

## 12. Performance Engineering

## 12.1 Latency Critical Path

Critical path for interactive decode:

1. transport receive
2. parse and admission
3. queue wait
4. decode micro-batch assembly
5. one decode step
6. sampling/post-process
7. flush

Primary tail drivers:

- queueing delay
- memory bandwidth contention
- cross-NUMA traffic
- oversized mixed batches
- TLS or QUIC CPU spikes on same cores as compute
- downstream write stalls during streaming

## 12.2 Adaptive Batching Heuristics

Use controller-based heuristics rather than static thresholds.

Inputs:

- recent batch service time
- target p50/p95 latency
- token cadence target
- current queue age
- acceptance ratio for speculative decode
- LLC miss and bandwidth signals

Outputs:

- max batching delay
- target batch size
- bucket split/merge policy
- prefill vs decode priority bias

## 12.3 Queueing Theory Load Control

Model each queue as bounded M/G/k approximation with observed service distribution.

Operational rules:

- maintain utilization below the knee of latency curve
- use queue age, not just queue length, as overload signal
- reserve headroom for burst absorption and retries
- reject requests when predicted sojourn time exceeds deadline slack

## 12.4 Memory Bandwidth Mitigation

CPU inference becomes bandwidth-bound before core-bound for many decode workloads.

Mitigations:

- per-NUMA model pinning
- replicate hot weights when cheaper than remote reads
- compact KV-cache pages
- cache-friendly batch bucketing by sequence length
- use quantized weights where accuracy permits
- avoid mixing bandwidth-heavy models on same node

## 12.5 Cache Usage Optimization

- 64-byte align all hot structures
- separate hot/cold fields in request structs
- use SoA layouts for scheduler metadata when scanned frequently
- keep queue nodes compact and padded
- pre-pack tensor panels for GEMM
- keep per-connection hot state in one or two cache lines

## 12.6 Tail Reduction Strategies

- isolate reactor threads from compute threads
- reserve dedicated small-batch urgent lane
- avoid giant prefill monopolizing decode cadence
- use queue age-based eviction for expired work
- sample and analyze p99 trace exemplars continuously
- use canary throttling on new model versions

---

## 13. Recommended Internal Modules

Suggested framework modules:

- `net/reactor/` - io_uring/epoll/kqueue abstraction
- `net/proto/` - HTTP/1.1, HTTP/2, HTTP/3, gRPC codecs
- `net/tls/` - TLS and QUIC integration
- `server/admission/` - quotas, deadlines, shedding
- `server/router/` - model/session/NUMA routing
- `server/batcher/` - adaptive batch queues and controllers
- `runtime/scheduler/` - coroutine + compute work-stealing schedulers
- `runtime/memory/` - arenas, pools, KV-cache allocators
- `runtime/model/` - mmap, hot reload, warmup, registry
- `runtime/infer/` - execution planner and backend dispatch
- `obs/log/` - structured logger
- `obs/trace/` - tracing and profiling
- `obs/metrics/` - Prometheus/OpenTelemetry exporters
- `control/admin/` - health, drain, config, rollout control

---

## 14. Production Tradeoff Analysis

### Prefer throughput

When:

- offline batch inference
- embedding generation
- high fan-in asynchronous workloads

Actions:

- larger prefill batches
- more coalesced streaming flushes
- lower tracing sample rate
- tighter connection reuse

### Prefer latency

When:

- interactive chat
- low-token decode cadence SLA
- premium priority tier

Actions:

- smaller decode batches
- urgent lane bypass
- stricter deadline admission
- more aggressive affinity to warm KV shards

### Prefer simplicity

When:

- early production rollout
- mixed OS/kernel fleet

Actions:

- epoll before io_uring if operational maturity is not proven
- HTTP/2 before HTTP/3 internally
- local KV only before disaggregated KV service

### Prefer maximum performance

When:

- homogeneous Linux fleet
- mature ops and profiling stack

Actions:

- io_uring with registered buffers
- QUIC at edge only where net conditions justify it
- per-NUMA worker processes
- speculative decoding with verifier isolation

---

## 15. Implementation Roadmap

## Phase 1 - Core Dataplane

Deliver:

- epoll/io_uring reactor abstraction
- HTTP/1.1 + HTTP/2 + gRPC server
- request arenas and slab buffers
- stateless routing
- per-model bounded queues
- fixed heuristic batcher
- structured logs and Prometheus metrics
- basic health and drain endpoints

Success criteria:

- stable under sustained concurrency
- bounded queueing
- p95 latency visible end-to-end

## Phase 2 - Low-Latency Serving

Deliver:

- split prefill/decode lanes
- coroutine cancellation and deadlines
- work-stealing compute scheduler
- NUMA-local request routing
- token streaming over gRPC and SSE
- KV-cache page allocator
- trace spans across request lifecycle

Success criteria:

- stable token cadence
- reduced p99 under mixed load
- no hot-path mallocs during steady state

## Phase 3 - High Efficiency

Deliver:

- adaptive batching controller
- mmap model loader and hot reload
- multi-version serving
- decode micro-batching
- queue age based shedding
- verifier/draft speculative decoding

Success criteria:

- higher throughput per socket/core
- controlled tail under bursty load

## Phase 4 - Distributed Scale-Out

Deliver:

- stateful KV-cache shard service
- session-aware routing
- fleet-aware load balancing
- canary rollout support
- OpenTelemetry export
- circuit breakers and retry budgets

Success criteria:

- scale horizontally without large p99 regression
- safe model rollout and shard failover

## Phase 5 - Advanced Optimization

Deliver:

- io_uring registered buffer fast path
- HTTP/3 edge path
- hugepage tuning
- optional kernel-bypass experimental path
- automated controller tuning from live telemetry

Success criteria:

- measurable gain over baseline without operational instability

---

## 16. Final Design Position

The correct production design for CPU-based ML/LLM inference is a **hybrid event-driven + NUMA-local compute architecture**:

- async networking for connection scale and protocol efficiency
- dedicated compute workers for deterministic CPU execution
- adaptive batching that is deadline-aware
- request arenas and paged KV-cache for memory stability
- structured logs, traces, and metrics embedded into every stage
- horizontal scale via stateless routing and stateful KV sharding where needed

This architecture minimizes median latency, controls tail behavior, and scales operationally without sacrificing implementation realism.
