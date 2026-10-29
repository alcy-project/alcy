# ADR-0122: Workspace diagnostics report every open document

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-29

## Context

The pull model lets a client ask one document for its diagnostics, and
the workspace variant asks for them all. The language server already
answers one document's full report; the workspace request is the same
report for each open buffer.

## Decision

- workspace/diagnostic returns one full report per open document, each
  tagged with its URI and version, reusing the document report verbatim.
- The diagnostic provider advertises workspaceDiagnostics, so a client
  knows the request is answered.

## Consequences

A client that pulls sees the same diagnostics the push model publishes,
for every buffer it holds, with no second checker. The report is built
per request, so it reflects the versions the client opened at the time
it asks.
