# ADR-0118: Workspace symbols cover the open documents

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-29

## Context

A quick-open request asks for symbols by name across the workspace.
The language server holds the documents the client opened and nothing
else: it has no workspace root, no file watcher, and no module index of
its own.

## Decision

- workspace/symbol searches the documents the client has open, pairing
  each with documentSymbols, the same parse-only outline the document
  request answers.
- A symbol is reported as SymbolInformation: its name, its outline
  kind, the document it came from, its range, and the container path of
  its parents.
- The query is a case-insensitive substring; an empty query matches
  every symbol.

## Consequences

Quick open reflects the buffers in hand, without a filesystem walk the
server cannot do correctly on its own, and it inherits the outline's
tolerance of a buffer that does not analyze. A workspace index over
files the client never opened is a later record; it needs a root and a
watcher this server does not yet have.
