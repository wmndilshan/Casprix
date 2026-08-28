> **STATUS: design proposal — not yet implemented.**
>
> This document describes a planned system. It is kept for the design
> thinking it records, not as a description of code that exists today.
> Parts of the runtime (`runtime/net/`, `runtime/ai/ml/`) contain early
> building blocks, but the architecture below is largely unbuilt. Do not
> treat any section here as current documentation.

# Language Binding Architecture for Casprix Async C Runtime

## 1. Runtime <-> Language Architecture (Textual Diagram)

```text
+----------------------------------------------------------------------------------+
|                               Language Toolchain                                |
|  Parser -> Typechecker -> MIR/IR -> Async Lowering -> ABI Shims -> Codegen      |
+-------------------------------------------+--------------------------------------+
                                            |
                                            v
+----------------------------------------------------------------------------------+
|                          Language Runtime Layer (Thin)                           |
|  - Task/Future facade                                                            |
|  - Async context + cancellation tokens                                           |
|  - Typed buffer/view wrappers                                                    |
|  - Panic boundary guards                                                         |
|  - Capability table (resolved at startup)                                        |
+-------------------------------------------+--------------------------------------+
                                            |
                     Stable C ABI (versioned function table + opaque handles)
                                            |
                                            v
+----------------------------------------------------------------------------------+
|                             Casprix C Async Runtime                              |
|  event loop | coroutine scheduler | async tasks | net I/O | timers | pools       |
|  (language-agnostic, opaque internals, no language metadata assumptions)         |
+-------------------------------------------+--------------------------------------+
                                            |
                                            v
+----------------------------------------------------------------------------------+
|                               OS / Kernel / NIC                                  |
|  epoll / kqueue / io_uring / IOCP, sockets, timers, memory mapping              |
+----------------------------------------------------------------------------------+
```

Design invariant:

- compiler and language runtime depend on stable C ABI contracts only
- C runtime does not depend on language internals
- integration glue remains replaceable and version-gated

---

## 2. ABI Design Specification

## 2.1 ABI Surface Style

Expose a single exported symbol returning a function table:

```c
typedef struct {
    uint32_t abi_major;
    uint32_t abi_minor;
    uint64_t feature_bits;

    // lifecycle
    int  (*rt_init)(const CpxRtConfig* cfg, CpxRtHandle** out_rt);
    void (*rt_shutdown)(CpxRtHandle* rt);

    // task/scheduler
    int  (*task_spawn)(CpxRtHandle* rt, const CpxTaskDesc* desc, CpxTaskHandle** out_task);
    int  (*task_cancel)(CpxTaskHandle* task);
    int  (*task_join_poll)(CpxTaskHandle* task, CpxTaskStatus* out);

    // io/timer
    int  (*io_submit)(CpxRtHandle* rt, const CpxIoReq* req, CpxOpHandle** out_op);
    int  (*timer_arm)(CpxRtHandle* rt, const CpxTimerReq* req, CpxTimerHandle** out_timer);

    // memory pools
    int  (*pool_alloc)(CpxPoolHandle* pool, size_t size, size_t align, void** out_ptr);
    int  (*pool_free)(CpxPoolHandle* pool, void* ptr);

    // diagnostics
    const char* (*status_str)(int code);
} CpxRuntimeApi;

const CpxRuntimeApi* cpx_runtime_get_api(void);
```

Why table-based ABI:

- easy capability extension without symbol explosion
- supports lazy feature probing
- avoids hard failure when older runtime is loaded

## 2.2 Handle Semantics

All cross-boundary objects are opaque handles:

- `CpxRtHandle*`
- `CpxTaskHandle*`
- `CpxOpHandle*`
- `CpxPoolHandle*`

No language-visible runtime internals or structs with internal fields.

## 2.3 Calling Convention and Layout Rules

- C calling convention only
- POD structs at ABI edge must be fixed-layout, explicitly sized fields
- no variable-sized inline tails in ABI structs
- all pointers are nullable unless specified non-null
- alignment requirements are explicit in APIs

