# ADR-0127: A repeated pull skips an unchanged buffer

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-29

## Context

A polled client asks one document for its diagnostics on a timer or
after an edit. When the buffer has not changed since the answer it
holds, running the linter again costs a frontend pass for an answer the
client already has. The protocol lets the server answer "unchanged".

## Decision

- The server remembers the last pull result id and the document version
  it answered for.
- A pull whose previous result id is the remembered one, for the same
  version, answers kind "unchanged" without running the linter.
- Every other pull runs the linter, answers a full report, and stores
  the new id and version. Closing a document forgets its entry.

## Consequences

A polled client pays for a change, not for a poll. The version is the
client's own edit counter, so a buffer edited in place without a version
bump would be answered unchanged; clients bump the version on every
change, which the protocol requires.
