# ADR-0125: A semantic token delta answers the edit since the last result

- Subject: the compiler
- Status: Accepted
- Date: 2026-10-29

## Context

A client that keeps the token array from a full request need not
receive the whole array after an edit. The protocol lets it send the
result id it holds and receive only the edit, which keeps a keystroke's
answer proportional to what changed.

## Decision

- The token walk returns the flat u32 array, and the JSON is built from
  it, so the same array feeds the full, range, and delta answers.
- The server remembers the last array per document with the result id
  it handed out. A delta request compares the client's id with the
  remembered one.
- A match answers one edit: the common prefix and suffix, each rounded
  to a token boundary, are kept and the middle is replaced. A mismatch
  answers the whole array with a fresh id, which the protocol allows.
- Closing a document forgets its array.

## Consequences

The answer stays small when few tokens change, and always correct when
the client is behind: it falls back to the whole array rather than
guessing. Edits are cut on token boundaries because an edit that split
a token would reinterpret the following tokens.