## 2.4 Error Model at ABI Boundary

C side returns integer status codes.

Categories:

- success: `CPX_OK`
- caller faults: invalid args, bad state
- transient runtime faults: queue full, timeout, would block
- terminal faults: shutdown, internal error, unsupported feature

Language binding maps codes to typed error enums/results (not raw ints in user APIs).

## 2.5 Ownership Transfer Contracts

Ownership is always explicit in function docs and names:

- `*_borrow` -> pointer valid for call duration only
- `*_clone` -> reference count increment or deep copy
- `*_take` -> ownership transferred to callee
- `*_give_back` -> ownership returned to caller

Never rely on convention by omission.

## 2.6 Data Marshaling Strategy

Marshaling tiers:

1. **zero-copy views** for byte buffers and immutable spans
2. **shallow marshaling** for POD descriptors
3. **deep marshaling** only when crossing ownership domains with incompatible lifetime guarantees

Use explicit boundary structs:

```c
typedef struct {
    const uint8_t* ptr;
    uint32_t len;
} CpxByteView;

typedef struct {
    uint8_t* ptr;
    uint32_t len;
    uint32_t cap;
    uint32_t owner_tag;
} CpxMutableBuffer;
```

## 2.7 Stack vs Heap Representation at Boundary

- small POD request descriptors pass by value or const pointer from caller stack
- async continuations and operations always heap-anchored (or pool-anchored)
- boundary must never keep references to caller stack memory beyond call return

Rule: if lifetime crosses suspension, representation must be heap/pool resident.

## 2.8 Async Callback Bridging

Use callback trampolines with explicit context pointers and cancellation tokens.

```c
typedef void (*CpxCallbackFn)(void* ctx, int status, const CpxOpResult* result);

typedef struct {
    CpxCallbackFn cb;
    void* ctx;
    CpxCancelToken* cancel;
} CpxCompletion;
```

Language compiler/runtime emits trampolines that re-enter language scheduler safely.

---

## 3. Async Model Integration Design

## 3.1 Async/Await Lowering Strategy

Language compiler lowers `async fn` to stackless state machines:

- state enum
- spilled locals frame
- resume entrypoint
- await points as switch labels

Generated frame layout must be deterministic and FFI-stable only internally to language runtime, not exposed to C runtime.

## 3.2 Suspension/Resume Protocol

Protocol:

1. language future polls
2. if not ready, binding submits runtime op with completion trampoline
3. trampoline marks language future as ready and schedules continuation
4. scheduler polls future again and resumes state machine

No direct resume from C thread into language frame without scheduler mediation.

## 3.3 Scheduler Interaction Model

Binding layer provides a scheduler adapter:

- map language tasks -> runtime tasks or ops
- maintain cooperative poll budget per tick
- preserve fairness and deadlines

Two execution classes:

- I/O wait tasks (event-loop driven)
- compute tasks (runtime scheduler queues)

## 3.4 Cancellation Propagation

Language cancellation token maps to runtime cancellation handle.

Requirements:

- cancellation idempotent
- visible at await boundaries
- cancellation reason preserved
- sibling task cancellation under structured concurrency scopes

## 3.5 Backpressure Handling

Backpressure is represented in language API as explicit pending states or awaitable credits.

Examples:

- write returns `Pending` when socket credits exhausted
- spawn may return `QueueFull` under bounded scheduler queues
- streams expose async `reserve()` or `ready_to_send()` APIs

Do not hide backpressure with unbounded buffering.

## 3.6 Stackless vs Stackful Coroutines

### Stackless

Pros:

- compact memory footprint
- explicit suspension points
- better predictability for FFI boundaries
- easier integration with C callback completion model

Cons:

- compiler complexity
- larger generated state machine code in some patterns

### Stackful

Pros:

- simpler source-level semantics for blocking-like code
- easier porting of legacy style code

Cons:

- larger per-task memory
- complex unwind/panic integration across FFI
- weaker visibility of suspension points

