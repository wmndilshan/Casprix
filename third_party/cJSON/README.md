# cJSON (vendored)

Ultralightweight JSON parser in ANSI C — https://github.com/DaveGamble/cJSON

- Version: 1.7.18
- License: MIT (see cJSON.h header)
- Files: cJSON.c, cJSON.h (unmodified upstream)

Vendored solely for the Casprix LSP server (tools/casprix-lsp/) to parse
*incoming* JSON-RPC requests. Outgoing JSON-RPC is hand-formatted.
