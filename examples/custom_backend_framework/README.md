# Custom Backend Framework Example

This folder shows what a small backend framework can look like in Casprix today.
It is built on top of the existing socket wrapper in `lib/net/Socket.cpx` and keeps the framework layer inside Casprix code.

## What it demonstrates

- A small `BackendApp` class that owns the TCP server
- `Request` and `Response` classes for HTTP-like handling
- String-based route registration with `get()` and `post()`
- A route dispatcher that maps request paths to handler names
- A single-request server loop with `serveOnce()`

## Why it is shaped this way

Casprix already has real C runtime networking under `runtime/net/`, but the higher-level Casprix-side backend abstractions are still incomplete.
This example stays inside current language/runtime constraints and shows a practical direction for a framework layer.

## File

- `mini_backend_framework.cpx`: self-contained micro-framework demo

## Build

This example currently needs a small compatibility shim because `lib/net/Socket.cpx` expects legacy `nuwan_socket_*` symbols while the main runtime archive in this tree is built around the newer socket API.
The build sequence below was verified on Linux in this repository.

```bash
build/casprix examples/custom_backend_framework/mini_backend_framework.cpx -o build/custom_backend_framework || true
```

The direct compiler link step currently fails, but it still emits `build/custom_backend_framework.asm`.
Assemble and link it explicitly:

```bash
nasm -f elf64 build/custom_backend_framework.asm -o build/custom_backend_framework_manual.o
gcc -c examples/custom_backend_framework/socket_compat.c -o build/custom_backend_framework_socket_compat.o
gcc -no-pie \
  build/custom_backend_framework_manual.o \
  build/custom_backend_framework_socket_compat.o \
  build/libcasprix_runtime.a \
  -o build/custom_backend_framework \
  -lpthread -lm
```

## Run

```bash
./build/custom_backend_framework
```

Then connect with a raw client or `curl`-style request to port `8081`.

Example requests:

```text
GET / HTTP/1.1

GET /health HTTP/1.1

GET /users HTTP/1.1

POST /users HTTP/1.1
```

## Notes

- The example is intentionally small and uses a fixed-size route table.
- Request parsing is simple and string-based.
- It handles one connection at a time to keep the framework structure easy to read.
- `socket_compat.c` is example-local glue so this demo can link on the current repository state.