Preferred for this integration: **stackless language coroutines + C runtime task/operation handles**.

## 3.7 Green Thread vs Async Task Mapping

- green threads may be offered as language-level feature layered over async tasks
- runtime binding should map directly to async task model for deterministic overhead

Green-thread M:N scheduling can be a higher layer, not part of ABI contract.

---

## 4. Task and Scheduler Mapping Strategy

## 4.1 Language Task Abstraction

Expose language task as typed handle:

- `Task<T>`
- `JoinHandle<T>`
- cancellation token
- deadline field
- priority hint

Internally maps to `CpxTaskHandle*` + typed result slot.

## 4.2 Queue Mapping

Runtime queue classes:

- local work queues per worker
- global injector queue
- optional high-priority lane

Language scheduler adapter sets task metadata:

- priority class
- affinity/NUMA hint
- deadline timestamp
- cancellation handle

## 4.3 Structured Concurrency

Binding should enforce parent-child task scopes:

- parent completion waits for children (unless detached)
- cancellation cascades downward
- resource cleanup tied to scope exit

Use scope frame IDs for bookkeeping across FFI.

## 4.4 Priority Scheduling

Language priorities map to runtime bands:

- critical
- high
- normal
- low

Mapping must be saturating and monotonic.

## 4.5 NUMA-Aware Placement

Expose optional affinity hints:

- none
- NUMA node
- CPU mask
- resource key (e.g., socket shard)

Runtime retains final authority; hints are best-effort.

---

## 5. Networking Binding Design

## 5.1 Language API Shape

Provide thin async wrappers:

- `TcpListener::accept().await`
- `TcpStream::read(&mut [u8]).await`
- `TcpStream::write(&[u8]).await`
- `UdpSocket::recv_from().await`
- `Timer::sleep(deadline).await`

These wrappers should compile to direct ABI submissions with minimal adapter logic.

## 5.2 Streaming I/O Model

- split read/write halves optional
- full-duplex stream operations use operation handles
- cancellation-aware in-flight ops
- explicit partial-read/partial-write semantics preserved

## 5.3 Zero-Copy Buffer Passing

Support borrowed and owned buffers:

- borrowed immutable slices for writes
- mutable borrowed slices for reads only if pinning/lifetime rules are met
- owned runtime buffers for long-lived operations

Pinned buffer descriptors prevent move/GC relocation during in-flight I/O.

## 5.4 Event Loop Integration

Language runtime does not run its own independent kernel poller when using Casprix runtime.

Instead:

- language scheduler tick integrates with runtime completion polling
- runtime drives completion; language drives continuation execution
- optional cooperative tick budget prevents starvation

---

## 6. Memory Interaction Model

## 6.1 Allocation Delegation Strategy

Use split allocation policy:

- language allocator for language objects and frames
- runtime pools for I/O buffers, task records, and operation state

Bridge allocators only via explicit APIs; avoid hidden cross-free patterns.

## 6.2 Lifetime Guarantees

Boundary rules:

- borrowed pointers valid only for documented scope
- async-crossing data must be owned or pinned
- runtime never stores unmanaged references to movable language objects

## 6.3 Pinning and Borrowing Across FFI

For languages with moving GC or compacting heaps:

- use pin scopes for borrowed buffers
- fallback to copy when pinning unavailable or too expensive

For ownership/borrowing languages:

- encode `Borrowed<T>` and `Owned<T>` wrappers in FFI layer
- static checks prevent use-after-free around await points

## 6.4 Pool Reuse and Safety

Runtime pool memory returned to language only through typed wrappers carrying owner tags.

Debug mode can poison and generation-tag freed blocks to catch stale handles.

---

## 7. Error and Panic Propagation Design

## 7.1 Runtime Errors -> Language Errors

Map C status codes to language `Result<T, E>` (or equivalent typed error).

Error classes:

- `WouldBlock`
- `Timeout`
- `Cancelled`
- `QueueFull`
- `UnsupportedFeature`
- `InternalRuntimeError`

