# lsp

The language server: `alcy lsp` speaks LSP over standard input and
output. The module is a library over the toolchain rather than a stage
of the compiler (ADR-0083), so it owns no analysis of its own:

- `frame.{h,cc}` is the base protocol: a `Content-Length` header and
  a JSON-RPC payload per frame.
- `transport.{h,cc}` reads and writes those frames over a descriptor
  pair and buffers the reads.
- `document.{h,cc}` holds the open buffers, applies incremental
  changes in protocol order, and maps LSP positions to byte offsets.
- `diagnostics.{h,cc}` runs `linter::lint_source` over one buffer and
  renders the bag as LSP diagnostics, so a compiler error and a lint
  finding arrive from the same pass.
- `formatting.{h,cc}` returns the `alcy_fmt` edit that canonicalizes a
  buffer, or none when the buffer already has that layout.
- `server.{h,cc}` routes the methods and owns the session lifecycle;
  `lsp.{h,cc}` is the entry point the cli calls.

The unit suite drives framing, position mapping, and diagnostics
directly. `tools/check_lsp.py` runs a session against the built binary
and asserts the handshake, a published diagnostic, and a formatting
edit, which is the gate CI asks.

## Notes

Standard output is protocol only. A debug message belongs on standard
error, and the server writes none today.

Positions are zero-based and counted in UTF-16 code units, as the
protocol requires, while the compiler reports byte offsets.
`PositionIndex` is the single translation point, rebuilt per change
rather than incrementally. The buffer is small next to the frontend run
each change triggers, so a line-start table built with one scan is the
simple side of that trade; a future incremental index belongs behind
the same interface.

Diagnostics run the linter's single-source entry, which runs the
frontend the rules need and then the rules. Caching per-document
analysis is future work behind the same request path, not a second
analysis path.
