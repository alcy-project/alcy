# ADR-0107: The language server is a library over the toolchain

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-28

## Context

Editors want diagnostics, formatting, the memory layout of a type, and
the facts the toolchain already computes as a buffer is typed. The frontend, the linter, and
the formatter are separate modules; a language server that re-read the
source for itself would answer from a second parser, which is the
failure mode every editor tool that grew its own has had. The server
also shares a process with the compiler, so standard output carries
protocol frames and nothing else.

## Decision

- `src/lsp/` is a module of the toolchain tree, not a stage of the
  compiler. It depends on `//src:alcy_lib`, `//src/linter`, and
  `//src/fmt:alcy_fmt`; the cli reaches it through `alcy lsp`, and no
  compiler target depends on it - the dependency direction is the
  boundary, and gn check holds it. The include spelling is `lsp/...`
  and the namespace is `lsp`.
- The transport is the LSP base protocol over a descriptor pair:
  a `Content-Length` header and a JSON-RPC payload per frame. The JSON
  value is `text::JsonValue`, whose reader and writer live in the
  shared `text` module so a second JSON implementation does not appear
  beside the one the compiler already prints with. Reads are buffered
  and writes are framed; no request handler writes to standard output.
- Diagnostics are the bag one `linter::lint_source` call fills, so the
  compiler's errors and the lint findings publish from one pass over the
  buffer rather than two that could disagree. A diagnostic's byte span
  becomes an LSP range through a per-document line index that counts
  UTF-16 code units; that index is the single place the protocol's
  position model meets the compiler's byte offsets.
- Formatting returns the edit that replaces the buffer with
  `alcy_fmt::format`'s canonical layout, and no edit when the buffer
  already has it. The open buffer is the source of truth: the server
  requests incremental changes and never reads the filesystem.
- A struct's memory layout is `ir::type_layout` and `ir::field_offset`
  over the checked package, not a second layout model. Hover renders it
  as markdown when the cursor is on a struct declaration name, and the
  `alcy/structLayout` request returns the same numbers as JSON for a
  client or another tool. A generic declaration has no concrete offsets,
  so the answer says so instead of guessing.
- The server is single-threaded over one client. `initialize`,
  `initialized`, `shutdown`, and `exit` follow the lifecycle the
  protocol defines; a request before initialize or after shutdown is
  refused with the protocol's error, so a client cannot get an answer
  from a server that is not there.

## Consequences

The server gets the frontend, the linter's rules, the formatter, the
JSON value, and the diagnostic model for free; a new feature is a
request handler over data a toolchain module already produces. `text`
grew the JSON reader its architecture entry already names. The session
holds only the open documents, so a restart costs one re-read of the
buffer.

Out of scope for this record: hover over expressions, completion,
document and workspace symbols, definition, incremental or background
analysis, a workspace index over unopened files, position encodings
other than UTF-16, and multi-root workspaces.