## 7.2 Cancellation Errors

Cancellation remains distinguishable from timeout and transport failures.

Language APIs should expose cancellation as first-class outcome, not generic failure.

## 7.3 Panic Safety Across FFI

Hard rule: no panic/exception unwinds across C boundary.

Language-to-C calls wrapped in panic guard:

- panic converted to failure status
- task marked failed and cancellation propagated
- optional process-abort policy for unrecoverable runtime invariants

C-to-language callbacks wrapped similarly; callback trampolines catch and translate panics.

---

## 8. Performance Tradeoff Analysis

## 8.1 FFI Overhead Mitigation

- batch syscalls and operation submissions
- use function table pointers cached in thread-local context
- avoid per-call dynamic symbol resolution
- favor POD descriptors over dynamic marshaling

## 8.2 Inline Fast Paths

Language wrappers should include fast paths for:

- immediate-ready completions
- short non-blocking writes
- timer wheel immediate expiration checks

Fallback path defers to full submit/resume machinery.

## 8.3 Coroutine Scheduling Cost

Cost components:

- state machine poll
- queue push/pop
- completion dispatch
- cache misses in task metadata

Mitigation:

- compact task headers
- per-core queues
- minimized cross-core steals
- deadline/priority metadata packed into hot cache lines

## 8.4 Cache Friendliness

- avoid pointer-rich linked structures in hot paths
- use bounded ring buffers
- separate hot/cold fields in task and operation records
- align queue indices/counters to cache line boundaries

## 8.5 Syscall Batching Strategy

For high-load I/O:

- batch submit operations
- batch completion drains per loop tick
- coalesce timers
- prefer runtime APIs that expose vectorized submit/drain

---

## 9. Evolution Strategy

## 9.1 Versioned ABI

ABI policy:

- major bump for incompatible changes
- minor bump for additive changes
- keep old major loader compatibility shim where feasible

## 9.2 Capability Negotiation

At startup, binding queries:

- ABI version
- feature bitset
- optional extension tables

Language runtime enables/disables features based on capabilities, avoiding hard compile-time assumptions.

## 9.3 Extension / Plugin System

Reserve extension registration points:

- custom scheduler hints
- protocol plugins
- metrics exporters
- custom allocators

Extensions are capability-gated and namespaced to avoid ABI collisions.

---

## 10. Backend Framework Enablement

This binding architecture directly supports building:

- async web servers
- middleware stacks
- RPC frameworks (HTTP/2, gRPC)
- event-driven background job schedulers
- high-throughput service meshes and gateways

Because the boundary is stable and low-level, framework teams can innovate at language level while the C runtime evolves independently.

---

## 11. Implementation Roadmap

## Phase 1 - ABI Foundation

- define `CpxRuntimeApi` function table
- implement version and feature negotiation
- create opaque handle registry and safety checks

## Phase 2 - Core Async Binding

- implement task spawn/join/cancel mapping
- implement operation submission + completion trampolines
- add cancellation token propagation

## Phase 3 - Networking and Timer APIs

- map sockets/listeners/streaming I/O
- implement zero-copy buffer wrappers and pinning contracts
- integrate timer APIs with async await points

## Phase 4 - Structured Concurrency and Priorities

- task scopes
- parent-child cancellation
- deadline/priority mapping
- NUMA hint APIs

## Phase 5 - Performance Hardening

- benchmark FFI micro-costs
- add vectorized submit/drain paths
- tighten memory layouts for cache
- add profiling counters per boundary crossing

## Phase 6 - Ecosystem Enablement

- framework primitives (HTTP server, middleware, client pool)
- plugin API draft
- long-term ABI compatibility test suite

---

## 12. Contract Summary

Non-negotiable contracts:

- stable C ABI with explicit versioning
- no panic unwind across boundary
- no hidden ownership transfer
- no use of caller stack memory across suspension
- cancellation and deadline semantics preserved end-to-end

These constraints provide future-proof, low-overhead integration suitable for systems-language compilers and production backend frameworks.
