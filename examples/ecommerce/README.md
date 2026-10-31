# Casprix Store backend

This example uses the shared `runtime/net` HTTP server and its synchronous route
API. It keeps inventory, cart and order updates behind one mutex so concurrent
requests cannot oversell stock or corrupt cart state.

Endpoints:

- `GET /health`
- `GET /api/products`
- `GET /api/cart`
- `POST /api/cart?product=1&quantity=2`
- `POST /api/orders`

The Casprix launcher is `backend.cpx`; `backend.c` is the native HTTP/FFI bridge.
For a standalone runtime build, compile `backend.c` with
`CPX_ECOMMERCE_STANDALONE` defined.

The integration test exercises the full cart/order flow and malformed HTTP
framing. The load benchmark uses `GET /api/products` and reports accepted,
completed, rejected, malformed and still-active connection counts so dropped
work cannot be mistaken for throughput.
