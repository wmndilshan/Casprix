# net2 — archived experimental async networking runtime

**Status: archived, not built, not maintained. Do not depend on this code.**

## What it was

`net2/` was an experimental rewrite of the Casprix networking runtime built
around:

- a **reactor** event loop (`cx_runtime.c`) driving epoll/io_uring,
- **stackful coroutines** (`coro_posix2.c`) as the unit of async work,
- a **thread-pool + scheduler-bridge** hand-off (`cx_task.c`, `cx_worker.c`,
  `cx_task_arc.c`) so suspended coroutine tasks could be resumed on any worker,
- ARC-managed `Future`s (`cx_future_create_arc`) for await points,
- a thin socket/IO shim (`cx_io.c`).

The goal was a higher-throughput, lower-copy successor to `runtime/net/` with
Rust-style `spawn` / `await` ergonomics (`cx_runtime_spawn`,
`cx_runtime_await`).

## Why it was archived

1. **Zero consumers.** Nothing in the repo links `cx_net2` or references
   `cx_runtime` / `cx_task` / `cx_worker` outside this directory. It only ever
   built its own `test_net2` harness.
2. **Unfinished protocol support.** No HTTP server/client, no WebSocket, no DNS —
   `runtime/net/` has all of these. Reaching parity was estimated at weeks.
3. **Known build break.** `cx_runtime.c`'s `bridge_thread_fn()` calls
   `cx_task_execute_trampoline` before it is declared (it is defined lower in the
   same file and forward-declared only inside `cx_task.c`), so `cx_net2` fails to
   compile with `-Werror`-adjacent settings:
   `error: 'cx_task_execute_trampoline' undeclared`. `usleep` is also used
   without `<unistd.h>`. **Not fixed** — archived as-is.
4. **Double-wake race in the scheduler.** `cx_task_waker_cb()` (`cx_task.c`)
   pushes a task onto the ready bridge *and* futex-wakes the pool, while
   `bridge_thread_fn()` (`cx_runtime.c`) independently polls the same bridge and
   submits ready tasks to the pool. A task whose future completes at nearly the
   same moment its coroutine finishes can be submitted to the thread pool twice
   (once by the waker callback, once by the bridge poller), leading to a
   use-after-complete on the `CxTask` / double `future_complete`. There is no
   synchronization claiming ownership of a ready task between those two paths.
   **Not fixed** — archived as-is.

## What to use instead

**`runtime/net/` is the actively-maintained networking implementation.** It has
sockets, HTTP client + server, WebSocket, DNS, a thread-pooled connection
handler, and (as of the accompanying change) socket read/write timeouts,
Content-Length-aware body reading, and partial-send/recv retry loops.

## Restoring this for reference

The original build wiring is preserved, commented out, in:

- `CMakeLists.txt` (repo root) — `add_subdirectory(runtime/net2)`
- `runtime/CMakeLists.txt` — `add_subdirectory(net2)`
- `archive/net2/CMakeLists.txt` — the `cx_net2` / `test_net2` targets

Re-pointing those at `archive/net2` will resurrect the targets, but expect to
fix build breaks (3) and the race (4) first.
